// SPDX-License-Identifier: LGPL-2.1-only

#include "com_core.hpp"
#include "filter_graph.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "winapi/common/elevation.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

/// @file ole_stream_persistence_stubs.cpp
/// @brief OLE stream persistence stubs
///
/// Closed cut from com_core.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// ----- OLE stream persistence stubs

void ComCoreManager::impl_OleSaveToStream(Marshaler& m) {
    constexpr u32 E_NOTIMPL = 0x80004001u;
    constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
    constexpr u32 OLE_E_BLANK = 0x80040007u;
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;

    const GuestAddr persist = m.arg_ptr(0);
    const GuestAddr stream = m.arg_ptr(1);
    if (persist == 0) {
        m.set_return(OLE_E_BLANK);
        return;
    }
    if (stream == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    if (!m_guest_call) {
        m.set_return(E_NOTIMPL);
        return;
    }

    const GuestAddr clsid = co_task_mem_alloc_guest(sizeof(Guid));
    if (clsid == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    auto method_at = [&](GuestAddr object, usize slot) -> GuestAddr {
        const auto vtable = m.read_ptr(object);
        if (!vtable.is_ok() || vtable.value() == 0) return 0;
        const auto method = m.read_ptr(
            static_cast<GuestAddr>(vtable.value()) + slot * pointer_size);
        return method.is_ok()
            ? static_cast<GuestAddr>(method.value())
            : 0;
    };
    auto failed = [](u32 hr) { return (hr & 0x80000000u) != 0; };

    const GuestAddr get_class_id = method_at(persist, 3);
    const GuestAddr write = method_at(stream, 4);
    const GuestAddr save = method_at(persist, 6);
    if (get_class_id == 0 || write == 0 || save == 0) {
        co_task_mem_free_guest(clsid);
        m.set_return(com::E_INVALIDARG);
        return;
    }

    const u64 get_class_args[2] = {persist, clsid};
    u32 hr = static_cast<u32>(m_guest_call(
        get_class_id, get_class_args, 2, m.cpu_is_64bit()));
    if (!failed(hr)) {
        const u64 write_args[4] = {stream, clsid, sizeof(Guid), 0};
        hr = static_cast<u32>(m_guest_call(
            write, write_args, 4, m.cpu_is_64bit()));
    }
    if (!failed(hr)) {
        const u64 save_args[3] = {persist, stream, 1};
        hr = static_cast<u32>(m_guest_call(
            save, save_args, 3, m.cpu_is_64bit()));
    }
    co_task_mem_free_guest(clsid);
    VITREO_TRACE(
        "[ole32] OleSaveToStream(persist=0x%llx, stream=0x%llx) "
        "-> 0x%08x\n",
        static_cast<unsigned long long>(persist),
        static_cast<unsigned long long>(stream), hr);
    m.set_return(hr);
}

void ComCoreManager::impl_WriteClassStm(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    const GuestAddr stream = m.arg_ptr(0);
    const GuestAddr clsid = m.arg_ptr(1);
    if (stream == 0 || clsid == 0 || !m_guest_call) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    const auto vtable = m.read_ptr(stream);
    if (vtable.is_err() || vtable.value() == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto write = m.read_ptr(
        static_cast<GuestAddr>(vtable.value()) + 4 * pointer_size);
    if (write.is_err() || write.value() == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    const u64 args[4] = {stream, clsid, sizeof(Guid), 0};
    const u32 hr = static_cast<u32>(m_guest_call(
        static_cast<GuestAddr>(write.value()), args, 4, m.cpu_is_64bit()));
    VITREO_TRACE("[ole32] WriteClassStm(stream=0x%llx) -> 0x%08x\n",
                 static_cast<unsigned long long>(stream), hr);
    m.set_return(hr);
}

void ComCoreManager::impl_OleLoadFromStream(Marshaler& m) {
    constexpr u32 E_NOTIMPL = 0x80004001u;
    constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    constexpr Guid IID_IPERSISTSTREAM{
        0x00000109u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    const GuestAddr stream = m.arg_ptr(0);
    const GuestAddr requested_iid = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (stream == 0 || requested_iid == 0 || out_ptr == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);
    if (!m_guest_call || m_thunks == nullptr) {
        m.set_return(E_NOTIMPL);
        return;
    }

    const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr scratch = co_task_mem_alloc_guest(32 + pointer_size);
    if (scratch == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const GuestAddr clsid = scratch;
    const GuestAddr persist_iid = scratch + 16;
    const GuestAddr persist_out = scratch + 32;
    const std::array<u8, 16> zero_clsid{};
    (void)m.memory().write_bytes(clsid, zero_clsid.data(), zero_clsid.size());
    (void)m.write_struct<Guid>(persist_iid, IID_IPERSISTSTREAM);
    (void)m.write_ptr(persist_out, 0);

    auto method_at = [&](GuestAddr object, usize slot) -> GuestAddr {
        const auto vtable = m.read_ptr(object);
        if (!vtable.is_ok() || vtable.value() == 0) return 0;
        const auto method = m.read_ptr(
            static_cast<GuestAddr>(vtable.value()) + slot * pointer_size);
        return method.is_ok()
            ? static_cast<GuestAddr>(method.value())
            : 0;
    };
    auto failed = [](u32 hr) { return (hr & 0x80000000u) != 0; };

    const GuestAddr read = method_at(stream, 3);
    if (read == 0) {
        co_task_mem_free_guest(scratch);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const u64 read_args[4] = {stream, clsid, sizeof(Guid), 0};
    u32 hr = static_cast<u32>(m_guest_call(
        read, read_args, 4, m.cpu_is_64bit()));
    if (hr != com::S_OK) {
        co_task_mem_free_guest(scratch);
        m.set_return(hr);
        return;
    }

    const auto loaded_clsid = m.read_struct<Guid>(clsid);
    if (loaded_clsid.is_err()) {
        co_task_mem_free_guest(scratch);
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    if (std::memcmp(&loaded_clsid.value(), zero_clsid.data(),
                    zero_clsid.size()) == 0) {
        co_task_mem_free_guest(scratch);
        m.set_return(com::S_OK);
        return;
    }

    const auto co_create =
        m_thunks->get_thunk_address("ole32.dll", "CoCreateInstance");
    if (co_create.is_err()) {
        co_task_mem_free_guest(scratch);
        m.set_return(com::CLASS_E_CLASSNOTAVAILABLE);
        return;
    }
    const u64 create_args[5] = {clsid, 0, 1, persist_iid, persist_out};
    hr = static_cast<u32>(m_guest_call(
        co_create.value(), create_args, 5, m.cpu_is_64bit()));
    const GuestAddr persist = m.read_ptr(persist_out).value_or(0);
    if (failed(hr) || persist == 0) {
        co_task_mem_free_guest(scratch);
        m.set_return(failed(hr) ? hr : com::E_INVALIDARG);
        return;
    }

    const GuestAddr load = method_at(persist, 5);
    if (load == 0) {
        guest_iunknown_release(m, persist);
        co_task_mem_free_guest(scratch);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const u64 load_args[2] = {persist, stream};
    hr = static_cast<u32>(m_guest_call(
        load, load_args, 2, m.cpu_is_64bit()));
    if (!failed(hr)) {
        hr = guest_iunknown_qi(m, persist, requested_iid, out_ptr);
    }
    guest_iunknown_release(m, persist);
    co_task_mem_free_guest(scratch);
    VITREO_TRACE(
        "[ole32] OleLoadFromStream(stream=0x%llx) -> 0x%08x\n",
        static_cast<unsigned long long>(stream), hr);
    m.set_return(hr);
}

bool ComCoreManager::monikers_equal(
    Marshaler& m, GuestAddr left, GuestAddr right) {
    if (left == right) return true;
    if (left == 0 || right == 0 || !m_guest_call) return false;
    const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto vtable = m.read_ptr(left);
    if (!vtable.is_ok() || vtable.value() == 0) return false;
    const auto is_equal = m.read_ptr(
        static_cast<GuestAddr>(vtable.value()) + 13 * pointer_size);
    if (!is_equal.is_ok() || is_equal.value() == 0) return false;
    const u64 args[2] = {left, right};
    return static_cast<u32>(m_guest_call(
        static_cast<GuestAddr>(is_equal.value()), args, 2,
        m.cpu_is_64bit())) == com::S_OK;
}

void ComCoreManager::impl_GetRunningObjectTable(Marshaler& m) {
    constexpr u32 E_UNEXPECTED = 0x8000FFFFu;
    const u32 reserved = static_cast<u32>(m.arg(0));
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (reserved != 0) {
        m.set_return(E_UNEXPECTED);
        return;
    }
    if (out_ptr == 0 || m_thunks == nullptr) {
        m.set_return(com::E_INVALIDARG);
        return;
    }

    std::lock_guard lock(m_running_object_table_mutex);
    if (m_running_object_table == 0) {
        static constexpr std::array<std::string_view, 10> methods{{
            "Vitreo_ROT_QueryInterface", "Vitreo_ROT_AddRef",
            "Vitreo_ROT_Release", "Vitreo_ROT_Register",
            "Vitreo_ROT_Revoke", "Vitreo_ROT_IsRunning",
            "Vitreo_ROT_GetObject", "Vitreo_ROT_NoteChangeTime",
            "Vitreo_ROT_GetTimeOfLastChange", "Vitreo_ROT_EnumRunning"}};
        const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr object = co_task_mem_alloc_guest(
            pointer_size * (methods.size() + 1));
        if (object == 0) {
            m.set_return(0x8007000Eu);
            return;
        }
        const GuestAddr vtable = object + pointer_size;
        if (m.write_ptr(object, vtable).is_err()) {
            m.set_return(com::E_INVALIDARG);
            return;
        }
        for (usize i = 0; i < methods.size(); ++i) {
            auto thunk = m_thunks->get_thunk_address("ole32.dll", methods[i]);
            if (thunk.is_err() ||
                m.write_ptr(vtable + i * pointer_size, thunk.value()).is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
        }
        m_running_object_table = object;
    }
    ++m_running_object_table_refs;
    if (m.write_ptr(out_ptr, m_running_object_table).is_err()) {
        --m_running_object_table_refs;
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::running_object_table_query_interface(Marshaler& m) {
    constexpr Guid IID_IROT{0x00000010u, 0, 0,
        {0xC0,0,0,0,0,0,0,0x46}};
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (m.arg_ptr(0) != m_running_object_table || iid.is_err() || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    if (!guid_equal(iid.value(), IID_IUNKNOWN) &&
        !guid_equal(iid.value(), IID_IROT)) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(0x80004002u);
        return;
    }
    std::lock_guard lock(m_running_object_table_mutex);
    ++m_running_object_table_refs;
    (void)m.write_ptr(out_ptr, m_running_object_table);
    m.set_return(com::S_OK);
}

void ComCoreManager::running_object_table_add_ref(Marshaler& m) {
    std::lock_guard lock(m_running_object_table_mutex);
    m.set_return(++m_running_object_table_refs);
}

void ComCoreManager::running_object_table_release(Marshaler& m) {
    std::lock_guard lock(m_running_object_table_mutex);
    if (m_running_object_table_refs > 1) --m_running_object_table_refs;
    m.set_return(m_running_object_table_refs);
}

void ComCoreManager::running_object_table_register(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(2);
    const GuestAddr moniker = m.arg_ptr(3);
    const GuestAddr cookie_out = m.arg_ptr(4);
    if (object == 0 || moniker == 0 || cookie_out == 0 || !m_guest_call) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)guest_iunknown_addref(m, object);
    (void)guest_iunknown_addref(m, moniker);
    std::string display_name;
    {
        std::lock_guard moniker_lock(m_item_monikers_mutex);
        const auto item = m_item_monikers.find(moniker);
        if (item != m_item_monikers.end()) {
            display_name = item->second.delimiter + item->second.item;
        }
    }
    u32 cookie = 0;
    {
        std::lock_guard lock(m_running_object_table_mutex);
        cookie = m_next_running_object_cookie++;
    }
    std::shared_ptr<ipc::RotObjectRegistration> shared_registration;
    if (!m_capsule_root.empty() && !display_name.empty()) {
        shared_registration = std::make_shared<ipc::RotObjectRegistration>(
            m_capsule_root, display_name, cookie, object,
            m.cpu_is_64bit() ? 8u : 4u);
    }
    {
        std::lock_guard lock(m_running_object_table_mutex);
        m_running_objects.push_back(RunningObjectEntry{
            cookie, object, moniker, {}, std::move(shared_registration)});
    }
    (void)m.write_u32(cookie_out, cookie);
    ipc::ipc_log(
        "ROT Register cookie=%u object=0x%llx moniker='%s' shared=%d",
        cookie, static_cast<unsigned long long>(object), display_name.c_str(),
        !display_name.empty() ? 1 : 0);
    m.set_return(com::S_OK);
}

void ComCoreManager::running_object_table_revoke(Marshaler& m) {
    const u32 cookie = static_cast<u32>(m.arg(1));
    RunningObjectEntry removed{};
    {
        std::lock_guard lock(m_running_object_table_mutex);
        auto it = std::find_if(m_running_objects.begin(), m_running_objects.end(),
            [cookie](const auto& entry) { return entry.cookie == cookie; });
        if (it == m_running_objects.end()) {
            m.set_return(0x800401E3u);
            return;
        }
        removed = *it;
        m_running_objects.erase(it);
    }
    guest_iunknown_release(m, removed.object);
    guest_iunknown_release(m, removed.moniker);
    m.set_return(com::S_OK);
}

void ComCoreManager::running_object_table_is_running(Marshaler& m) {
    const GuestAddr moniker = m.arg_ptr(1);
    std::vector<GuestAddr> monikers;
    {
        std::lock_guard lock(m_running_object_table_mutex);
        for (const auto& entry : m_running_objects) monikers.push_back(entry.moniker);
    }
    const bool found = std::any_of(monikers.begin(), monikers.end(),
        [&](GuestAddr candidate) { return monikers_equal(m, candidate, moniker); });
    m.set_return(found ? com::S_OK : com::S_FALSE);
}

void ComCoreManager::running_object_table_get_object(Marshaler& m) {
    const GuestAddr moniker = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (moniker == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);
    std::string display_name;
    {
        std::lock_guard moniker_lock(m_item_monikers_mutex);
        const auto item = m_item_monikers.find(moniker);
        if (item != m_item_monikers.end()) {
            display_name = item->second.delimiter + item->second.item;
        }
    }
    ipc::ipc_log("ROT GetObject moniker='%s'", display_name.c_str());
    std::vector<RunningObjectEntry> entries;
    {
        std::lock_guard lock(m_running_object_table_mutex);
        entries = m_running_objects;
    }
    for (const auto& entry : entries) {
        if (!monikers_equal(m, entry.moniker, moniker)) continue;
        (void)guest_iunknown_addref(m, entry.object);
        (void)m.write_ptr(out_ptr, entry.object);
        m.set_return(com::S_OK);
        return;
    }

    ipc::RotObjectSnapshot remote{};
    if (!display_name.empty() &&
        ipc::find_rot_object(m_capsule_root, display_name, remote) &&
        remote.owner_pid != static_cast<u32>(::getpid()) &&
        ipc::remote_process_endpoint_available(
            static_cast<pid_t>(remote.owner_pid))) {
        GuestAddr vtable = 0;
        auto vtable_bytes = std::span<Byte>(
            reinterpret_cast<Byte*>(&vtable), remote.pointer_size);
        if (ipc::remote_read_process_memory(
                static_cast<pid_t>(remote.owner_pid), remote.object,
                vtable_bytes) &&
            vtable != 0) {
            if (remote.pointer_size == 4) vtable &= 0xFFFFFFFFu;
            GuestAddr add_ref = 0;
            auto add_ref_bytes = std::span<Byte>(
                reinterpret_cast<Byte*>(&add_ref), remote.pointer_size);
            if (ipc::remote_read_process_memory(
                    static_cast<pid_t>(remote.owner_pid),
                    vtable + remote.pointer_size, add_ref_bytes) &&
                add_ref != 0) {
                if (remote.pointer_size == 4) add_ref &= 0xFFFFFFFFu;
                const std::array<u64, 1> args{remote.object};
                const auto add_ref_result = ipc::remote_call_guest(
                    static_cast<pid_t>(remote.owner_pid), add_ref, args,
                    m_elevation != nullptr && m_elevation->is_elevated());
                if (add_ref_result) {
                    ipc::ComClassSnapshot owner{
                        remote.owner_pid, remote.cookie, remote.object, 0, 0,
                        remote.pointer_size};
                    const GuestAddr proxy = create_remote_iunknown_proxy(
                        m, owner, remote.object, IID_IUNKNOWN);
                    if (proxy != 0 && m.write_ptr(out_ptr, proxy).is_ok()) {
                        ipc::ipc_log(
                            "ROT GetObject remote owner=%u object=0x%llx "
                            "proxy=0x%llx",
                            remote.owner_pid,
                            static_cast<unsigned long long>(remote.object),
                            static_cast<unsigned long long>(proxy));
                        m.set_return(com::S_OK);
                        return;
                    }
                }
            }
        }
    }
    m.set_return(0x800401E3u);
}

void ComCoreManager::running_object_table_note_change_time(Marshaler& m) {
    const u32 cookie = static_cast<u32>(m.arg(1));
    const GuestAddr time_ptr = m.arg_ptr(2);
    std::array<u8, 8> value{};
    if (time_ptr == 0 || m.memory().read_bytes(time_ptr, value.data(), 8).is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    std::lock_guard lock(m_running_object_table_mutex);
    auto it = std::find_if(m_running_objects.begin(), m_running_objects.end(),
        [cookie](const auto& entry) { return entry.cookie == cookie; });
    if (it == m_running_objects.end()) {
        m.set_return(0x800401E3u);
        return;
    }
    it->last_change = value;
    m.set_return(com::S_OK);
}

void ComCoreManager::running_object_table_get_time_of_last_change(Marshaler& m) {
    const GuestAddr moniker = m.arg_ptr(1);
    const GuestAddr time_ptr = m.arg_ptr(2);
    std::vector<RunningObjectEntry> entries;
    {
        std::lock_guard lock(m_running_object_table_mutex);
        entries = m_running_objects;
    }
    for (const auto& entry : entries) {
        if (!monikers_equal(m, entry.moniker, moniker)) continue;
        const auto result = m.memory().write_bytes(
            time_ptr, entry.last_change.data(), entry.last_change.size());
        m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
        return;
    }
    m.set_return(0x800401E3u);
}

void ComCoreManager::running_object_table_enum_running(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    m.set_return(0x80004001u);
}

void ComCoreManager::impl_CreateItemMoniker(Marshaler& m) {
    const GuestAddr delimiter_ptr = m.arg_ptr(0);
    const GuestAddr item_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0 || m_thunks == nullptr) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);
    if (delimiter_ptr == 0 || item_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    auto delimiter = m.read_wide_string(delimiter_ptr);
    auto item = m.read_wide_string(item_ptr);
    if (delimiter.is_err() || item.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }

    static constexpr std::array<std::string_view, 23> methods{{
        "Vitreo_ItemMoniker_QueryInterface", "Vitreo_ItemMoniker_AddRef",
        "Vitreo_ItemMoniker_Release", "Vitreo_ItemMoniker_GetClassID",
        "Vitreo_ItemMoniker_IsDirty", "Vitreo_ItemMoniker_NotImpl1",
        "Vitreo_ItemMoniker_NotImpl1", "Vitreo_ItemMoniker_NotImpl1",
        "Vitreo_ItemMoniker_NotImpl4", "Vitreo_ItemMoniker_NotImpl4",
        "Vitreo_ItemMoniker_NotImpl4", "Vitreo_ItemMoniker_NotImpl3",
        "Vitreo_ItemMoniker_NotImpl2", "Vitreo_ItemMoniker_IsEqual",
        "Vitreo_ItemMoniker_Hash", "Vitreo_ItemMoniker_NotImpl3",
        "Vitreo_ItemMoniker_NotImpl3", "Vitreo_ItemMoniker_NotImpl1",
        "Vitreo_ItemMoniker_NotImpl2", "Vitreo_ItemMoniker_NotImpl2",
        "Vitreo_ItemMoniker_GetDisplayName", "Vitreo_ItemMoniker_NotImpl5",
        "Vitreo_ItemMoniker_IsSystemMoniker"}};
    const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = co_task_mem_alloc_guest(
        pointer_size * (methods.size() + 1));
    if (object == 0) {
        m.set_return(0x8007000Eu);
        return;
    }
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) {
        co_task_mem_free_guest(object);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    for (usize i = 0; i < methods.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("ole32.dll", methods[i]);
        if (thunk.is_err() ||
            m.write_ptr(vtable + i * pointer_size, thunk.value()).is_err()) {
            co_task_mem_free_guest(object);
            m.set_return(com::E_INVALIDARG);
            return;
        }
    }
    {
        std::lock_guard lock(m_item_monikers_mutex);
        m_item_monikers.emplace(object, ItemMonikerState{
            1, std::move(delimiter.value()), std::move(item.value())});
    }
    if (m.write_ptr(out_ptr, object).is_err()) {
        std::lock_guard lock(m_item_monikers_mutex);
        m_item_monikers.erase(object);
        co_task_mem_free_guest(object);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::item_moniker_method(Marshaler& m, u32 slot) {
    constexpr u32 E_NOINTERFACE_HR = 0x80004002u;
    constexpr u32 E_NOTIMPL = 0x80004001u;
    constexpr Guid IID_IPERSIST{0x0000010Cu, 0, 0,
        {0xC0,0,0,0,0,0,0,0x46}};
    constexpr Guid IID_IPERSISTSTREAM{0x00000109u, 0, 0,
        {0xC0,0,0,0,0,0,0,0x46}};
    constexpr Guid IID_IMONIKER{0x0000000Fu, 0, 0,
        {0xC0,0,0,0,0,0,0,0x46}};
    constexpr Guid CLSID_ITEMMONIKER{0x00000304u, 0, 0,
        {0xC0,0,0,0,0,0,0,0x46}};
    const GuestAddr object = m.arg_ptr(0);

    std::unique_lock lock(m_item_monikers_mutex);
    auto found = m_item_monikers.find(object);
    if (found == m_item_monikers.end()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    ItemMonikerState& state = found->second;
    if (slot == 0) {
        const GuestAddr iid_ptr = m.arg_ptr(1);
        const GuestAddr out_ptr = m.arg_ptr(2);
        auto iid = m.read_struct<Guid>(iid_ptr);
        if (iid.is_err() || out_ptr == 0) {
            m.set_return(com::E_INVALIDARG);
            return;
        }
        if (!guid_equal(iid.value(), IID_IUNKNOWN) &&
            !guid_equal(iid.value(), IID_IPERSIST) &&
            !guid_equal(iid.value(), IID_IPERSISTSTREAM) &&
            !guid_equal(iid.value(), IID_IMONIKER)) {
            (void)m.write_ptr(out_ptr, 0);
            m.set_return(E_NOINTERFACE_HR);
            return;
        }
        ++state.ref_count;
        (void)m.write_ptr(out_ptr, object);
        m.set_return(com::S_OK);
        return;
    }
    if (slot == 1) {
        m.set_return(++state.ref_count);
        return;
    }
    if (slot == 2) {
        const u32 refs = --state.ref_count;
        if (refs == 0) {
            m_item_monikers.erase(found);
            lock.unlock();
            co_task_mem_free_guest(object);
        }
        m.set_return(refs);
        return;
    }
    if (slot == 3) {
        const GuestAddr clsid_ptr = m.arg_ptr(1);
        const auto result = m.write_struct<Guid>(clsid_ptr, CLSID_ITEMMONIKER);
        m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
        return;
    }
    if (slot == 4) {
        m.set_return(com::S_FALSE);
        return;
    }
    if (slot == 13) {
        auto other = m_item_monikers.find(m.arg_ptr(1));
        if (other == m_item_monikers.end()) {
            m.set_return(com::S_FALSE);
            return;
        }
        const std::string display = state.delimiter + state.item;
        const std::string other_display =
            other->second.delimiter + other->second.item;
        const bool equal = display.size() == other_display.size() &&
            std::equal(display.begin(), display.end(), other_display.begin(),
                [](char left, char right) {
                    return std::tolower(static_cast<unsigned char>(left)) ==
                           std::tolower(static_cast<unsigned char>(right));
                });
        m.set_return(equal ? com::S_OK : com::S_FALSE);
        return;
    }
    if (slot == 14) {
        const GuestAddr hash_ptr = m.arg_ptr(1);
        u32 hash = 2166136261u;
        for (char character : state.delimiter + state.item) {
            const auto byte = static_cast<unsigned char>(character);
            hash ^= static_cast<u8>(std::tolower(byte));
            hash *= 16777619u;
        }
        const auto result = m.write_u32(hash_ptr, hash);
        m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
        return;
    }
    if (slot == 20) {
        const GuestAddr out_ptr = m.arg_ptr(3);
        if (out_ptr == 0) {
            m.set_return(com::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(out_ptr, 0);
        const std::string display = state.delimiter + state.item;
        const GuestAddr buffer = co_task_mem_alloc_guest(
            (display.size() + 1) * sizeof(u16));
        if (buffer == 0) {
            m.set_return(0x8007000Eu);
            return;
        }
        if (m.write_wide_string(buffer, display).is_err() ||
            m.write_ptr(out_ptr, buffer).is_err()) {
            co_task_mem_free_guest(buffer);
            m.set_return(com::E_INVALIDARG);
            return;
        }
        m.set_return(com::S_OK);
        return;
    }
    if (slot == 22) {
        const auto result = m.write_u32(m.arg_ptr(1), 4u);
        m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
        return;
    }
    m.set_return(E_NOTIMPL);
}

} // namespace vitreo::winapi
