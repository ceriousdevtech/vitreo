// SPDX-License-Identifier: LGPL-2.1-only

/// @file msftedit.cpp
/// @brief Msftedit.dll exports. See msftedit.hpp for the contract sources.

#include "msftedit.hpp"

#include "core/pe/os_version.hpp"
#include "winapi/core/marshaler.hpp"

#include <atomic>
#include <cstdio>

namespace vitreo::winapi {

namespace {

constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kEFail = 0x80004005u;
constexpr u32 kClassEClassNotAvailable = 0x80040111u;

constexpr std::array<u8, 16> make_guid(u32 d1, u16 d2, u16 d3, u64 d4) {
    std::array<u8, 16> g{};
    for (int i = 0; i < 4; ++i) g[static_cast<usize>(i)] = static_cast<u8>(d1 >> (8 * i));
    for (int i = 0; i < 2; ++i) g[static_cast<usize>(4 + i)] = static_cast<u8>(d2 >> (8 * i));
    for (int i = 0; i < 2; ++i) g[static_cast<usize>(6 + i)] = static_cast<u8>(d3 >> (8 * i));
    for (int i = 0; i < 8; ++i) g[static_cast<usize>(8 + i)] = static_cast<u8>(d4 >> (56 - 8 * i));
    return g;
}

// msftedit_lifecycle_probe{32,64}.expected "iid" lines (identical per bitness).
constexpr std::array<MsfteditInterfaceId, 10> kInterfaceIds{{
    {"IID_IRichEditOle", 2, make_guid(0x00020d00, 0x0000, 0x0000, 0xc000000000000046ull)},
    {"IID_IRichEditOleCallback", 3, make_guid(0x00020d03, 0x0000, 0x0000, 0xc000000000000046ull)},
    {"IID_ITextServices", 5, make_guid(0x8d33f740, 0xcf58, 0x11ce, 0xa89d00aa006cadc5ull)},
    {"IID_ITextHost", 6, make_guid(0x13e670f4, 0x1a5a, 0x11cf, 0xabeb00aa00b65ea1ull)},
    {"IID_ITextHost2", 7, make_guid(0x13e670f5, 0x1a5a, 0x11cf, 0xabeb00aa00b65ea1ull)},
    {"IID_ITextDocument2", 24, make_guid(0xc241f5e0, 0x7206, 0x11d8, 0xa2c700a0d1d6c6b3ull)},
    {"IID_ITextServices2", 25, make_guid(0x8d33f741, 0xcf58, 0x11ce, 0xa89d00aa006cadc5ull)},
    {"IID_IRicheditWindowlessAccessibility", 32,
     make_guid(0x983e572d, 0x20cd, 0x460b, 0x910483111592dd10ull)},
    {"IID_IRicheditUiaOverrides", 33, make_guid(0xf2fb5cc0, 0xb5a9, 0x437f, 0x9ba247632082269full)},
    {"IID_IRicheditUiaNotificationOverrides", 37,
     make_guid(0xa6bf39f8, 0x11de, 0x48ea, 0x87809950bc13949dull)},
}};

void report_unimplemented_once(std::atomic_flag& flag, const char* api, const char* what) {
    if (!flag.test_and_set()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msftedit!%s: %s\n", api, what);
    }
}

// HRESULT CreateTextServices(IUnknown* punkOuter, ITextHost* host, IUnknown** ppUnk)
// Learn: pITextHost "must not be NULL"; failures are E_INVALIDARG,
// E_OUTOFMEMORY or E_FAIL ("could not be initialized").
void CreateTextServices(Marshaler& m) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    const GuestAddr host = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (host == 0 || out == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    // COM: an [out] interface pointer is NULL when the call fails.
    (void)m.write_ptr(out, 0);
    report_unimplemented_once(reported, "CreateTextServices",
                              "windowless text services object (ITextServices/TOM)");
    m.set_return(kEFail);
}

// HRESULT ShutdownTextServices(IUnknown* pTextServices)
// Learn: E_INVALIDARG when pTextServices is not an instance CreateTextServices
// made -- and none exists, since CreateTextServices never succeeds here.
void ShutdownTextServices(Marshaler& m) {
    (void)m.arg_ptr(0);
    m.set_return(kEInvalidArg);
}

// HRESULT DllGetVersion(DLLVERSIONINFO*). Measured: only cbSize ==
// sizeof(DLLVERSIONINFO) (20) is accepted -- DLLVERSIONINFO2, 0, 19 and a
// NULL pointer all fail with E_INVALIDARG and leave the structure untouched.
// The version is the system's (10.0.<build>), platform DLLVER_PLATFORM_WINDOWS.
void DllGetVersion(Marshaler& m) {
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    auto cb = m.read_u32(info);
    if (cb.is_err() || cb.value() != 20) {
        m.set_return(kEInvalidArg);
        return;
    }
    (void)m.write_u32(info + 4, pe::kOsMajorVersion);
    (void)m.write_u32(info + 8, pe::kOsMinorVersion);
    (void)m.write_u32(info + 12, pe::kOsBuildNumber);
    (void)m.write_u32(info + 16, 1);  // DLLVER_PLATFORM_WINDOWS
    m.set_return(kSOk);
}

// HRESULT DllGetActivationFactory(HSTRING activatableClassId, IActivationFactory**)
void DllGetActivationFactory(Marshaler& m) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    (void)m.write_ptr(out, 0);
    report_unimplemented_once(reported, "DllGetActivationFactory",
                              "Windows Runtime activation factories");
    m.set_return(kClassEClassNotAvailable);
}

// Undocumented exports with no SDK prototype. Their argument bytes are
// measured; their behaviour is not modelled, so each reports itself and
// fails with E_NOTIMPL.
void undocumented(Marshaler& m, std::atomic_flag& flag, const char* api) {
    report_unimplemented_once(flag, api, "undocumented export not modelled");
    m.set_return(kENotImpl);
}

} // namespace

std::span<const MsfteditInterfaceId> msftedit_interface_ids() { return kInterfaceIds; }

DllExports create_msftedit_thunks(bool guest_is_64bit, MsfteditServices services) {
    DllExports exports;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes, ApiHandler handler) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(handler), stdcall_bytes});
    };
    auto control = services.control_proc;

    // Window procedures: WNDPROC is CALLBACK, 16 argument bytes on x86.
    add("RichEditWndProc", 13, 16, [control](Marshaler& m) { control(m, false); });
    add("RichEditANSIWndProc", 9, 16, [control](Marshaler& m) { control(m, true); });
    add("RichEdit10ANSIWndProc", 10, 16, [control](Marshaler& m) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        report_unimplemented_once(reported, "RichEdit10ANSIWndProc",
                                  "Rich Edit 1.0 emulation differences (ANSI 4.1 behaviour used)");
        control(m, true);
    });

    add("CreateTextServices", 4, 12, CreateTextServices);
    add("ShutdownTextServices", 35, 4, ShutdownTextServices);
    add("DllGetVersion", 12, 4, DllGetVersion);
    add("DllGetActivationFactory", 26, 8, DllGetActivationFactory);
    // void DisableOleinitCheck(void): turns off the control's check that OLE
    // is initialised. Vitreo's control has no such check, so there is nothing
    // to disable. The x86 export keeps its stdcall decoration.
    add(guest_is_64bit ? "DisableOleinitCheck" : "_DisableOleinitCheck@0", 8, 0,
        [](Marshaler&) {});

    struct Undocumented { const char* name; u16 ordinal; u16 bytes; };
    static constexpr Undocumented kUndocumented[] = {
        {"SetCustomTextOutHandlerEx", 11, 8},
        {"MathBuildUp", 17, 12},
        {"MathBuildDown", 18, 12},
        {"MathTranslate", 19, 8},
        {"GetMathAlphanumericCode", 21, 8},
        {"GetMathAlphanumeric", 22, 8},
        {"SetTextServicesDpiCalculationOverride", 36, 8},
    };
    for (const auto& u : kUndocumented) {
        auto flag = std::make_shared<std::atomic_flag>();
        const char* name = u.name;
        add(name, u.ordinal, u.bytes,
            [flag, name](Marshaler& m) { undocumented(m, *flag, name); });
    }

    exports.lifecycle.unloads_on_last_free = true;
    exports.lifecycle.process_attach =
        [register_class = services.register_class,
         export_address = services.export_address](GuestAddr base) {
            register_class(base, export_address("RichEditWndProc"));
        };
    exports.lifecycle.process_detach = services.unregister_class;
    return exports;
}

} // namespace vitreo::winapi
