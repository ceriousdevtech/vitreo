// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core_thunks_objects.cpp
/// @brief ole32 thunks for the internal COM vtable objects: remote proxies,
/// IServerSecurity, ShellLink, IGlobalOptions, FTM/agile/threading-info and
/// memory-stream methods.

#include "shell_item_dialogs.hpp"
#include "com_core.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_thunks_internal.hpp"

#include <atomic>
#include <string>

namespace vitreo::winapi {

/// Remote-proxy IUnknown/IDispatch methods, IServerSecurity and the
/// Vitreo_RemoteProxy_Invoke_N slots.
void append_com_remote_proxy_thunks(DllExports& exports, ComCoreManager& mgr) {
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteProxy_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.remote_proxy_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteProxy_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.remote_proxy_add_ref(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteProxy_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.remote_proxy_release(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteDispatch_GetTypeInfoCount", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.remote_dispatch_get_type_info_count(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteDispatch_GetTypeInfo", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.remote_dispatch_get_type_info(m));
        }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteDispatch_GetIDsOfNames", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.remote_dispatch_get_ids_of_names(m));
        }, 24});
    exports.entries.push_back(ExportEntry{
        "Vitreo_RemoteDispatch_Invoke", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.remote_dispatch_invoke(m));
        }, 36});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_query_interface(m));
        }, 12});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_add_ref(m));
        }, 4});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_release(m));
        }, 4});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_QueryBlanket", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_query_blanket(m));
        }, 32});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_ImpersonateClient", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_impersonate_client(m));
        }, 4});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_RevertToSelf", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_revert_to_self(m));
        }, 4});
    exports.entries.push_back({
        "Vitreo_ServerSecurity_IsImpersonating", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.server_security_is_impersonating(m));
        }, 4});
    // Pre-register one gateway for each physical x86 argument count. Runtime
    // proxy vtables use dispatch aliases to select a method while retaining
    // the exact stdcall stack cleanup encoded by its typelib signature.
    for (u16 physical_slots = 1; physical_slots <= 32; ++physical_slots) {
        exports.entries.push_back(ExportEntry{
            "Vitreo_RemoteProxy_Invoke_" + std::to_string(physical_slots),
            0,
            [&mgr](Marshaler& m) {
                m.set_return(mgr.remote_proxy_invoke(m));
            },
            static_cast<u16>(physical_slots * 4u)});
    }
}

/// ShellLink and IGlobalOptions vtable methods.
void append_com_object_vtable_thunks(DllExports& exports, ComCoreManager& mgr) {
    // Internal COM method thunks used by ShellLink vtables
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_GlobalOptions_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.global_options_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_GlobalOptions_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.global_options_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_GlobalOptions_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.global_options_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_GlobalOptions_Set", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.global_options_set(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_GlobalOptions_Query", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.global_options_query(m)); }, 12});
    mgr.item_dialogs().append_thunks(exports);
}

/// ole32 completions: FTM, agile references, PROPVARIANT copy, HGLOBAL
/// streams, object context, IComThreadingInfo.
void append_com_completion_thunks(DllExports& exports, ComCoreManager& mgr) {
    // CoGetObjectContext (2 args, stdcall_bytes=8). Delay-loaded. A missing
    // export makes the MSVC delay-load helper raise 0xC06D007F
    // (ERROR_PROC_NOT_FOUND) and kill the process.
    // -- ole32 completions -------------------------------------------------
    // CoAllowSetForegroundWindow: an in-process guest already owns its
    // windows, so the permission it transfers is always already held.
    exports.entries.push_back(ExportEntry{
        "CoAllowSetForegroundWindow", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            (void)m.arg_ptr(1);
            m.set_return(com::S_OK);
        }, 8});
    // CoCreateFreeThreadedMarshaler: the free-threaded marshaler exists to let
    // an object be used from any apartment without proxying. Vitreo runs every
    // apartment in one address space, so the aggregated inner object only has
    // to be a valid IUnknown that reports agility; a caller that never
    // marshals (the common case) then behaves exactly as on Windows. Callers
    // that DO marshal reach IMarshal through QueryInterface, which this inner
    // object honestly declines.
    exports.entries.push_back(ExportEntry{
        "CoCreateFreeThreadedMarshaler", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.co_create_free_threaded_marshaler(m));
        }, 8});
    // RoGetAgileReference: an agile reference resolves back to the original
    // pointer, which is correct when there is no cross-apartment proxying.
    exports.entries.push_back(ExportEntry{
        "RoGetAgileReference", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.ro_get_agile_reference(m)); },
        16});
    // PropVariantCopy: a byte copy is right for every by-value PROPVARIANT
    // type; the pointer-bearing ones need their payload duplicated, which
    // this refuses rather than aliasing into the source.
    exports.entries.push_back(ExportEntry{
        "PropVariantCopy", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.prop_variant_copy(m)); }, 8});
    // HGLOBAL-backed ILockBytes / IStream accessors. Vitreo's memory streams
    // already carry their bytes in guest memory, so the HGLOBAL a caller asks
    // for is the stream's own buffer.
    exports.entries.push_back(ExportEntry{
        "GetHGlobalFromStream", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.get_hglobal_from_stream(m)); },
        8});
    exports.entries.push_back(ExportEntry{
        "CreateILockBytesOnHGlobal", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(2);
            if (out != 0) (void)m.write_ptr(out, 0);
            VITREO_TRACE(
                "[ole32] CreateILockBytesOnHGlobal E_NOTIMPL (ILockBytes byte "
                "arrays are not modelled)\n");
            m.set_return(com::E_NOTIMPL);
        }, 12});
    exports.entries.push_back(ExportEntry{
        "GetHGlobalFromILockBytes", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(1);
            if (out != 0) (void)m.write_ptr(out, 0);
            m.set_return(com::E_NOTIMPL);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "StgCreateDocfileOnILockBytes", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(3);
            if (out != 0) (void)m.write_ptr(out, 0);
            VITREO_TRACE(
                "[ole32] StgCreateDocfileOnILockBytes E_NOTIMPL (needs an "
                "ILockBytes byte array)\n");
            m.set_return(com::E_NOTIMPL);
        }, 16});
    exports.entries.push_back(ExportEntry{
        "CoGetObjectContext", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.co_get_object_context(m)); },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FreeThreadedMarshaler_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.ftm_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FreeThreadedMarshaler_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.ftm_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FreeThreadedMarshaler_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.ftm_release(m)); }, 4});
    // The FTM's IMarshal vtable (objidl.h IMarshalVtbl order).
    {
        struct Slot { const char* name; u32 slot; u16 bytes; };
        static constexpr Slot kSlots[] = {
            {"Vitreo_FtmMarshal_QueryInterface", 0, 12},
            {"Vitreo_FtmMarshal_AddRef", 1, 4},
            {"Vitreo_FtmMarshal_Release", 2, 4},
            {"Vitreo_FtmMarshal_GetUnmarshalClass", 3, 28},
            {"Vitreo_FtmMarshal_GetMarshalSizeMax", 4, 28},
            {"Vitreo_FtmMarshal_MarshalInterface", 5, 28},
            {"Vitreo_FtmMarshal_UnmarshalInterface", 6, 16},
            {"Vitreo_FtmMarshal_ReleaseMarshalData", 7, 8},
            {"Vitreo_FtmMarshal_DisconnectObject", 8, 8},
        };
        for (const Slot& slot : kSlots) {
            const u32 index = slot.slot;
            exports.entries.push_back(ExportEntry{slot.name, 0,
                [&mgr, index](Marshaler& m) {
                    m.set_return(mgr.ftm_marshal_method(m, index));
                },
                static_cast<u16>(slot.bytes)});
        }
    }
    exports.entries.push_back(ExportEntry{
        "Vitreo_AgileReference_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.agile_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AgileReference_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.agile_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AgileReference_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.agile_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AgileReference_Resolve", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.agile_resolve(m)); }, 12});
    // DWORD CoGetCurrentProcess(void) (Learn): a value unique to the calling
    // thread that, unlike a thread id, is never reused. Measured
    // (cogetcurrentprocess_probe{32,64}.expected): assigned on a thread's
    // first call from an increasing counter, the same for every later call
    // on that thread, independent of CoInitialize/CoUninitialize. Guest
    // threads run on their own host threads.
    exports.entries.push_back(ExportEntry{
        "CoGetCurrentProcess", 0,
        [](Marshaler& m) {
            static std::atomic<u32> s_next{1};
            thread_local u32 t_value = 0;
            if (t_value == 0) t_value = s_next.fetch_add(1);
            m.set_return(t_value);
        },
        0});
    exports.entries.push_back(ExportEntry{
        "CoGetApartmentType", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.co_get_apartment_type(m)); },
        8});
    // HRESULT CoGetContextToken(ULONG_PTR* pToken). Called by .NET's COM
    // interop; previously an unimplemented-export stub that returned
    // 0xFFFFFFFF and never touched the out-parameter.
    exports.entries.push_back(ExportEntry{
        "CoGetContextToken", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.co_get_context_token(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_add_ref(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_release(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_GetCurrentApartmentType", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_get_apartment_type(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_GetCurrentThreadType", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_get_thread_type(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_GetCurrentLogicalThreadId", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_get_logical_thread_id(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_ComThreadingInfo_SetCurrentLogicalThreadId", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.com_threading_info_set_logical_thread_id(m));
        }, 8});
}

/// IShellLink and IPersistFile vtable methods.
void append_com_shelllink_thunks(DllExports& exports, ComCoreManager& mgr) {
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetPath", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_path(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetArguments", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_arguments(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetWorkingDirectory", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_working_directory(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetDescription", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_description(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetIconLocation", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_icon_location(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetShowCmd", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_set_show_cmd(m)); }, 8});

    // Stdcall-size-correct no-op methods for IShellLinkW methods we don't need yet.
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetPath", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_get_path(m)); }, 20});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetIDList", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetIDList", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetDescription", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetWorkingDirectory", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetArguments", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetHotkey", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetHotkey", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetShowCmd", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_GetIconLocation", 0,
        [](Marshaler& m) { m.set_return(com::S_FALSE); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_SetRelativePath", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_Resolve", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 12});

    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistLoad", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_load(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistSave", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_save(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistIsDirty", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_is_dirty(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistGetClassID", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_get_class_id(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistSaveCompleted", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_save_completed(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PersistGetCurFile", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_persist_get_cur_file(m)); }, 8});
    // IPropertyStore on CLSID_ShellLink (propsys.h vtable order after
    // IUnknown: GetCount, GetAt, GetValue, SetValue, Commit).
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PropGetCount", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_propstore_get_count(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PropGetAt", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_propstore_get_at(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PropGetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_propstore_get_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PropSetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_propstore_set_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ShellLink_PropCommit", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.shelllink_propstore_commit(m)); }, 4});
    // SHGetPropertyStoreForWindow's IPropertyStore (IUnknown, then GetCount,
    // GetAt, GetValue, SetValue, Commit).
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_GetCount", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_get_count(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_GetAt", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_get_at(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_GetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_get_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_SetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_set_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_WindowPropertyStore_Commit", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.window_propstore_commit(m)); }, 4});
}

/// SHCreateMemStream IStream vtable methods.
void append_com_memory_stream_thunks(DllExports& exports, ComCoreManager& mgr) {
    // Internal COM method thunks used by SHCreateMemStream's IStream vtable.
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_query_interface(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_add_ref(m); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Release", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_release(m); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Read", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_read(m); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Write", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_write(m); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Seek", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_seek(m); }, 20});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_SetSize", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_set_size(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_CopyTo", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_copy_to(m); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Commit", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_commit(m); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Revert", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_revert(m); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_LockRegion", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_lock_region(m); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_UnlockRegion", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_unlock_region(m); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Stat", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_stat(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_MemoryStream_Clone", 0,
        [&mgr](Marshaler& m) { mgr.memory_stream_clone(m); }, 8});
}

} // namespace vitreo::winapi
