// SPDX-License-Identifier: LGPL-2.1-only
/// @file windowscodecs_dll.cpp
/// @brief windowscodecs.dll: the Windows Imaging Component system DLL.
///
/// On Windows WIC is windowscodecs.dll in System32 (and SysWOW64); installers
/// look for that file and its version (the .NET Framework 4 MSI's launch
/// condition), and explicit linkers bind its exports. The coclasses
/// themselves are implemented in wic.cpp; this is the module surface.
///
/// Export set: mingw-w64 libwindowscodecs.a (both bitnesses) plus the in-proc
/// server entry points every COM server DLL has.
///   DllGetClassObject / DllCanUnloadNow  (combaseapi.h)
///   *_Proxy  "Proxy function for the <Method> method" (Learn, WIC codec
///            reference): THIS_PTR, then the method's own parameters; the
///            call goes through the object's vtable. Slots, argument counts
///            and x86 frame sizes are generated into windowscodecs_proxies.inc
///            by tools/gen_windowscodecs_proxies.py from the SDK.
///   WICCreateImagingFactory_Proxy, WICConvertBitmapSource: implemented.
///   WICSetEncoderFormat_Proxy, WICCreateBitmapFromSection(Ex), the metadata
///   name/content functions: fail loudly (no encoders, IWICBitmap or
///   metadata handlers exist in this WIC tier).

#include "wic.hpp"

#include "winapi/core/marshaler.hpp"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kSOk = 0;
constexpr u32 kSFalse = 1;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kEInvalidArg = 0x80070057u;

constexpr Guid kIidImagingFactory{
    0xEC5EC8A9u, 0xC395u, 0x4314u, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};

void log_unimplemented(const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (!seen.insert(api).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED windowscodecs!%s: %s\n", api, what);
}

struct ProxyRow {
    const char* name;
    u16 slot;       ///< vtable slot of the method
    u16 x64_argc;   ///< parameters including THIS_PTR
    u16 x86_bytes;  ///< stdcall frame (import-lib @N)
};

constexpr ProxyRow kProxies[] = {
#include "windowscodecs_proxies.inc"
};

/// Call method @p row.slot on THIS_PTR with the proxy's own arguments.
/// x86: every stack dword is forwarded verbatim (doubles included).
/// x64: integer slots are forwarded; a double in an argument register lives
/// in the XMM register the caller loaded, which the guest call leaves intact
/// (run_guest_callback saves/restores but never writes SSE state on entry).
u32 forward_proxy(ComCoreManager& mgr, Marshaler& m, const ProxyRow& row) {
    const bool is64 = m.cpu_is_64bit();
    const usize argc = is64 ? row.x64_argc : row.x86_bytes / 4u;
    std::vector<u64> args(argc);
    for (usize i = 0; i < argc; ++i) {
        args[i] = is64 ? m.arg(i) : static_cast<u32>(m.arg(i));
    }
    const GuestAddr self = argc > 0 ? static_cast<GuestAddr>(args[0]) : 0;
    if (self == 0) return kEPointer;
    const u32 ptr = is64 ? 8u : 4u;
    const GuestAddr vtable = static_cast<GuestAddr>(m.read_ptr(self).value_or(0));
    const GuestAddr fn = vtable == 0 ? 0
        : static_cast<GuestAddr>(m.read_ptr(vtable + row.slot * ptr).value_or(0));
    if (fn == 0) return kEPointer;
    if (!mgr.guest_call()) {
        log_unimplemented(row.name, "no guest caller installed");
        return kENotImpl;
    }
    return static_cast<u32>(mgr.guest_call()(fn, args.data(), args.size(), is64));
}

void add_failing(DllExports& exports, const char* name, u16 bytes,
                 const char* what) {
    const usize argc = bytes / 4u;
    exports.entries.push_back(ExportEntry{
        name, 0,
        [name, what, argc](Marshaler& m) {
            for (usize i = 0; i < argc; ++i) (void)m.arg(i);
            log_unimplemented(name, what);
            m.set_return(kENotImpl);
        },
        bytes});
}

} // namespace

DllExports create_windowscodecs_thunks(ComCoreManager& mgr) {
    DllExports exports;
    const auto add = [&](const char* name, u16 bytes,
                         u32 (*fn)(ComCoreManager&, Marshaler&)) {
        exports.entries.push_back(ExportEntry{
            name, 0, [&mgr, fn](Marshaler& m) { m.set_return(fn(mgr, m)); },
            bytes});
    };

    add("DllGetClassObject", 12, wic_dll_get_class_object);
    // Objects handed out stay live for the process; the module never unloads.
    exports.entries.push_back(ExportEntry{
        "DllCanUnloadNow", 0, [](Marshaler& m) { m.set_return(kSFalse); }, 0});

    add("WICConvertBitmapSource", 12, wic_convert_bitmap_source);

    // HRESULT WICCreateImagingFactory_Proxy(UINT SDKVersion,
    //     IWICImagingFactory** ppIImagingFactory)
    exports.entries.push_back(ExportEntry{
        "WICCreateImagingFactory_Proxy", 0,
        [&mgr](Marshaler& m) {
            (void)m.arg(0);
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) { m.set_return(kEInvalidArg); return; }
            m.set_return(wic_co_create_instance(mgr, m, kIidImagingFactory, 0, out));
        },
        8});

    add_failing(exports, "WICSetEncoderFormat_Proxy", 16,
                "no WIC encoders are implemented");
    add_failing(exports, "WICCreateBitmapFromSection", 28,
                "IWICBitmap is not implemented");
    add_failing(exports, "WICCreateBitmapFromSectionEx", 32,
                "IWICBitmap is not implemented");
    add_failing(exports, "WICMapGuidToShortName", 16,
                "metadata handler name table is not implemented");
    add_failing(exports, "WICMapShortNameToGuid", 8,
                "metadata handler name table is not implemented");
    add_failing(exports, "WICMapSchemaToName", 20,
                "metadata schema name table is not implemented");
    add_failing(exports, "WICGetMetadataContentSize", 12,
                "metadata handlers are not implemented");
    add_failing(exports, "WICMatchMetadataContent", 16,
                "metadata handlers are not implemented");
    add_failing(exports, "WICSerializeMetadataContent", 16,
                "metadata handlers are not implemented");

    for (const ProxyRow& row : kProxies) {
        exports.entries.push_back(ExportEntry{
            row.name, 0,
            [&mgr, &row](Marshaler& m) { m.set_return(forward_proxy(mgr, m, row)); },
            row.x86_bytes});
    }
    return exports;
}

} // namespace vitreo::winapi
