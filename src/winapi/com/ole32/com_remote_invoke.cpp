// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_remote_invoke.cpp
/// @brief IDispatch::Invoke and typelib-driven method invocation through a remote
/// proxy, with the VARIANT/BSTR marshalling helpers it needs.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "com_core_internal.hpp"

#include <array>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi {

namespace {

constexpr u32 MAX_AUTOMATION_BSTR_BYTES = 16u * 1024u * 1024u;

[[nodiscard]] u32 automation_value_size(u16 vt) {
    switch (vt) {
        case 16: // VT_I1
        case 17: // VT_UI1
            return 1;
        case 2:  // VT_I2
        case 11: // VT_BOOL
        case 18: // VT_UI2
            return 2;
        case 3:  // VT_I4
        case 4:  // VT_R4
        case 10: // VT_ERROR
        case 19: // VT_UI4
        case 22: // VT_INT
        case 23: // VT_UINT
            return 4;
        case 5:  // VT_R8
        case 6:  // VT_CY
        case 7:  // VT_DATE
        case 20: // VT_I8
        case 21: // VT_UI8
            return 8;
        default:
            return 0;
    }
}

[[nodiscard]] bool is_scalar_automation_vt(u16 vt) {
    switch (vt) {
        case 2:  // VT_I2
        case 3:  // VT_I4
        case 4:  // VT_R4
        case 5:  // VT_R8
        case 6:  // VT_CY
        case 7:  // VT_DATE
        case 10: // VT_ERROR
        case 11: // VT_BOOL
        case 16: // VT_I1
        case 17: // VT_UI1
        case 18: // VT_UI2
        case 19: // VT_UI4
        case 20: // VT_I8
        case 21: // VT_UI8
        case 22: // VT_INT
        case 23: // VT_UINT
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool read_presented_bstr(
    Marshaler& m, GuestAddr bstr, std::vector<Byte>& block) {
    if (bstr < sizeof(u32)) return false;
    const auto byte_length = m.read_u32(bstr - sizeof(u32));
    if (byte_length.is_err() ||
        (byte_length.value() & 1u) != 0 ||
        byte_length.value() > MAX_AUTOMATION_BSTR_BYTES) {
        return false;
    }
    const usize total_size =
        sizeof(u32) + static_cast<usize>(byte_length.value()) + sizeof(u16);
    block.assign(total_size, Byte{0});
    const u32 length = byte_length.value();
    std::memcpy(block.data(), &length, sizeof(length));
    if (m.memory().read_bytes(
            bstr, block.data() + sizeof(u32),
            length + sizeof(u16)).is_err()) return false;
    return block[total_size - 2] == Byte{0} &&
           block[total_size - 1] == Byte{0};
}

[[nodiscard]] bool read_null_terminated_string_bytes(
    Marshaler& m, GuestAddr string, u32 unit_size,
    std::vector<Byte>& bytes) {
    if (string == 0 || (unit_size != 1 && unit_size != 2)) return false;
    u32 byte_length = 0;
    while (byte_length <= MAX_AUTOMATION_BSTR_BYTES - unit_size) {
        bool terminated = false;
        if (unit_size == 1) {
            const auto value = m.read_u8(string + byte_length);
            if (value.is_err()) return false;
            terminated = value.value() == 0;
        } else {
            const auto value = m.read_u16(string + byte_length);
            if (value.is_err()) return false;
            terminated = value.value() == 0;
        }
        byte_length += unit_size;
        if (terminated) {
            bytes.resize(byte_length);
            return m.memory().read_bytes(
                string, bytes.data(), bytes.size()).is_ok();
        }
    }
    return false;
}

[[nodiscard]] bool read_remote_presented_bstr(
    pid_t pid, GuestAddr bstr, std::vector<Byte>& utf16_bytes) {
    if (bstr < sizeof(u32)) return false;
    u32 byte_length = 0;
    if (!ipc::remote_read_process_memory(
            pid, bstr - sizeof(u32),
            std::span<Byte>(reinterpret_cast<Byte*>(&byte_length),
                            sizeof(byte_length))) ||
        (byte_length & 1u) != 0 ||
        byte_length > MAX_AUTOMATION_BSTR_BYTES) {
        return false;
    }
    utf16_bytes.assign(static_cast<usize>(byte_length) + sizeof(u16), Byte{0});
    if (!ipc::remote_read_process_memory(
            pid, bstr,
            std::span<Byte>(utf16_bytes.data(), utf16_bytes.size())) ||
        utf16_bytes[utf16_bytes.size() - 2] != Byte{0} ||
        utf16_bytes[utf16_bytes.size() - 1] != Byte{0}) {
        return false;
    }
    utf16_bytes.resize(byte_length);
    return true;
}

[[nodiscard]] bool write_automation_scalar(
    Marshaler& m, GuestAddr destination, u16 vt, u64 value) {
    switch (automation_value_size(vt)) {
        case 1:
            return m.write_u8(destination, static_cast<u8>(value)).is_ok();
        case 2:
            return m.write_u16(destination, static_cast<u16>(value)).is_ok();
        case 4:
            return m.write_u32(destination, static_cast<u32>(value)).is_ok();
        case 8:
            return m.write_u64(destination, value).is_ok();
        default:
            return false;
    }
}

} // namespace

u32 ComCoreManager::remote_dispatch_invoke(Marshaler& m) {
    const i32 dispid = static_cast<i32>(m.arg(1));
    const GuestAddr iid_pointer = m.arg_ptr(2);
    const u32 lcid = static_cast<u32>(m.arg(3));
    const u16 flags = static_cast<u16>(m.arg(4));
    const GuestAddr parameters = m.arg_ptr(5);
    const GuestAddr result_pointer = m.arg_ptr(6);
    u32 argument_count = 0;
    u32 named_count = 0;
    if (parameters != 0) {
        const GuestAddr count_offset = m.cpu_is_64bit() ? 16u : 8u;
        argument_count =
            m.memory().read_u32(parameters + count_offset).value_or(0);
        named_count =
            m.memory().read_u32(parameters + count_offset + 4u).value_or(0);
    }
    ipc::ipc_log(
        "remote IDispatch::Invoke dispid=%d flags=0x%x args=%u named=%u",
        dispid, flags, argument_count, named_count);
    if (iid_pointer == 0 || parameters == 0 || argument_count != 0 ||
        named_count != 0) {
        return com::E_NOTIMPL;
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
    const GuestAddr remote_parameters = allocate(24);
    const GuestAddr remote_result =
        result_pointer != 0 ? allocate(24) : GuestAddr{0};
    if (remote_iid == 0 || remote_parameters == 0 ||
        (result_pointer != 0 && remote_result == 0)) {
        return E_OUTOFMEMORY;
    }
    const std::array<Byte, 24> zero{};
    const auto iid_bytes = std::span<const Byte>(
        reinterpret_cast<const Byte*>(&iid.value()), sizeof(Guid));
    if (!ipc::remote_write_process_memory(owner, remote_iid, iid_bytes) ||
        !ipc::remote_write_process_memory(owner, remote_parameters, zero) ||
        (remote_result != 0 &&
         !ipc::remote_write_process_memory(owner, remote_result, zero))) {
        return RPC_E_SERVERFAULT;
    }
    const std::array<u64, 9> args{
        proxy.remote_object, static_cast<u32>(dispid), remote_iid, lcid, flags,
        remote_parameters, remote_result, 0, 0};
    const auto call_result = call_remote_iunknown(proxy, 6, args);
    if (!call_result) return RPC_E_SERVERFAULT;
    ipc::ipc_log("remote IDispatch::Invoke dispid=%d owner=%u -> 0x%08x",
                 dispid, proxy.owner_pid, *call_result);
    if ((*call_result & 0x80000000u) != 0 || result_pointer == 0) {
        return *call_result;
    }

    std::array<Byte, 24> remote_variant{};
    if (!ipc::remote_read_process_memory(owner, remote_result,
                                         remote_variant)) {
        return RPC_E_SERVERFAULT;
    }
    u16 vt = 0;
    std::memcpy(&vt, remote_variant.data(), sizeof(vt));
    std::vector<Byte> local_variant(m.cpu_is_64bit() ? 24u : 16u);
    std::memcpy(local_variant.data(), &vt, sizeof(vt));
    constexpr u16 VT_BSTR = 8;
    constexpr u16 VT_UNKNOWN = 13;
    constexpr u16 VT_DISPATCH = 9;
    if (vt == VT_BSTR) {
        GuestAddr remote_bstr = 0;
        std::memcpy(
            &remote_bstr, remote_variant.data() + 8, proxy.pointer_size);
        if (proxy.pointer_size == 4) remote_bstr &= 0xFFFFFFFFu;
        GuestAddr local_bstr = 0;
        if (remote_bstr != 0) {
            if (!m_automation_bstr_allocator) return com::E_NOTIMPL;
            std::vector<Byte> utf16_bytes;
            if (!read_remote_presented_bstr(
                    owner, remote_bstr, utf16_bytes)) {
                return RPC_E_SERVERFAULT;
            }
            local_bstr = m_automation_bstr_allocator(
                m, std::span<const Byte>(
                    utf16_bytes.data(), utf16_bytes.size()));
            if (local_bstr == 0) return E_OUTOFMEMORY;
            // Do not CallGuest the caller's oleaut32!SysFreeString thunk in
            // the owner: thunk addresses are process-local, and a 32-bit
            // 0x7000xxxx stub is unmapped in a 64-bit helper. The BSTR is
            // already copied into the caller; Vitreo SysFreeString is a
            // bump-arena no-op, so skipping the remote free is correct.
        }
        if (m.cpu_is_64bit()) {
            std::memcpy(local_variant.data() + 8, &local_bstr, sizeof(u64));
        } else {
            const u32 pointer32 = static_cast<u32>(local_bstr);
            std::memcpy(local_variant.data() + 8, &pointer32, sizeof(pointer32));
        }
    } else if (vt == VT_UNKNOWN || vt == VT_DISPATCH ||
               (vt & 0x6000u) != 0) {
        ipc::ipc_log(
            "remote IDispatch::Invoke dispid=%d unsupported result vt=0x%x",
            dispid, vt);
        return com::E_NOTIMPL;
    } else {
        std::memcpy(local_variant.data() + 8, remote_variant.data() + 8, 8);
    }
    if (m.memory().write_bytes(
            result_pointer, local_variant.data(), local_variant.size())
            .is_err()) {
        return com::E_INVALIDARG;
    }
    ipc::ipc_log("remote IDispatch::Invoke dispid=%d result vt=0x%x",
                 dispid, vt);
    return *call_result;
}

u32 ComCoreManager::remote_proxy_invoke(Marshaler& m) {
    if (!m.has_cpu()) return com::E_NOTIMPL;
    const GuestAddr alias = m.cpu()->rip;
    RemoteMethodBinding binding{};
    RemoteInterfaceProxy proxy{};
    {
        std::lock_guard lock(m_remote_proxies_mutex);
        auto method = m_remote_methods.find(alias);
        if (method == m_remote_methods.end()) {
            VITREO_TRACE(
                "[ole32] remote Automation alias 0x%llx has no method binding\n",
                static_cast<unsigned long long>(alias));
            return com::E_NOTIMPL;
        }
        auto object = m_remote_proxies.find(method->second.proxy_object);
        if (object == m_remote_proxies.end()) {
            VITREO_TRACE(
                "[ole32] remote Automation method '%s' lost proxy 0x%llx\n",
                method->second.layout.name.c_str(),
                static_cast<unsigned long long>(
                    method->second.proxy_object));
            return com::E_NOTIMPL;
        }
        binding = method->second;
        proxy = object->second;
    }

    const auto& layout = binding.layout;
    if (layout.parameter_vts.size() != layout.parameter_count ||
        layout.parameter_flags.size() != layout.parameter_count) {
        return com::E_NOTIMPL;
    }
    const Guid null_iid{};
    for (u16 index = 0; index < layout.parameter_count; ++index) {
        const u16 vt = layout.parameter_vts[index];
        const u16 flags = layout.parameter_flags[index];
        const bool is_input = (flags & 0x0001u) != 0;
        const bool is_output = (flags & 0x000Au) != 0;
        if ((flags & 0x0004u) != 0 || (is_input && is_output)) {
            VITREO_TRACE(
                "[ole32] remote Automation method '%s' parameter=%u "
                "vt=%u flags=0x%x unsupported direction\n",
                layout.name.c_str(), index, vt, flags);
            return com::E_NOTIMPL;
        }
        if (is_output) {
            if (is_scalar_automation_vt(vt) ||
                (vt == 8 /*VT_BSTR*/ && m_automation_bstr_allocator)) {
                continue;
            }
            VITREO_TRACE(
                "[ole32] remote Automation method '%s' output parameter=%u "
                "vt=%u flags=0x%x unsupported\n",
                layout.name.c_str(), index, vt, flags);
            return com::E_NOTIMPL;
        }
        if (is_scalar_automation_vt(vt) || vt == 8 /*VT_BSTR*/ ||
            vt == 30 /*VT_LPSTR*/ || vt == 31 /*VT_LPWSTR*/) continue;
        if ((vt != 13 /*VT_UNKNOWN*/ && vt != 9 /*VT_DISPATCH*/) ||
            (flags & 0x0001u) == 0 ||
            layout.parameter_iids.size() != layout.parameter_count ||
            guid_equal(layout.parameter_iids[index], null_iid)) {
            const bool has_iid =
                layout.parameter_iids.size() == layout.parameter_count &&
                !guid_equal(layout.parameter_iids[index], null_iid);
            VITREO_TRACE(
                "[ole32] remote Automation method '%s' input parameter=%u "
                "vt=%u flags=0x%x interface_iid=%s unsupported\n",
                layout.name.c_str(), index, vt, flags,
                has_iid ? format_guid(
                    layout.parameter_iids[index]).c_str() : "<unresolved>");
            return com::E_NOTIMPL;
        }
    }
    std::vector<u64> arguments;
    arguments.reserve(1u + layout.parameter_count * 2u);
    arguments.push_back(proxy.remote_object);
    std::vector<GuestAddr> marshaled_interfaces;
    std::vector<GuestAddr> remote_allocations;
    struct OutputParameter {
        u16 vt;
        GuestAddr client_pointer;
        GuestAddr remote_pointer;
    };
    std::vector<OutputParameter> output_parameters;
    const auto release_marshaled_interfaces = [&] {
        for (GuestAddr target_interface : marshaled_interfaces) {
            RemoteInterfaceProxy target_proxy{
                proxy.owner_pid, proxy.pointer_size, target_interface,
                IID_IUNKNOWN, 1, 0};
            const std::array<u64, 1> release_args{target_interface};
            (void)call_remote_iunknown(target_proxy, 2, release_args);
        }
        marshaled_interfaces.clear();
    };
    const auto release_remote_allocations = [&] {
        for (GuestAddr allocation : remote_allocations) {
            (void)ipc::remote_virtual_free(
                static_cast<pid_t>(proxy.owner_pid), allocation, 0,
                MEM_RELEASE);
        }
        remote_allocations.clear();
    };
    const auto release_remote_resources = [&] {
        release_marshaled_interfaces();
        release_remote_allocations();
    };
    usize client_abi_index = 1;
    for (u16 index = 0; index < layout.parameter_count; ++index) {
        const u16 vt = layout.parameter_vts[index];
        const u16 flags = layout.parameter_flags[index];
        const bool is_output = (flags & 0x000Au) != 0;
        if (is_output) {
            const GuestAddr client_pointer = m.arg_ptr(client_abi_index++);
            if (client_pointer == 0) {
                arguments.push_back(0);
                continue;
            }
            const u32 value_size = vt == 8 /*VT_BSTR*/
                ? proxy.pointer_size : automation_value_size(vt);
            const GuestAddr remote_pointer = ipc::remote_virtual_alloc(
                static_cast<pid_t>(proxy.owner_pid), 0, value_size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote_pointer == 0) {
                release_remote_resources();
                return E_OUTOFMEMORY;
            }
            remote_allocations.push_back(remote_pointer);
            const std::array<Byte, 8> zero{};
            if (!ipc::remote_write_process_memory(
                    static_cast<pid_t>(proxy.owner_pid), remote_pointer,
                    std::span<const Byte>(zero.data(), value_size))) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            output_parameters.push_back(
                OutputParameter{vt, client_pointer, remote_pointer});
            arguments.push_back(remote_pointer);
            continue;
        }
        if ((vt == 13 /*VT_UNKNOWN*/ || vt == 9 /*VT_DISPATCH*/) &&
            (flags & 0x0001u) != 0) {
            const Guid& parameter_iid = layout.parameter_iids[index];
            const GuestAddr local_interface = m.arg_ptr(client_abi_index++);
            if (local_interface == 0) {
                arguments.push_back(0);
                continue;
            }
            if (guest_iunknown_addref(m, local_interface) == 0) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            const auto iid_bytes = std::span<const Byte>(
                reinterpret_cast<const Byte*>(&parameter_iid),
                sizeof(parameter_iid));
            const GuestAddr target_interface =
                ipc::remote_marshal_com_interface(
                    static_cast<pid_t>(proxy.owner_pid),
                    static_cast<u32>(::getpid()), local_interface,
                    m.cpu_is_64bit() ? 8u : 4u, iid_bytes);
            if (target_interface == 0) {
                guest_iunknown_release(m, local_interface);
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            marshaled_interfaces.push_back(target_interface);
            arguments.push_back(target_interface);
            continue;
        }
        if (vt == 8 /*VT_BSTR*/) {
            const GuestAddr local_bstr = m.arg_ptr(client_abi_index++);
            if (local_bstr == 0) {
                arguments.push_back(0);
                continue;
            }
            std::vector<Byte> block;
            if (!read_presented_bstr(m, local_bstr, block)) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            const GuestAddr remote_block = ipc::remote_virtual_alloc(
                static_cast<pid_t>(proxy.owner_pid), 0, block.size(),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote_block == 0) {
                release_remote_resources();
                return E_OUTOFMEMORY;
            }
            remote_allocations.push_back(remote_block);
            if (!ipc::remote_write_process_memory(
                    static_cast<pid_t>(proxy.owner_pid), remote_block,
                    std::span<const Byte>(block.data(), block.size()))) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            arguments.push_back(remote_block + sizeof(u32));
            continue;
        }
        if (vt == 30 /*VT_LPSTR*/ || vt == 31 /*VT_LPWSTR*/) {
            const GuestAddr local_string = m.arg_ptr(client_abi_index++);
            if (local_string == 0) {
                arguments.push_back(0);
                continue;
            }
            std::vector<Byte> bytes;
            if (!read_null_terminated_string_bytes(
                    m, local_string, vt == 30 ? 1u : 2u, bytes)) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            const GuestAddr remote_string = ipc::remote_virtual_alloc(
                static_cast<pid_t>(proxy.owner_pid), 0, bytes.size(),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote_string == 0) {
                release_remote_resources();
                return E_OUTOFMEMORY;
            }
            remote_allocations.push_back(remote_string);
            if (!ipc::remote_write_process_memory(
                    static_cast<pid_t>(proxy.owner_pid), remote_string,
                    std::span<const Byte>(bytes.data(), bytes.size()))) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            arguments.push_back(remote_string);
            continue;
        }
        if (!is_scalar_automation_vt(vt)) {
            VITREO_TRACE(
                "[ole32] remote Automation method '%s' parameter=%u "
                "vt=%u flags=0x%x unsupported\n",
                layout.name.c_str(), index, vt, flags);
            release_remote_resources();
            return com::E_NOTIMPL;
        }
        const u32 client_slots = automation_value_slots(
            vt, m.cpu_is_64bit());
        u64 value = m.arg(client_abi_index);
        if (client_slots == 2) {
            value = static_cast<u32>(value) |
                (static_cast<u64>(static_cast<u32>(
                    m.arg(client_abi_index + 1))) << 32);
        }
        client_abi_index += client_slots;

        const u32 server_slots = automation_value_slots(
            vt, proxy.pointer_size == 8);
        if (server_slots == 2) {
            arguments.push_back(static_cast<u32>(value));
            arguments.push_back(static_cast<u32>(value >> 32));
        } else {
            arguments.push_back(value);
        }
    }
    const auto result = call_remote_iunknown(
        proxy, binding.vtable_slot, arguments);
    if (result) {
        const pid_t owner_pid = static_cast<pid_t>(proxy.owner_pid);
        for (const auto& output : output_parameters) {
            if (output.vt == 8 /*VT_BSTR*/) {
                GuestAddr remote_bstr = 0;
                if (!ipc::remote_read_process_memory(
                        owner_pid, output.remote_pointer,
                        std::span<Byte>(
                            reinterpret_cast<Byte*>(&remote_bstr),
                            proxy.pointer_size))) {
                    release_remote_resources();
                    return RPC_E_SERVERFAULT;
                }
                if (proxy.pointer_size == 4) remote_bstr &= 0xFFFFFFFFu;
                GuestAddr client_bstr = 0;
                if (remote_bstr != 0) {
                    std::vector<Byte> utf16_bytes;
                    if (!read_remote_presented_bstr(
                            owner_pid, remote_bstr, utf16_bytes)) {
                        release_remote_resources();
                        return RPC_E_SERVERFAULT;
                    }
                    client_bstr = m_automation_bstr_allocator(
                        m, std::span<const Byte>(
                            utf16_bytes.data(), utf16_bytes.size()));
                    if (client_bstr == 0) {
                        release_remote_resources();
                        return E_OUTOFMEMORY;
                    }
                    // Same process-local thunk rule as IDispatch::Invoke:
                    // never execute the caller's SysFreeString in the owner.
                }
                if (m.write_ptr(
                        output.client_pointer, client_bstr).is_err()) {
                    release_remote_resources();
                    return RPC_E_SERVERFAULT;
                }
                continue;
            }

            std::array<Byte, 8> raw{};
            const u32 value_size = automation_value_size(output.vt);
            if (!ipc::remote_read_process_memory(
                    owner_pid, output.remote_pointer,
                    std::span<Byte>(raw.data(), value_size))) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
            u64 value = 0;
            std::memcpy(&value, raw.data(), value_size);
            if (!write_automation_scalar(
                    m, output.client_pointer, output.vt, value)) {
                release_remote_resources();
                return RPC_E_SERVERFAULT;
            }
        }
    }
    release_remote_resources();
    return result.value_or(RPC_E_SERVERFAULT);
}

} // namespace vitreo::winapi
