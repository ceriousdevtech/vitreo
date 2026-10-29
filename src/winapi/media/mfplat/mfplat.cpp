#include "mfplat.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace vitreo::winapi {

namespace {

constexpr u32 kSOk = 0;
constexpr u32 kENotImpl = 0x80004001u;             // E_NOTIMPL
constexpr u32 kEPointer = 0x80004003u;             // E_POINTER
constexpr u32 kClassNotAvailable = 0x80040111u;    // CLASS_E_CLASSNOTAVAILABLE
constexpr u32 kSelfRegClass = 0x80040201u;         // SELFREG_E_CLASS
constexpr u32 kErrorCallNotImplemented = 120;      // ERROR_CALL_NOT_IMPLEMENTED

/// One `[vitreo] UNIMPLEMENTED <dll>!<api>: <case>` line per API per process.
struct LoudOnce {
    std::atomic<bool> fired{false};
    void operator()(const char* dll, std::string_view api, const char* why) {
        if (fired.exchange(true)) return;
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s!%.*s: %s\n", dll,
                     static_cast<int>(api.size()), api.data(), why);
    }
};

/// Media Foundation object factories Vitreo cannot build: E_NOTIMPL with the
/// [out] interface pointer (argument @p out_index) nulled so a caller that
/// ignores the HRESULT does not use stack garbage, and a one-time line.
ExportEntry factory_notimpl(const char* dll, const char* name, usize out_index,
                            u16 stdcall_bytes, const char* why) {
    auto once = std::make_shared<LoudOnce>();
    return ExportEntry{
        name, 0,
        [dll, name, out_index, why, once](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(out_index);
            if (out == 0) {
                m.set_return(kEPointer);
                return;
            }
            (void)m.write_ptr(out, 0);
            (*once)(dll, name, why);
            m.set_return(kENotImpl);
        },
        stdcall_bytes};
}

/// Return convention of a table export (see tools/gen_mf_export_table.py).
enum class ReturnKind { HRESULT, BOOL, PTR, VOID, I64 };

/// The documented failure value for an export Vitreo does not implement:
/// E_NOTIMPL for an HRESULT, FALSE + ERROR_CALL_NOT_IMPLEMENTED for a BOOL,
/// NULL for a pointer, 0 for a 64-bit integer, nothing for void. Never the
/// generic missing-export stub's 0xFFFFFFFF.
ExportEntry table_notimpl(const char* dll, const char* name, u16 stdcall_bytes,
                          ReturnKind kind) {
    auto once = std::make_shared<LoudOnce>();
    return ExportEntry{
        name, 0,
        [dll, name, kind, once](Marshaler& m) {
            (*once)(dll, name, "no Media Foundation pipeline");
            switch (kind) {
                case ReturnKind::HRESULT: m.set_return(kENotImpl); break;
                case ReturnKind::BOOL:
                    set_last_error(kErrorCallNotImplemented);
                    m.set_return(0);
                    break;
                case ReturnKind::PTR: m.set_return(0); break;
                case ReturnKind::VOID: break;
                case ReturnKind::I64: m.set_return_i64(0); break;
            }
        },
        stdcall_bytes};
}

/// Append a loud thunk for every SDK import-library export of @p dll that is
/// not already registered (the implemented ones come first).
void add_table_exports(DllExports& exports, const char* dll) {
    // Copies, not views: pushing entries below reallocates the vector and
    // would leave string_views into the moved names dangling (which made
    // every real export register a second, loud twin that then won).
    std::unordered_set<std::string> present;
    for (const auto& e : exports.entries) present.insert(e.name);
    struct Row { const char* dll; const char* name; u16 bytes; ReturnKind kind; };
#define HRESULT ReturnKind::HRESULT
#define BOOL ReturnKind::BOOL
#define PTR ReturnKind::PTR
#define VOID ReturnKind::VOID
#define I64 ReturnKind::I64
#define MF_EXPORT(d, n, b, k) Row{d, n, static_cast<u16>(b), k},
    static constexpr Row kTable[] = {
#include "winapi/media/mfplat/mf_export_table.inc"
    };
#undef MF_EXPORT
#undef HRESULT
#undef BOOL
#undef PTR
#undef VOID
#undef I64
    for (const Row& row : kTable) {
        if (std::string_view(row.dll) != dll) continue;
        if (present.count(row.name) != 0) continue;
        exports.entries.push_back(table_notimpl(row.dll, row.name, row.bytes, row.kind));
    }
}

/// A COM server DLL's standard exports when the server has no classes to
/// offer: DllCanUnloadNow S_OK (nothing is alive), DllGetClassObject
/// CLASS_E_CLASSNOTAVAILABLE with *ppv nulled, Dll(Un)RegisterServer
/// SELFREG_E_CLASS (documented: the class could not be registered).
void add_com_server_exports(DllExports& exports, const char* dll, bool self_registering) {
    exports.entries.push_back(ExportEntry{
        "DllCanUnloadNow", 0, [](Marshaler& m) { m.set_return(kSOk); }, 0});
    auto once = std::make_shared<LoudOnce>();
    exports.entries.push_back(ExportEntry{
        "DllGetClassObject", 0,
        [dll, once](Marshaler& m) {
            if (const GuestAddr ppv = m.arg_ptr(2); ppv != 0) (void)m.write_ptr(ppv, 0);
            (*once)(dll, "DllGetClassObject", "no Media Foundation codecs or renderers");
            m.set_return(kClassNotAvailable);
        },
        12});
    if (!self_registering) return;
    for (const char* name : {"DllRegisterServer", "DllUnregisterServer"}) {
        auto reg_once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            name, 0,
            [dll, name, reg_once](Marshaler& m) {
                (*reg_once)(dll, name, "no classes to register");
                m.set_return(kSelfRegClass);
            },
            0});
    }
}

/// MFTEnum / MFTEnum2 / MFTEnumEx: no transform is registered, so the
/// documented empty result (Learn: "If no MFTs match the search criteria, the
/// function returns S_OK" with a zero count; measured: the array pointer is
/// NULL). The category GUID is by value: one pointer slot on x64, four stack
/// slots on x86, which shifts the x86 indices of everything after it.
void enum_empty(Marshaler& m, usize array_index64, usize count_index64) {
    const bool is64 = m.cpu_is_64bit();
    const GuestAddr out = m.arg_ptr(is64 ? array_index64 : array_index64 + 3);
    const GuestAddr count = m.arg_ptr(is64 ? count_index64 : count_index64 + 3);
    if (out == 0 || count == 0) {
        m.set_return(kEPointer);
        return;
    }
    (void)m.write_ptr(out, 0);
    (void)m.write_u32(count, 0);
    m.set_return(kSOk);
}

} // namespace

DllExports create_mfplat_thunks(MfPlatManager& mgr) {
    DllExports exports;
    constexpr const char* kDll = "mfplat.dll";

    // HRESULT MFStartup(ULONG Version, DWORD dwFlags)
    // https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfstartup
    // Windows: S_OK / MF_E_BAD_STARTUP_VERSION; flags accepted (measured,
    // mf_startup_probe.expected). Vitreo has no Media Foundation pipeline:
    // after a successful MFStartup a caller is entitled to MFCreateEventQueue,
    // MFCreateStreamDescriptor, MFCreateSourceReader... succeeding, and a
    // caller that got S_OK here dereferenced the null those left behind
    // (an H.264 decoder DLL, #GP at startup, every renderer, 2026-10-01).
    // So the version contract is kept and the platform itself still reports
    // not implemented, loudly, until the pipeline exists.
    exports.entries.push_back(ExportEntry{
        "MFStartup", 0,
        [&mgr](Marshaler& m) {
            const u32 version = m.arg32(0);
            (void)m.arg32(1);
            const u32 hr = mgr.startup(version);
            if (hr != kSOk) {
                m.set_return(hr);
                return;
            }
            (void)mgr.shutdown();
            static std::once_flag logged;
            std::call_once(logged, [] {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED mfplat!MFStartup: no Media "
                             "Foundation pipeline (MFCreate* would fail after a "
                             "successful startup)\n");
            });
            m.set_return(kENotImpl);
        },
        8});
    // HRESULT MFShutdown(void): S_OK, also unbalanced (measured).
    exports.entries.push_back(ExportEntry{
        "MFShutdown", 0, [&mgr](Marshaler& m) { m.set_return(mgr.shutdown()); }, 0});
    // HRESULT MFLockPlatform(void) / MFUnlockPlatform(void)
    exports.entries.push_back(ExportEntry{
        "MFLockPlatform", 0, [&mgr](Marshaler& m) { m.set_return(mgr.lock_platform()); }, 0});
    exports.entries.push_back(ExportEntry{
        "MFUnlockPlatform", 0, [&mgr](Marshaler& m) { m.set_return(mgr.unlock_platform()); }, 0});

    // HRESULT MFCreateAttributes(IMFAttributes** ppMFAttributes, UINT32 cInitialSize)
    exports.entries.push_back(ExportEntry{
        "MFCreateAttributes", 0,
        [&mgr](Marshaler& m) {
            (void)m.arg32(1);
            m.set_return(mgr.create_object(m, MfIface::Attributes, m.arg_ptr(0)));
        },
        8});
    // HRESULT MFCreateMediaType(IMFMediaType** ppMFType)
    exports.entries.push_back(ExportEntry{
        "MFCreateMediaType", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.create_object(m, MfIface::MediaType, m.arg_ptr(0)));
        },
        4});
    register_mf_object_thunks(exports, mgr);

    // HRESULT MFTEnumEx(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
    //   const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*): x86 36.
    exports.entries.push_back(ExportEntry{
        "MFTEnumEx", 0, [](Marshaler& m) { enum_empty(m, 4, 5); }, 36});
    // HRESULT MFTEnum(GUID, UINT32, MFT_REGISTER_TYPE_INFO*, MFT_REGISTER_TYPE_INFO*,
    //   IMFAttributes*, CLSID**, UINT32*): x86 40.
    exports.entries.push_back(ExportEntry{
        "MFTEnum", 0, [](Marshaler& m) { enum_empty(m, 5, 6); }, 40});
    // HRESULT MFTEnum2(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
    //   const MFT_REGISTER_TYPE_INFO*, IMFAttributes*, IMFActivate***, UINT32*): x86 40.
    exports.entries.push_back(ExportEntry{
        "MFTEnum2", 0, [](Marshaler& m) { enum_empty(m, 5, 6); }, 40});

    // MFTIME MFGetSystemTime(void): 100 ns units. Measured
    // (tools/oracle_probe/s7_mf_probe.expected): it is the performance
    // counter / interrupt-time clock, not FILETIME, so it is the same
    // monotonic source as QueryPerformanceCounter here. A LONGLONG return is
    // EDX:EAX on x86.
    exports.entries.push_back(ExportEntry{
        "MFGetSystemTime", 0,
        [](Marshaler& m) { m.set_return_i64(virtual_monotonic_ns() / 100); }, 0});
    // HRESULT MFFrameRateToAverageTimePerFrame(UINT32 num, UINT32 den, UINT64*)
    // Learn gives a table for common rates; the rest is measured
    // (s7_mf_probe.expected, ~400 rates): a rate r = num/den within
    // [c - 0.01, c + 0.01] (double arithmetic) of a table rate c returns the
    // table duration; otherwise round-half-up(10^7 * den / num). A zero
    // numerator or denominator yields 0 with S_OK.
    exports.entries.push_back(ExportEntry{
        "MFFrameRateToAverageTimePerFrame", 0,
        [](Marshaler& m) {
            const u32 numerator = m.arg32(0);
            const u32 denominator = m.arg32(1);
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(kEPointer);
                return;
            }
            u64 duration = 0;
            if (numerator != 0 && denominator != 0) {
                struct Entry { double rate; u64 duration; };
                static constexpr std::array<Entry, 8> kTable{{
                    {59.94, 166833}, {29.97, 333667}, {23.976, 417188},
                    {60.0, 166667}, {30.0, 333333}, {50.0, 200000},
                    {25.0, 400000}, {24.0, 416667}}};
                const double rate = static_cast<double>(numerator) /
                                    static_cast<double>(denominator);
                bool matched = false;
                for (const Entry& e : kTable) {
                    if (rate >= e.rate - 0.01 && rate <= e.rate + 0.01) {
                        duration = e.duration;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    duration = (10000000ull * denominator + numerator / 2) / numerator;
                }
            }
            (void)m.write_u64(out, duration);
            m.set_return(kSOk);
        }, 12});
    // LONGLONG MFllMulDiv(LONGLONG a, LONGLONG b, LONGLONG c, LONGLONG d):
    // ((a * b) + d) / c with a 128-bit intermediate (Learn mfapi.h). Pure
    // arithmetic; x86 passes the four values as eight stack slots and
    // returns EDX:EAX. Division by zero is not documented and unmeasured: it
    // answers -1 and says so once.
    exports.entries.push_back(ExportEntry{
        "MFllMulDiv", 0,
        [](Marshaler& m) {
            const bool is64 = m.cpu_is_64bit();
            const auto read = [&](usize i) -> i64 {
                if (is64) return static_cast<i64>(m.arg(i));
                return static_cast<i64>((m.arg(2 * i) & 0xFFFFFFFFull) | (m.arg(2 * i + 1) << 32));
            };
            const i64 a = read(0), b = read(1), c = read(2), d = read(3);
            if (c == 0) {
                static LoudOnce once;
                once("mfplat.dll", "MFllMulDiv", "division by zero is undocumented");
                m.set_return_i64(static_cast<u64>(-1));
                return;
            }
            const __int128 r = (static_cast<__int128>(a) * b + d) / c;
            m.set_return_i64(static_cast<u64>(static_cast<i64>(r)));
        }, 32});

    // -- Object factories Vitreo cannot provide. Each one's out parameter is
    // nulled and the failure is announced once.
    const char* kNoPipeline = "no Media Foundation pipeline";
    exports.entries.push_back(factory_notimpl(kDll, "MFGetService", 3, 16, kNoPipeline));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateEventQueue", 0, 4, kNoPipeline));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateMemoryBuffer", 1, 8, "no sample/buffer objects"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreatePresentationDescriptor", 2, 12, kNoPipeline));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSample", 0, 4, "no sample/buffer objects"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateStreamDescriptor", 3, 16, kNoPipeline));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateAlignedMemoryBuffer", 2, 12, "no sample/buffer objects"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateAsyncResult", 3, 16, "no work queues"));
    // HRESULT MFCreateDXGIDeviceManager(UINT* resetToken, IMFDXGIDeviceManager**)
    {
        auto once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            "MFCreateDXGIDeviceManager", 0,
            [once](Marshaler& m) {
                if (const GuestAddr token = m.arg_ptr(0); token != 0) (void)m.write_u32(token, 0);
                const GuestAddr out = m.arg_ptr(1);
                if (out == 0) {
                    m.set_return(kEPointer);
                    return;
                }
                (void)m.write_ptr(out, 0);
                (*once)(kDll, "MFCreateDXGIDeviceManager", "no DXGI device manager for Media Foundation");
                m.set_return(kENotImpl);
            }, 8});
    }
    // HRESULT MFCreateDXGISurfaceBuffer(REFIID, IUnknown*, UINT, BOOL,
    //                                   IMFMediaBuffer**): x86 stdcall 20.
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateDXGISurfaceBuffer", 4, 20, "no sample/buffer objects"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateMediaEvent", 4, 20, kNoPipeline));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateMediaBufferWrapper", 3, 16, "no sample/buffer objects"));
    // HRESULT MFCreateWaveFormatExFromMFMediaType(IMFMediaType*, WAVEFORMATEX**,
    //                                             UINT32* pcbSize, UINT32 Flags)
    {
        auto once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            "MFCreateWaveFormatExFromMFMediaType", 0,
            [once](Marshaler& m) {
                if (const GuestAddr size = m.arg_ptr(2); size != 0) (void)m.write_u32(size, 0);
                const GuestAddr out = m.arg_ptr(1);
                if (out == 0) {
                    m.set_return(kEPointer);
                    return;
                }
                (void)m.write_ptr(out, 0);
                (*once)(kDll, "MFCreateWaveFormatExFromMFMediaType", "media type to WAVEFORMATEX conversion is not built");
                m.set_return(kENotImpl);
            }, 16});
    }
    {
        auto once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            "MFLockDXGIDeviceManager", 0,
            [once](Marshaler& m) {
                if (const GuestAddr reset = m.arg_ptr(0); reset != 0) (void)m.write_u32(reset, 0);
                if (const GuestAddr device = m.arg_ptr(1); device != 0) (void)m.write_ptr(device, 0);
                (*once)(kDll, "MFLockDXGIDeviceManager", "no DXGI device manager for Media Foundation");
                m.set_return(kENotImpl);
            }, 8});
    }

    // Everything else the SDK import library publishes: loud, documented
    // failure values, never a missing-export stub.
    add_table_exports(exports, kDll);
    return exports;
}

DllExports create_mf_thunks() {
    DllExports exports;
    constexpr const char* kDll = "mf.dll";
    exports.entries.push_back(factory_notimpl(kDll, "MFGetService", 3, 16, "no Media Foundation pipeline"));
    // Capture device enumeration needs a real MF pipeline; an empty
    // successful list would tell the guest the machine has no camera or
    // microphone, which is a different claim from "unsupported".
    {
        auto once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            "MFEnumDeviceSources", 0,
            [once](Marshaler& m) {
                (void)m.arg_ptr(0);
                if (const GuestAddr out = m.arg_ptr(1); out != 0) (void)m.write_ptr(out, 0);
                if (const GuestAddr count = m.arg_ptr(2); count != 0) (void)m.write_u32(count, 0);
                (*once)(kDll, "MFEnumDeviceSources", "no capture device enumeration");
                m.set_return(kENotImpl);
            }, 12});
    }
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateDeviceSource", 1, 8, "no capture devices"));
    add_table_exports(exports, kDll);
    add_com_server_exports(exports, kDll, false);
    return exports;
}

DllExports create_dxva2_thunks() {
    DllExports exports;
    constexpr const char* kDll = "dxva2.dll";
    // HRESULT DXVA2CreateDirect3DDeviceManager9(UINT*, IDirect3DDeviceManager9**)
    // https://learn.microsoft.com/en-us/windows/win32/api/dxva2api/nf-dxva2api-dxva2createdirect3ddevicemanager9
    {
        auto once = std::make_shared<LoudOnce>();
        exports.entries.push_back(ExportEntry{
            "DXVA2CreateDirect3DDeviceManager9", 0,
            [once](Marshaler& m) {
                if (const GuestAddr token = m.arg_ptr(0); token != 0) (void)m.write_u32(token, 0);
                const GuestAddr out = m.arg_ptr(1);
                if (out == 0) {
                    m.set_return(kEPointer);
                    return;
                }
                (void)m.write_ptr(out, 0);
                (*once)(kDll, "DXVA2CreateDirect3DDeviceManager9", "no Direct3D 9 device manager for DXVA");
                m.set_return(kENotImpl);
            },
            8});
    }
    add_table_exports(exports, kDll);
    return exports;
}

DllExports create_mfreadwrite_thunks() {
    DllExports exports;
    constexpr const char* kDll = "mfreadwrite.dll";
    // HRESULT MFCreateSourceReaderFromMediaSource(
    //   IMFMediaSource*, IMFAttributes*, IMFSourceReader**)
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSourceReaderFromMediaSource", 2, 12, "no source reader"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSourceReaderFromURL", 2, 12, "no source reader"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSourceReaderFromByteStream", 2, 12, "no source reader"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSinkWriterFromURL", 3, 16, "no sink writer"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateSinkWriterFromMediaSink", 2, 12, "no sink writer"));
    add_table_exports(exports, kDll);
    add_com_server_exports(exports, kDll, false);
    return exports;
}

DllExports create_evr_thunks() {
    DllExports exports;
    constexpr const char* kDll = "evr.dll";
    // HRESULT MFCreateVideoPresenter(IUnknown* pOwner, REFIID riidDevice,
    //                                REFIID riid, void** ppVideoPresenter)
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateVideoPresenter", 3, 16, "no enhanced video renderer"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateVideoMixer", 3, 16, "no enhanced video renderer"));
    exports.entries.push_back(factory_notimpl(kDll, "MFCreateVideoSampleAllocator", 1, 8, "no enhanced video renderer"));
    add_table_exports(exports, kDll);
    add_com_server_exports(exports, kDll, true);
    return exports;
}

DllExports create_msmpeg2vdec_thunks() {
    DllExports exports;
    add_com_server_exports(exports, "msmpeg2vdec.dll", true);
    return exports;
}

} // namespace vitreo::winapi
