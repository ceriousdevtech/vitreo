// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core_thunks.cpp
/// @brief ole32 thunk registration: create_com_core_thunks() drives the ordered
/// append_com_*_thunks() slices (this file and com_core_thunks_*.cpp) and the
/// filter-graph and WIC appenders, then applies the WASAPI diagnostic
/// wrapper.

#include "com_core.hpp"
#include "filter_graph.hpp"
#include "wic.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "com_core_thunks_internal.hpp"
#include "ole_guid_internal.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_com_core_thunks(ComCoreManager& mgr) {
    DllExports exports;
    exports.entries.reserve(11);

    append_com_lifecycle_thunks(exports, mgr);
    append_com_remote_proxy_thunks(exports, mgr);
    append_com_object_vtable_thunks(exports, mgr);
    append_com_completion_thunks(exports, mgr);
    append_com_network_thunks(exports, mgr);
    append_com_audio_thunks(exports, mgr);
    append_com_shelllink_thunks(exports, mgr);
    append_filter_graph_thunks(exports, mgr);
    append_wic_thunks(exports, mgr);
    append_com_memory_stream_thunks(exports, mgr);
    append_com_runtime_thunks(exports, mgr);
    append_com_guid_string_thunks(exports, mgr);
    append_com_ole_stub_thunks(exports, mgr);

    if (std::getenv("VITREO_WASAPI_DIAG") != nullptr) {
        for (auto& entry : exports.entries) {
            const std::string& name = entry.name;
            if (name.rfind("Vitreo_Audio", 0) != 0 &&
                name.rfind("Vitreo_MMDevice", 0) != 0) {
                continue;
            }
            auto calls = std::make_shared<std::atomic<u32>>(0u);
            entry.handler = [inner = std::move(entry.handler), name,
                             calls](Marshaler& m) {
                const u64 a1 = m.arg(1), a2 = m.arg(2), a3 = m.arg(3);
                inner(m);
                const u32 n = calls->fetch_add(1u) + 1u;
                if (n <= 12u || (n & (n - 1u)) == 0u) {
                    std::fprintf(stderr,
                                 "[wasapi-diag] %s #%u a1=0x%llx a2=0x%llx "
                                 "a3=0x%llx -> 0x%08llx\n",
                                 name.c_str(), n,
                                 static_cast<unsigned long long>(a1),
                                 static_cast<unsigned long long>(a2),
                                 static_cast<unsigned long long>(a3),
                                 static_cast<unsigned long long>(
                                     m.cpu() ? m.cpu()->gpr.rax & 0xFFFFFFFFu
                                             : 0u));
                }
            };
        }
    }

    return exports;
}

/// CoInitialize through CoCreateInstance: apartment lifecycle and class
/// creation.
void append_com_lifecycle_thunks(DllExports& exports, ComCoreManager& mgr) {
    // CoInitialize (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CoInitialize", 0,
        [&mgr](Marshaler& m) {
            u32 hr = mgr.co_initialize(m);
            m.set_return(hr);
        },
        4
    });
    // CoInitializeEx (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CoInitializeEx", 0,
        [&mgr](Marshaler& m) {
            u32 coinit = static_cast<u32>(m.arg(1));
            u32 hr = mgr.co_initialize_ex(m, coinit);
            m.set_return(hr);
        },
        8
    });
    // CoUninitialize (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "CoUninitialize", 0,
        [&mgr](Marshaler& m) {
            mgr.co_uninitialize(m);
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "CoIncrementMTAUsage", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr cookie_ptr = m.arg_ptr(0);
            if (cookie_ptr == 0 || m.write_ptr(cookie_ptr, 0).is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            GuestAddr cookie = 0;
            const u32 hr = mgr.co_increment_mta_usage(&cookie);
            if (hr == com::S_OK && m.write_ptr(cookie_ptr, cookie).is_err()) {
                (void)mgr.co_decrement_mta_usage(cookie);
                m.set_return(com::E_INVALIDARG);
                return;
            }
            m.set_return(hr);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CoDecrementMTAUsage", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_decrement_mta_usage(m.arg_ptr(0)));
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CoAddRefServerProcess", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_add_ref_server_process());
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "CoReleaseServerProcess", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_release_server_process());
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "CoResumeClassObjects", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_resume_class_objects());
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "CoSuspendClassObjects", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_suspend_class_objects());
        },
        0
    });
    // CoGetCallContext(REFIID, void**). Standard marshaling exposes
    // IServerSecurity only while dispatching an incoming COM call.
    exports.entries.push_back(ExportEntry{
        "CoGetCallContext", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_get_call_context(m));
        },
        8
    });
    // CoCreateInstance (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "CoCreateInstance", 0,
        [&mgr](Marshaler& m) {
            u32 hr = mgr.co_create_instance(m);
            if ((hr & 0x80000000u) != 0) {
                // One line per (class, interface, HRESULT): a guest that does
                // not check the result dereferences the NULL *ppv later, far
                // from the cause, and the trace gate is often off in exactly
                // the store-launched processes where that happens.
                Guid clsid{}, iid{};
                const bool have_clsid = m.arg_ptr(0) != 0 &&
                    m.read_struct<Guid>(m.arg_ptr(0)).is_ok();
                if (have_clsid) clsid = m.read_struct<Guid>(m.arg_ptr(0)).value();
                if (m.arg_ptr(3) != 0) {
                    if (auto r = m.read_struct<Guid>(m.arg_ptr(3)); r.is_ok()) iid = r.value();
                }
                static std::mutex mutex;
                static std::set<std::string> seen;
                char key[160];
                std::snprintf(key, sizeof(key), "%s %s %08x", format_guid(clsid).c_str(),
                              format_guid(iid).c_str(), hr);
                std::lock_guard lock(mutex);
                if (seen.insert(key).second) {
                    std::fprintf(stderr,
                                 "[ole32] CoCreateInstance clsid=%s iid=%s ctx=0x%x -> 0x%08x "
                                 "(thread in apartment: %s)\n",
                                 format_guid(clsid).c_str(), format_guid(iid).c_str(),
                                 static_cast<u32>(m.arg(2)), hr,
                                 mgr.thread_in_apartment(m) ? "yes" : "no");
                }
            }
            m.set_return(hr);
        },
        20
    });
}

/// CoGetMalloc and the IMalloc vtable, task memory, OLE init, CoCreateGuid
/// and library loading.
void append_com_runtime_thunks(DllExports& exports, ComCoreManager& mgr) {
    // CoGetMalloc(DWORD dwMemContext, LPMALLOC* ppMalloc) → HRESULT
    exports.entries.push_back(ExportEntry{
        "CoGetMalloc", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoGetMalloc(m); },
        8});
    // CoRegisterInitializeSpy(IInitializeSpy*, ULARGE_INTEGER*) → HRESULT
    exports.entries.push_back(ExportEntry{
        "CoRegisterInitializeSpy", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoRegisterInitializeSpy(m); },
        8});
    // CoRevokeInitializeSpy(ULARGE_INTEGER) — x86 stdcall pops 8 (two dwords).
    exports.entries.push_back(ExportEntry{
        "CoRevokeInitializeSpy", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoRevokeInitializeSpy(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_query_interface(m); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_add_ref(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_Release", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_release(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_Alloc", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_alloc(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_Realloc", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_realloc(m); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_Free", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_free(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_GetSize", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_get_size(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_DidAlloc", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_did_alloc(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_IMalloc_HeapMinimize", 0,
        [&mgr](Marshaler& m) { mgr.imalloc_heap_minimize(m); }, 4});

    // CoTaskMemAlloc (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CoTaskMemAlloc", 0,
        [&mgr](Marshaler& m) {
            const u64 size = m.arg(0);
            const GuestAddr addr = mgr.co_task_mem_alloc_guest(size);
            VITREO_TRACE(
                "[ole32] CoTaskMemAlloc(%llu) -> 0x%llx\n",
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(addr));
            m.set_return(static_cast<u64>(addr));
        },
        4
    });
    // CoTaskMemFree (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CoTaskMemFree", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr address = m.arg_ptr(0);
            VITREO_TRACE(
                "[ole32] CoTaskMemFree(0x%llx)\n",
                static_cast<unsigned long long>(address));
            mgr.co_task_mem_free_guest(address);
        },
        4
    });
    // CoTaskMemRealloc (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CoTaskMemRealloc", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr address = m.arg_ptr(0);
            const u64 size = m.arg(1);
            m.set_return(mgr.co_task_mem_realloc_guest(address, size));
        },
        8
    });

    // OleInitialize (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "OleInitialize", 0,
        [&mgr](Marshaler& m) {
            // OleInitialize(NULL) is the only documented call: it enters an
            // STA (so a later CoInitializeEx(MULTITHREADED) is
            // RPC_E_CHANGED_MODE) and takes an init count. pvReserved is
            // reserved and not otherwise interpreted.
            (void)m.arg(0);
            u32 hr = mgr.ole_initialize(m);
            m.set_return(hr);
        },
        4
    });

    // OleUninitialize (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "OleUninitialize", 0,
        [&mgr](Marshaler& m) {
            mgr.ole_uninitialize(m);
        },
        0
    });

    // CoCreateGuid (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CoCreateGuid", 0,
        [](Marshaler& m) {
            const GuestAddr output = m.arg_ptr(0);
            if (output == 0) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            const auto uuid = generate_uuid_v4();
            if (m.memory().write_bytes(
                    output, uuid.data(), uuid.size()).is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            m.set_return(com::S_OK);
        },
        4
    });

    // -------------------------------------------------------------------------
    // CoLoadLibrary / CoFreeLibrary / CoFreeUnusedLibraries
    //
    // These are LoadLibrary/FreeLibrary plus COM's "unload when unused"
    // bookkeeping. Callers pass the returned HINSTANCE to GetProcAddress.
    // A missing-export stub returns 0xFFFFFFFF; GetProcAddress on that
    // non-module returns NULL, and the caller then calls through it.
    // -------------------------------------------------------------------------

    // HINSTANCE CoLoadLibrary(LPOLESTR lpszLibName, BOOL bAutoFree)
    exports.entries.push_back(ExportEntry{
        "CoLoadLibrary", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.co_load_library(m)); },
        8
    });

    // void CoFreeLibrary(HINSTANCE hInst)
    exports.entries.push_back(ExportEntry{
        "CoFreeLibrary", 0,
        [&mgr](Marshaler& m) { mgr.co_free_library(m); m.set_return(0u); },
        4
    });

    // void CoFreeUnusedLibraries(void)
    //
    // Vitreo never marks a library auto-free (CoLoadLibrary's bAutoFree only
    // affects unload timing), so there is nothing to release here. Doing
    // nothing is what Windows does when no library is eligible.
    exports.entries.push_back(ExportEntry{
        "CoFreeUnusedLibraries", 0,
        [](Marshaler& m) { m.set_return(0u); },
        0
    });
}

} // namespace vitreo::winapi
