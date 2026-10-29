// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_remote_proxy.cpp
/// @brief Interface proxies for COM objects living in another process: creation,
/// IUnknown forwarding and the IDispatch type-info/GetIDsOfNames methods.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/common/elevation.hpp"
#include "com_core_internal.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

GuestAddr ComCoreManager::create_remote_iunknown_proxy(
    Marshaler& m, const ipc::ComClassSnapshot& owner,
    GuestAddr remote_object, const Guid& iid,
    const AutomationInterfaceLayout* layout,
    GuestAddr identity_object) {
    if (m_guest_task_memory == nullptr || m_thunks == nullptr ||
        remote_object == 0) {
        return 0;
    }
    const auto qi = m_thunks->get_thunk_address(
        "ole32.dll", "Vitreo_RemoteProxy_QueryInterface");
    const auto add_ref = m_thunks->get_thunk_address(
        "ole32.dll", "Vitreo_RemoteProxy_AddRef");
    const auto release = m_thunks->get_thunk_address(
        "ole32.dll", "Vitreo_RemoteProxy_Release");
    if (qi.is_err() || add_ref.is_err() || release.is_err()) return 0;

    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    u32 method_count = guid_equal(iid, IID_IDISPATCH) ? 7u : 3u;
    if (layout != nullptr) {
        for (const auto& method : layout->methods) {
            if (method.vtable_offset % pointer_size != 0 ||
                method.parameter_vts.size() != method.parameter_count ||
                method.parameter_flags.size() != method.parameter_count) {
                return 0;
            }
            method_count = std::max(
                method_count,
                static_cast<u32>(method.vtable_offset / pointer_size) + 1u);
        }
    }
    const GuestAddr allocation = m_guest_task_memory->allocate(
        pointer_size * (1u + method_count));
    if (allocation == 0) return 0;
    const GuestAddr object = allocation;
    const GuestAddr vtable = allocation + pointer_size;
    if (m.write_ptr(object, vtable).is_err() ||
        m.write_ptr(vtable, qi.value()).is_err() ||
        m.write_ptr(vtable + pointer_size, add_ref.value()).is_err() ||
        m.write_ptr(vtable + 2u * pointer_size, release.value()).is_err()) {
        m_guest_task_memory->free(allocation);
        return 0;
    }
    if (guid_equal(iid, IID_IDISPATCH)) {
        static constexpr std::array<std::string_view, 4> dispatch_methods{{
            "Vitreo_RemoteDispatch_GetTypeInfoCount",
            "Vitreo_RemoteDispatch_GetTypeInfo",
            "Vitreo_RemoteDispatch_GetIDsOfNames",
            "Vitreo_RemoteDispatch_Invoke"}};
        for (usize index = 0; index < dispatch_methods.size(); ++index) {
            const auto method = m_thunks->get_thunk_address(
                "ole32.dll", dispatch_methods[index]);
            if (method.is_err() ||
                m.write_ptr(
                    vtable + (index + 3u) * pointer_size,
                    method.value()).is_err()) {
                m_guest_task_memory->free(allocation);
                return 0;
            }
        }
    }

    std::lock_guard lock(m_remote_proxies_mutex);
    m_remote_proxies.emplace(object, RemoteInterfaceProxy{
        owner.owner_pid, owner.pointer_size, remote_object, iid, 1, vtable,
        identity_object != 0 ? identity_object : object});
    const auto discard_proxy = [&] {
        m_remote_proxies.erase(object);
        std::erase_if(m_remote_methods, [object](const auto& entry) {
            return entry.second.proxy_object == object;
        });
        m_guest_task_memory->free(allocation);
    };
    if (layout != nullptr) {
        for (const auto& method : layout->methods) {
            u32 physical_slots = 1;
            for (usize parameter = 0;
                 parameter < method.parameter_vts.size(); ++parameter) {
                const bool indirect =
                    (method.parameter_flags[parameter] & 0x000Au) != 0;
                physical_slots += indirect ? 1u : automation_value_slots(
                    method.parameter_vts[parameter], m.cpu_is_64bit());
            }
            if (physical_slots == 0 || physical_slots > 32) {
                discard_proxy();
                return 0;
            }
            const std::string gateway_name =
                "Vitreo_RemoteProxy_Invoke_" +
                std::to_string(physical_slots);
            auto gateway = m_thunks->get_thunk_address(
                "ole32.dll", gateway_name);
            if (gateway.is_err()) {
                discard_proxy();
                return 0;
            }
            const GuestAddr alias = m_next_remote_method_alias;
            m_next_remote_method_alias += 16;
            if (!m_thunks->register_dispatch_alias(alias, gateway.value()) ||
                m.write_ptr(
                    vtable + method.vtable_offset, alias).is_err()) {
                discard_proxy();
                return 0;
            }
            m_remote_methods.emplace(alias, RemoteMethodBinding{
                object,
                static_cast<u32>(method.vtable_offset / pointer_size),
                method});
        }
    }
    return object;
}

GuestAddr ComCoreManager::create_marshaled_interface_proxy(
    Marshaler& m, u32 owner_pid, u32 owner_pointer_size,
    GuestAddr remote_object, const Guid& iid) {
    if (owner_pid == 0 || remote_object == 0 ||
        (owner_pointer_size != 4 && owner_pointer_size != 8)) {
        return 0;
    }
    std::optional<AutomationInterfaceLayout> layout;
    if (!guid_equal(iid, IID_IUNKNOWN)) {
        if (m_automation_layout_resolver) {
            layout = m_automation_layout_resolver(iid);
        }
        if (!layout) return 0;
    }
    ipc::ComClassSnapshot owner{};
    owner.owner_pid = owner_pid;
    owner.class_object = remote_object;
    owner.pointer_size = owner_pointer_size;
    return create_remote_iunknown_proxy(
        m, owner, remote_object, iid, layout ? &*layout : nullptr);
}

std::optional<u32> ComCoreManager::call_remote_iunknown(
    const RemoteInterfaceProxy& proxy, u32 slot,
    std::span<const u64> arguments) const {
    if ((proxy.pointer_size != 4 && proxy.pointer_size != 8) ||
        proxy.owner_pid == 0 || proxy.remote_object == 0) {
        return std::nullopt;
    }
    GuestAddr vtable = 0;
    auto vtable_bytes = std::span<Byte>(
        reinterpret_cast<Byte*>(&vtable), proxy.pointer_size);
    const pid_t pid = static_cast<pid_t>(proxy.owner_pid);
    if (!ipc::remote_read_process_memory(
            pid, proxy.remote_object, vtable_bytes) || vtable == 0) {
        return std::nullopt;
    }
    if (proxy.pointer_size == 4) vtable &= 0xFFFFFFFFu;

    GuestAddr method = 0;
    auto method_bytes = std::span<Byte>(
        reinterpret_cast<Byte*>(&method), proxy.pointer_size);
    if (!ipc::remote_read_process_memory(
            pid, vtable + slot * proxy.pointer_size, method_bytes) ||
        method == 0) {
        return std::nullopt;
    }
    if (proxy.pointer_size == 4) method &= 0xFFFFFFFFu;
    const auto result = ipc::remote_call_guest(
        pid, method, arguments,
        m_elevation != nullptr && m_elevation->is_elevated());
    if (!result) return std::nullopt;
    return static_cast<u32>(*result);
}

u32 ComCoreManager::remote_proxy_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr output_ptr = m.arg_ptr(2);
    if (output_ptr == 0) return com::E_POINTER;
    if (iid_ptr == 0 || m.write_ptr(output_ptr, 0).is_err()) {
        return com::E_INVALIDARG;
    }
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    ipc::ipc_log("remote QueryInterface proxy=0x%llx iid=%s",
                 static_cast<unsigned long long>(object),
                 format_guid(iid.value()).c_str());

    RemoteInterfaceProxy source{};
    GuestAddr identity_object = 0;
    {
        std::lock_guard lock(m_remote_proxies_mutex);
        auto found = m_remote_proxies.find(object);
        if (found == m_remote_proxies.end()) return E_NOINTERFACE;
        source = found->second;
        identity_object = source.identity_object != 0
            ? source.identity_object : object;

        // Every interface on one COM object must return the same physical
        // IUnknown pointer. Reuse an existing local proxy for repeated queries
        // instead of manufacturing another identity.
        GuestAddr existing_object = 0;
        if (guid_equal(iid.value(), IID_IUNKNOWN)) {
            existing_object = identity_object;
        } else if (guid_equal(iid.value(), source.iid)) {
            existing_object = object;
        } else {
            for (const auto& [candidate_object, candidate] :
                 m_remote_proxies) {
                if (candidate.identity_object == identity_object &&
                    guid_equal(candidate.iid, iid.value())) {
                    existing_object = candidate_object;
                    break;
                }
            }
        }
        if (existing_object != 0) {
            auto existing = m_remote_proxies.find(existing_object);
            if (existing == m_remote_proxies.end()) return E_NOINTERFACE;
            ++existing->second.local_refs;
            if (m.write_ptr(output_ptr, existing_object).is_err()) {
                --existing->second.local_refs;
                return com::E_INVALIDARG;
            }
            return com::S_OK;
        }
    }

    std::optional<AutomationInterfaceLayout> layout;
    if (m_automation_layout_resolver) {
        layout = m_automation_layout_resolver(iid.value());
    }
    const bool standard_dispatch = guid_equal(iid.value(), IID_IDISPATCH);
    if (!layout && !standard_dispatch) {
        ipc::ipc_log(
            "remote QueryInterface iid=%s failed: no marshaling layout",
            format_guid(iid.value()).c_str());
        VITREO_TRACE(
            "[ole32] remote QueryInterface IID %s has no registered "
            "marshaling layout\n", format_guid(iid.value()).c_str());
        return com::E_NOTIMPL;
    }

    const pid_t owner_pid = static_cast<pid_t>(source.owner_pid);
    const GuestAddr scratch = ipc::remote_virtual_alloc(
        owner_pid, 0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (scratch == 0) return E_OUTOFMEMORY;
    struct ScratchGuard {
        pid_t pid;
        GuestAddr address;
        ~ScratchGuard() {
            (void)ipc::remote_virtual_free(pid, address, 0, MEM_RELEASE);
        }
    } scratch_guard{owner_pid, scratch};

    const auto iid_bytes = std::span<const Byte>(
        reinterpret_cast<const Byte*>(&iid.value()), sizeof(Guid));
    std::array<Byte, 8> null_output{};
    if (!ipc::remote_write_process_memory(owner_pid, scratch, iid_bytes) ||
        !ipc::remote_write_process_memory(
            owner_pid, scratch + sizeof(Guid),
            std::span<const Byte>(null_output.data(), source.pointer_size))) {
        return RPC_E_SERVERFAULT;
    }

    const std::array<u64, 3> arguments{
        source.remote_object, scratch, scratch + sizeof(Guid)};
    const auto result = call_remote_iunknown(source, 0, arguments);
    if (!result) return RPC_E_SERVERFAULT;
    ipc::ipc_log("remote QueryInterface iid=%s owner=%u -> 0x%08x",
                 format_guid(iid.value()).c_str(), source.owner_pid, *result);
    if ((*result & 0x80000000u) != 0) return *result;

    GuestAddr remote_object = 0;
    auto object_bytes = std::span<Byte>(
        reinterpret_cast<Byte*>(&remote_object), source.pointer_size);
    if (!ipc::remote_read_process_memory(
            owner_pid, scratch + sizeof(Guid), object_bytes) ||
        remote_object == 0) {
        return RPC_E_SERVERFAULT;
    }
    if (source.pointer_size == 4) remote_object &= 0xFFFFFFFFu;

    ipc::ComClassSnapshot owner{};
    owner.owner_pid = source.owner_pid;
    owner.pointer_size = source.pointer_size;
    const GuestAddr proxy = create_remote_iunknown_proxy(
        m, owner, remote_object, iid.value(),
        layout ? &*layout : nullptr, identity_object);
    RemoteInterfaceProxy remote_result{
        source.owner_pid, source.pointer_size, remote_object, iid.value(),
        1, 0, identity_object};
    const auto release_remote_result = [&] {
        const std::array<u64, 1> release_args{remote_object};
        (void)call_remote_iunknown(remote_result, 2, release_args);
    };
    if (proxy == 0) {
        release_remote_result();
        return E_OUTOFMEMORY;
    }
    if (m.write_ptr(output_ptr, proxy).is_err()) {
        {
            std::lock_guard lock(m_remote_proxies_mutex);
            m_remote_proxies.erase(proxy);
            std::erase_if(m_remote_methods, [proxy](const auto& entry) {
                return entry.second.proxy_object == proxy;
            });
        }
        m_guest_task_memory->free(proxy);
        release_remote_result();
        return com::E_INVALIDARG;
    }
    VITREO_TRACE(
        "[ole32] remote QueryInterface IID %s object=0x%llx "
        "proxy=0x%llx identity=0x%llx\n",
        format_guid(iid.value()).c_str(),
        static_cast<unsigned long long>(remote_object),
        static_cast<unsigned long long>(proxy),
        static_cast<unsigned long long>(identity_object));
    return com::S_OK;
}

u32 ComCoreManager::remote_proxy_add_ref(Marshaler& m) {
    std::lock_guard lock(m_remote_proxies_mutex);
    auto found = m_remote_proxies.find(m.arg_ptr(0));
    if (found == m_remote_proxies.end()) return 0;
    return ++found->second.local_refs;
}

u32 ComCoreManager::remote_dispatch_get_type_info_count(Marshaler& m) {
    const GuestAddr output = m.arg_ptr(1);
    if (output == 0) return com::E_POINTER;
    RemoteInterfaceProxy proxy{};
    {
        std::lock_guard lock(m_remote_proxies_mutex);
        const auto found = m_remote_proxies.find(m.arg_ptr(0));
        if (found == m_remote_proxies.end()) return E_NOINTERFACE;
        proxy = found->second;
    }
    const pid_t owner = static_cast<pid_t>(proxy.owner_pid);
    const GuestAddr remote_output = ipc::remote_virtual_alloc(
        owner, 0, sizeof(u32), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_output == 0) return E_OUTOFMEMORY;
    struct Guard {
        pid_t pid;
        GuestAddr address;
        ~Guard() { (void)ipc::remote_virtual_free(pid, address, 0, MEM_RELEASE); }
    } guard{owner, remote_output};
    const std::array<Byte, sizeof(u32)> zero{};
    if (!ipc::remote_write_process_memory(owner, remote_output, zero)) {
        return RPC_E_SERVERFAULT;
    }
    const std::array<u64, 2> args{proxy.remote_object, remote_output};
    const auto result = call_remote_iunknown(proxy, 3, args);
    if (!result) return RPC_E_SERVERFAULT;
    u32 count = 0;
    auto count_bytes =
        std::span<Byte>(reinterpret_cast<Byte*>(&count), sizeof(count));
    if ((*result & 0x80000000u) == 0 &&
        (!ipc::remote_read_process_memory(owner, remote_output, count_bytes) ||
         m.write_u32(output, count).is_err())) {
        return RPC_E_SERVERFAULT;
    }
    return *result;
}

u32 ComCoreManager::remote_dispatch_get_type_info(Marshaler& m) {
    const GuestAddr output = m.arg_ptr(3);
    if (output != 0) (void)m.write_ptr(output, 0);
    return 0x8002000Bu; // DISP_E_BADINDEX
}

u32 ComCoreManager::remote_dispatch_get_ids_of_names(Marshaler& m) {
    const GuestAddr iid_pointer = m.arg_ptr(1);
    const GuestAddr names_pointer = m.arg_ptr(2);
    const u32 name_count = static_cast<u32>(m.arg(3));
    const u32 lcid = static_cast<u32>(m.arg(4));
    const GuestAddr dispids_pointer = m.arg_ptr(5);
    if (iid_pointer == 0 || names_pointer == 0 || name_count == 0 ||
        name_count > 256 || dispids_pointer == 0) {
        return com::E_INVALIDARG;
    }
    RemoteInterfaceProxy proxy{};
    {
        std::lock_guard lock(m_remote_proxies_mutex);
        const auto found = m_remote_proxies.find(m.arg_ptr(0));
        if (found == m_remote_proxies.end()) return E_NOINTERFACE;
        proxy = found->second;
    }
    const auto iid = m.read_struct<Guid>(iid_pointer);
    if (iid.is_err()) return com::E_INVALIDARG;
    const usize local_pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    std::vector<std::string> names;
    names.reserve(name_count);
    for (u32 index = 0; index < name_count; ++index) {
        const auto pointer = m.read_ptr(
            names_pointer + static_cast<GuestAddr>(index) * local_pointer_size);
        if (pointer.is_err() || pointer.value() == 0) return com::E_INVALIDARG;
        const auto name = m.read_wide_string(
            static_cast<GuestAddr>(pointer.value()));
        if (name.is_err()) return com::E_INVALIDARG;
        names.push_back(name.value());
    }

    const pid_t owner = static_cast<pid_t>(proxy.owner_pid);
    std::vector<GuestAddr> allocations;
    const auto allocate = [&](u64 size) -> GuestAddr {
        const GuestAddr address = ipc::remote_virtual_alloc(
            owner, 0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (address != 0) allocations.push_back(address);
        return address;
    };
    struct AllocationsGuard {
        pid_t pid;
        std::vector<GuestAddr>& values;
        ~AllocationsGuard() {
            for (const GuestAddr value : values) {
                (void)ipc::remote_virtual_free(pid, value, 0, MEM_RELEASE);
            }
        }
    } guard{owner, allocations};

    const GuestAddr remote_iid = allocate(sizeof(Guid));
    const GuestAddr remote_names =
        allocate(static_cast<u64>(name_count) * proxy.pointer_size);
    const GuestAddr remote_dispids =
        allocate(static_cast<u64>(name_count) * sizeof(i32));
    if (remote_iid == 0 || remote_names == 0 || remote_dispids == 0) {
        return E_OUTOFMEMORY;
    }
    const auto iid_bytes = std::span<const Byte>(
        reinterpret_cast<const Byte*>(&iid.value()), sizeof(Guid));
    if (!ipc::remote_write_process_memory(owner, remote_iid, iid_bytes)) {
        return RPC_E_SERVERFAULT;
    }
    for (u32 index = 0; index < name_count; ++index) {
        std::vector<Byte> wide((names[index].size() + 1u) * sizeof(u16));
        for (usize character = 0; character < names[index].size(); ++character) {
            const u16 value = static_cast<u8>(names[index][character]);
            std::memcpy(wide.data() + character * sizeof(u16), &value,
                        sizeof(value));
        }
        const GuestAddr remote_string = allocate(wide.size());
        if (remote_string == 0 ||
            !ipc::remote_write_process_memory(owner, remote_string, wide)) {
            return RPC_E_SERVERFAULT;
        }
        std::array<Byte, 8> pointer_bytes{};
        std::memcpy(pointer_bytes.data(), &remote_string, proxy.pointer_size);
        if (!ipc::remote_write_process_memory(
                owner,
                remote_names +
                    static_cast<GuestAddr>(index) * proxy.pointer_size,
                std::span<const Byte>(
                    pointer_bytes.data(), proxy.pointer_size))) {
            return RPC_E_SERVERFAULT;
        }
    }
    const std::array<u64, 6> args{
        proxy.remote_object, remote_iid, remote_names, name_count, lcid,
        remote_dispids};
    const auto result = call_remote_iunknown(proxy, 5, args);
    if (!result) return RPC_E_SERVERFAULT;
    ipc::ipc_log(
        "remote IDispatch::GetIDsOfNames owner=%u name='%s' count=%u -> "
        "0x%08x",
        proxy.owner_pid, names.front().c_str(), name_count, *result);
    if ((*result & 0x80000000u) != 0) return *result;
    std::vector<Byte> dispid_bytes(name_count * sizeof(i32));
    if (!ipc::remote_read_process_memory(owner, remote_dispids, dispid_bytes) ||
        m.memory().write_bytes(
            dispids_pointer, dispid_bytes.data(), dispid_bytes.size()).is_err()) {
        return RPC_E_SERVERFAULT;
    }
    return *result;
}

u32 ComCoreManager::remote_proxy_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    RemoteInterfaceProxy proxy{};
    {
        std::lock_guard lock(m_remote_proxies_mutex);
        auto found = m_remote_proxies.find(object);
        if (found == m_remote_proxies.end()) return 0;
        if (found->second.local_refs > 1) {
            return --found->second.local_refs;
        }
        proxy = found->second;
        m_remote_proxies.erase(found);
        std::erase_if(m_remote_methods, [object](const auto& entry) {
            return entry.second.proxy_object == object;
        });
    }

    const std::array<u64, 1> arguments{proxy.remote_object};
    (void)call_remote_iunknown(proxy, 2, arguments);
    if (m_guest_task_memory != nullptr) m_guest_task_memory->free(object);
    return 0;
}

} // namespace vitreo::winapi
