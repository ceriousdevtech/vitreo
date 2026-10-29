// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — in-proc system DLLs that must LoadLibrary without faking engines.
//
// DllGetClassObject: https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-dllgetclassobject
// AMGetErrorTextA:   https://learn.microsoft.com/en-us/windows/win32/api/errors/nf-errors-amgeterrortexta
// D3DCompile:        https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile

#include "inproc_servers.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <cstdio>
#include <vector>

namespace vitreo::winapi {

namespace {

#include "audio_async_ordinal.inc"

constexpr u32 kSOk = 0;
constexpr u32 kSFalse = 1;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kEFail = 0x80004005u;
constexpr u32 kClassNotAvailable = 0x80040111u; // CLASS_E_CLASSNOTAVAILABLE
constexpr u32 kMmsyserrNotSupported = 8; // MMSYSERR_NOTSUPPORTED

void dll_get_class_object(Marshaler& m) {
    // HRESULT DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const GuestAddr ppv = m.arg_ptr(2);
    if (ppv != 0) {
        (void)m.write_ptr(ppv, 0);
    }
    m.set_return(kClassNotAvailable);
}

void dll_can_unload_now(Marshaler& m) {
    m.set_return(kSFalse);
}

void consume_and_notimpl(Marshaler& m, usize argc) {
    for (usize i = 0; i < argc; ++i) {
        (void)m.arg(i);
    }
    m.set_return(kENotImpl);
}

u32 write_am_error(Marshaler& m, GuestAddr buf, u32 max_len, const char* text,
                   bool wide) {
    if (buf == 0 || max_len == 0 || text == nullptr) {
        return 0;
    }
    const usize needed = std::strlen(text);
    if (needed + 1 > max_len) {
        return 0;
    }
    if (wide) {
        for (usize i = 0; i <= needed; ++i) {
            const u16 ch = i < needed ? static_cast<u16>(static_cast<unsigned char>(text[i])) : 0;
            (void)m.write_u16(buf + i * 2, ch);
        }
    } else {
        (void)m.memory().write_bytes(buf, reinterpret_cast<const u8*>(text), needed + 1);
    }
    return static_cast<u32>(needed);
}

const char* known_am_text(u32 hr) {
    switch (hr) {
    case kSOk:
        return "The operation completed successfully.";
    case kSFalse:
        return "The operation completed successfully (S_FALSE).";
    case kENotImpl:
        return "Not implemented.";
    case kEPointer:
        return "Invalid pointer.";
    case kEFail:
        return "Unspecified error.";
    case kClassNotAvailable:
        return "ClassFactory cannot supply requested class.";
    default:
        return nullptr;
    }
}

} // namespace

DllExports create_quartz_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    exports.entries.push_back(
        {"AMGetErrorTextA", 0,
         [](Marshaler& m) {
             const u32 hr = static_cast<u32>(m.arg(0));
             const GuestAddr buf = m.arg_ptr(1);
             const u32 max_len = static_cast<u32>(m.arg(2));
             const char* text = known_am_text(hr);
             m.set_return(write_am_error(m, buf, max_len, text, false));
         },
         12});
    exports.entries.push_back(
        {"AMGetErrorTextW", 0,
         [](Marshaler& m) {
             const u32 hr = static_cast<u32>(m.arg(0));
             const GuestAddr buf = m.arg_ptr(1);
             const u32 max_len = static_cast<u32>(m.arg(2));
             const char* text = known_am_text(hr);
             m.set_return(write_am_error(m, buf, max_len, text, true));
         },
         12});
    return exports;
}

DllExports create_devenum_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    return exports;
}

DllExports create_dxdiagn_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    return exports;
}

DllExports create_d3dxof_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    return exports;
}

DllExports create_d3dcompiler_47_thunks() {
    DllExports exports;
    // HRESULT D3DCompile(...) — no HLSL compiler; E_NOTIMPL.
    exports.entries.push_back(
        {"D3DCompile", 0, [](Marshaler& m) { consume_and_notimpl(m, 11); }, 44});
    exports.entries.push_back(
        {"D3DCompile2", 0, [](Marshaler& m) { consume_and_notimpl(m, 15); }, 60});
    exports.entries.push_back(
        {"D3DGetBlobPart", 0, [](Marshaler& m) { consume_and_notimpl(m, 5); }, 20});
    return exports;
}

DllExports create_msacm32_thunks() {
    DllExports exports;
    // DWORD acmGetVersion(void)
    exports.entries.push_back({"acmGetVersion", 0,
                               [](Marshaler& m) { m.set_return(0x04000000); }, 0});
    // MMRESULT acmDriverEnum(ACMDRIVERENUMCB, DWORD_PTR, DWORD)
    exports.entries.push_back(
        {"acmDriverEnum", 0,
         [](Marshaler& m) {
             (void)m.arg(0);
             (void)m.arg(1);
             (void)m.arg(2);
             m.set_return(kMmsyserrNotSupported);
         },
         12});
    exports.entries.push_back(
        {"acmFormatSuggest", 0,
         [](Marshaler& m) {
             for (usize i = 0; i < 6; ++i) {
                 (void)m.arg(i);
             }
             m.set_return(kMmsyserrNotSupported);
         },
         24});
    // No ACM codecs are installed. Queries must not manufacture a stream.
    // Microsoft Learn acmStreamOpen: ACMERR_NOTPOSSIBLE reports unsupported conversion.
    exports.entries.push_back({"acmStreamOpen", 0, [](Marshaler& m) {
        constexpr u32 query = 1;
        const u32 flags = static_cast<u32>(m.arg(7));
        const GuestAddr output = m.arg_ptr(0);
        if ((flags & ~0x70007u) != 0 ||
            ((flags & 0x70000u) != 0 && (flags & 0x70000u) != 0x10000u &&
             (flags & 0x70000u) != 0x30000u && (flags & 0x70000u) != 0x50000u)) {
            m.set_return(10u); // MMSYSERR_INVALFLAG
            return;
        }
        if (!(flags & query)) {
            if (!output) { m.set_return(11u); return; }
            const bool ok = m.cpu_is_64bit() ? m.memory().write_u64(output, 0).is_ok()
                                           : m.memory().write_u32(output, 0).is_ok();
            if (!ok) { m.set_return(11u); return; }
        }
        if (m.arg_ptr(1)) { m.set_return(5u); return; } // invalid driver handle
        if (!m.arg_ptr(2) || !m.arg_ptr(3)) { m.set_return(11u); return; }
        m.set_return(512u); // ACMERR_NOTPOSSIBLE
    }, 32});
    for (const char* name : {"acmStreamPrepareHeader", "acmStreamUnprepareHeader",
                             "acmStreamConvert"}) {
        exports.entries.push_back({name, 0,
            [](Marshaler& m) { m.set_return(5u); }, 12});
    }
    for (const char* name : {"acmStreamClose", "acmStreamReset"}) {
        exports.entries.push_back({name, 0,
            [](Marshaler& m) { m.set_return(5u); }, 8});
    }
    exports.entries.push_back({"acmStreamSize", 0,
        [](Marshaler& m) { m.set_return(5u); }, 16});
    return exports;
}

DllExports create_avicap32_thunks() {
    DllExports exports;
    // HWND capCreateCaptureWindowA(LPCSTR, DWORD, int, int, int, int, HWND, int)
    // No capture device; return NULL.
    exports.entries.push_back(
        {"capCreateCaptureWindowA", 0,
         [](Marshaler& m) {
             for (usize i = 0; i < 8; ++i) {
                 (void)m.arg(i);
             }
             m.set_return(0);
         },
         32});
    exports.entries.push_back(
        {"capCreateCaptureWindowW", 0,
         [](Marshaler& m) {
             for (usize i = 0; i < 8; ++i) {
                 (void)m.arg(i);
             }
             m.set_return(0);
         },
         32});
    exports.entries.push_back(
        {"capGetDriverDescriptionA", 0,
         [](Marshaler& m) {
             for (usize i = 0; i < 5; ++i) {
                 (void)m.arg(i);
             }
             m.set_return(0);
         },
         20});
    return exports;
}

// ---- msdmo.dll DMO_MEDIA_TYPE helpers (dmort.h) ----
//
// Layout from mediaobj.h (SDK 10.0.26100): GUID majortype, GUID subtype,
// BOOL bFixedSizeSamples, BOOL bTemporalCompression, ULONG lSampleSize,
// GUID formattype, IUnknown* pUnk, ULONG cbFormat, BYTE* pbFormat.
// sizeof is 72 on x86 (pUnk 60, cbFormat 64, pbFormat 68) and 88 on x64
// (pUnk 64, cbFormat 72, pbFormat 80), measured by
// tools/oracle_probe/gfx_media_exports_probe(.x86).expected.
//
// Behaviour measured on Windows 11 by the same probe:
//   - every NULL pointer argument is E_POINTER; out-params stay untouched;
//   - MoInitMediaType writes only pUnk=NULL, cbFormat, and pbFormat
//     (CoTaskMemAlloc(cbFormat), NULL when cbFormat is 0);
//   - MoFreeMediaType CoTaskMemFrees pbFormat and NULLs pbFormat and pUnk
//     WITHOUT calling pUnk->Release; cbFormat is left as it was;
//   - MoCopyMediaType copies every field except pUnk (dest pUnk = NULL, no
//     AddRef) into a fresh format block;
//   - MoCreate/MoDuplicate allocate the struct itself with CoTaskMemAlloc
//     (IMalloc::GetSize == sizeof) and leave its header uninitialised.
namespace {

constexpr u32 kEOutOfMemory = 0x8007000Eu;
constexpr GuestAddr kMtHeaderBytes = 60; // majortype..formattype

struct MediaTypeLayout {
    u32 size, punk, cb_format, pb_format;
};

MediaTypeLayout media_type_layout(bool x64) {
    return x64 ? MediaTypeLayout{88, 64, 72, 80} : MediaTypeLayout{72, 60, 64, 68};
}

class MediaTypeOps {
public:
    MediaTypeOps(Marshaler& m, const TaskMemoryFns& task)
        : m_(m), task_(task), l_(media_type_layout(m.cpu_is_64bit())) {}

    u32 init(GuestAddr mt, u32 cb_format) {
        if (mt == 0) return kEPointer;
        GuestAddr block = 0;
        if (cb_format != 0) {
            block = task_.allocate ? task_.allocate(cb_format) : 0;
            if (block == 0) return kEOutOfMemory;
        }
        (void)m_.write_ptr(mt + l_.punk, 0);
        (void)m_.write_u32(mt + l_.cb_format, cb_format);
        (void)m_.write_ptr(mt + l_.pb_format, block);
        return kSOk;
    }

    u32 free_block(GuestAddr mt) {
        if (mt == 0) return kEPointer;
        const GuestAddr block = read_ptr(mt + l_.pb_format);
        if (block != 0 && task_.free) task_.free(block);
        (void)m_.write_ptr(mt + l_.pb_format, 0);
        (void)m_.write_ptr(mt + l_.punk, 0);
        return kSOk;
    }

    u32 copy(GuestAddr dst, GuestAddr src) {
        if (dst == 0 || src == 0) return kEPointer;
        std::vector<u8> header(kMtHeaderBytes);
        if (m_.memory().read_bytes(src, header.data(), header.size()).is_err())
            return kEPointer;
        const u32 cb = m_.read_u32(src + l_.cb_format).value_or(0);
        const GuestAddr src_block = read_ptr(src + l_.pb_format);
        std::vector<u8> bytes;
        if (cb != 0 && src_block != 0) {
            bytes.resize(cb);
            if (m_.memory().read_bytes(src_block, bytes.data(), cb).is_err())
                return kEPointer;
        }
        GuestAddr block = 0;
        if (cb != 0) {
            block = task_.allocate ? task_.allocate(cb) : 0;
            if (block == 0) return kEOutOfMemory;
            if (!bytes.empty())
                (void)m_.memory().write_bytes(block, bytes.data(), bytes.size());
        }
        (void)m_.memory().write_bytes(dst, header.data(), header.size());
        (void)m_.write_ptr(dst + l_.punk, 0);
        (void)m_.write_u32(dst + l_.cb_format, cb);
        (void)m_.write_ptr(dst + l_.pb_format, block);
        return kSOk;
    }

    /// Allocate a DMO_MEDIA_TYPE and fill it with @p fill; on failure the
    /// struct is released again and *out is left untouched.
    template <typename Fill>
    u32 allocate_into(GuestAddr out, Fill&& fill) {
        const GuestAddr mt = task_.allocate ? task_.allocate(l_.size) : 0;
        if (mt == 0) return kEOutOfMemory;
        const u32 hr = fill(mt);
        if (hr != kSOk) {
            if (task_.free) task_.free(mt);
            return hr;
        }
        (void)m_.write_ptr(out, mt);
        return kSOk;
    }

    u32 destroy(GuestAddr mt) {
        const u32 hr = free_block(mt);
        if (hr == kSOk && task_.free) task_.free(mt);
        return hr;
    }

private:
    GuestAddr read_ptr(GuestAddr at) {
        return m_.cpu_is_64bit() ? static_cast<GuestAddr>(m_.memory().read_u64(at).value_or(0))
                                 : static_cast<GuestAddr>(m_.read_u32(at).value_or(0));
    }

    Marshaler& m_;
    const TaskMemoryFns& task_;
    MediaTypeLayout l_;
};

} // namespace

DllExports create_msdmo_thunks(TaskMemoryFns task_memory) {
    DllExports exports;
    auto task = std::make_shared<TaskMemoryFns>(std::move(task_memory));
    // Ordinals from dumpbin /exports C:\Windows\System32\msdmo.dll.
    exports.entries.push_back(
        {"DMOEnum", 1, [](Marshaler& m) { consume_and_notimpl(m, 7); }, 28});
    // HRESULT MoInitMediaType(DMO_MEDIA_TYPE*, DWORD cbFormat)
    exports.entries.push_back({"MoInitMediaType", 15, [task](Marshaler& m) {
        m.set_return(MediaTypeOps(m, *task).init(m.arg_ptr(0), m.arg32(1)));
    }, 8});
    // HRESULT MoFreeMediaType(DMO_MEDIA_TYPE*)
    exports.entries.push_back({"MoFreeMediaType", 14, [task](Marshaler& m) {
        m.set_return(MediaTypeOps(m, *task).free_block(m.arg_ptr(0)));
    }, 4});
    // HRESULT MoCopyMediaType(DMO_MEDIA_TYPE* dest, const DMO_MEDIA_TYPE* src)
    exports.entries.push_back({"MoCopyMediaType", 10, [task](Marshaler& m) {
        m.set_return(MediaTypeOps(m, *task).copy(m.arg_ptr(0), m.arg_ptr(1)));
    }, 8});
    // HRESULT MoCreateMediaType(DMO_MEDIA_TYPE** ppmt, DWORD cbFormat)
    exports.entries.push_back({"MoCreateMediaType", 11, [task](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        const u32 cb = m.arg32(1);
        if (out == 0) { m.set_return(kEPointer); return; }
        MediaTypeOps ops(m, *task);
        m.set_return(ops.allocate_into(out, [&](GuestAddr mt) { return ops.init(mt, cb); }));
    }, 8});
    // HRESULT MoDeleteMediaType(DMO_MEDIA_TYPE*)
    exports.entries.push_back({"MoDeleteMediaType", 12, [task](Marshaler& m) {
        m.set_return(MediaTypeOps(m, *task).destroy(m.arg_ptr(0)));
    }, 4});
    // HRESULT MoDuplicateMediaType(DMO_MEDIA_TYPE** ppmtDest, const DMO_MEDIA_TYPE* src)
    exports.entries.push_back({"MoDuplicateMediaType", 13, [task](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        const GuestAddr src = m.arg_ptr(1);
        if (out == 0 || src == 0) { m.set_return(kEPointer); return; }
        MediaTypeOps ops(m, *task);
        m.set_return(ops.allocate_into(out, [&](GuestAddr mt) { return ops.copy(mt, src); }));
    }, 8});
    return exports;
}

DllExports create_wbemprox_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    return exports;
}

DllExports create_wbemdisp_thunks() {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    return exports;
}

DllExports create_mmdevapi_thunks(std::function<u32(Marshaler&)> activate_async) {
    DllExports exports;
    exports.entries.push_back({"DllGetClassObject", 0, dll_get_class_object, 12});
    exports.entries.push_back({"DllCanUnloadNow", 0, dll_can_unload_now, 0});
    // HRESULT ActivateAudioInterface(REFIID, PROPVARIANT*, void**)
    // HRESULT ActivateAudioInterfaceAsync(...)
    // COM owns async operation objects and the real WASAPI audio client.
    exports.entries.push_back(
        {"ActivateAudioInterface", 0, [](Marshaler& m) { consume_and_notimpl(m, 3); }, 12});
    exports.entries.push_back(
        {"ActivateAudioInterfaceAsync", kAudioActivateAsyncOrdinal,
         [activate_async = std::move(activate_async)](Marshaler& m) {
             if (activate_async) m.set_return(activate_async(m));
             else {
                 static std::once_flag once;
                 std::call_once(once, [] { std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mmdevapi!ActivateAudioInterfaceAsync: COM activation provider unavailable\n"); });
                 consume_and_notimpl(m, 5);
             }
         }, 20});
    return exports;
}

} // namespace vitreo::winapi
