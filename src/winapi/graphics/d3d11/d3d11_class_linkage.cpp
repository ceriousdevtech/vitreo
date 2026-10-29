// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_class_linkage.cpp
/// @brief The ID3D11ClassLinkage and ID3D11ClassInstance objects.
///
/// CreateClassLinkage, the linkage's type and instance name registry
/// (filled from each shader created against it), GetClassInstance /
/// CreateClassInstance, and both objects' methods and release. Binding
/// a shader with class instances is in d3d11_class_instance_binding.cpp.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// ID3D11ClassLinkage / ID3D11ClassInstance
// -----------------------------------------------------------------------------

namespace {

/// A class or instance name: an ANSI string of at most this many bytes.
constexpr usize kClassNameLimit = 1024;

}  // namespace

bool D3D11Manager::valid_class_linkage(GuestAddr device_owner,
                                       GuestAddr linkage) const {
    if (linkage == 0) return true;
    const auto found = m_class_linkages.find(linkage);
    return found != m_class_linkages.end() &&
           found->second.device_owner == device_owner;
}

u32 D3D11Manager::refuse_interface_shader(GuestAddr linkage,
                                          const char* method) {
    if (linkage != 0) return d3d11::S_OK;
    // WARP: a shader that declares interfaces needs a class linkage
    // (measured E_INVALIDARG in every stage).
    d3d11_diag("ID3D11Device::%s -> E_INVALIDARG (class-linkage shader "
               "without a linkage)", method);
    return d3d11::E_INVALIDARG;
}

namespace {

/// Index of @p name in a linkage's registry, appended when new.
u32 register_name(std::vector<std::string>& names, const std::string& name) {
    const auto known = std::find(names.begin(), names.end(), name);
    if (known != names.end()) {
        return static_cast<u32>(known - names.begin());
    }
    names.push_back(name);
    return static_cast<u32>(names.size() - 1u);
}

}  // namespace

void D3D11Manager::register_class_linkage(
    GuestAddr linkage, const graphics::DxbcContainer& container) {
    auto found = m_class_linkages.find(linkage);
    if (found == m_class_linkages.end() || !container.has_class_linkage) {
        return;
    }
    for (const auto& type : container.class_linkage.types) {
        (void)register_name(found->second.type_names, type.name);
    }
    for (const auto& instance : container.class_linkage.instances) {
        (void)register_name(found->second.instance_names, instance.name);
    }
}

void D3D11Manager::attach_shader_linkage(
    ShaderLinkage& linkage, const std::vector<u8>& bytes,
    const graphics::DxbcContainer& container) {
    if (!container.valid() || !graphics::dxbc_uses_class_linkage(container)) {
        return;
    }
    linkage.classes = std::make_shared<const graphics::DxbcClassLinkage>(
        container.class_linkage);
    linkage.bytecode = bytes;
}

void D3D11Manager::create_class_linkage(Marshaler& m, GuestAddr self) {
    // CreateClassLinkage(ppLinkage). Windows faults on a NULL ppLinkage
    // (measured); refusing it is the nearest defined answer.
    const GuestAddr linkage_out = m.arg_ptr(1);
    if (linkage_out == 0) {
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(linkage_out, 0);
    const GuestAddr owner = m_interfaces.find(self)->second.owner;
    const GuestAddr address = ensure_storage()
        ? allocate_object(m_class_linkage_vtable)
        : 0;
    if (address == 0) {
        m.set_return(d3d11::E_OUTOFMEMORY);
        return;
    }
    ClassLinkage linkage{};
    linkage.interface_address = address;
    linkage.device_owner = owner;
    m_class_linkages.emplace(address, std::move(linkage));
    acquire_parent(owner);
    if (m.write_ptr(linkage_out, address).is_err()) {
        (void)release_object(address);
        m.set_return(d3d11::E_POINTER);
        return;
    }
    m.set_return(d3d11::S_OK);
}

u32 D3D11Manager::release_class_linkage(GuestAddr address) {
    auto it = m_class_linkages.find(address);
    if (it == m_class_linkages.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        m_class_linkages.erase(it);
        free_object(address);
    }
    return count;
}

u32 D3D11Manager::release_class_instance(GuestAddr address) {
    auto it = m_class_instances.find(address);
    if (it == m_class_instances.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        const GuestAddr linkage = it->second.linkage;
        m_class_instances.erase(it);
        free_object(address);
        release_internal(linkage);
    }
    return count;
}

void D3D11Manager::impl_ClassLinkageMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto linkage = m_class_linkages.find(self);
    if (linkage == m_class_linkages.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11CLASSLINKAGE))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        (void)m.write_ptr(out, self);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(linkage->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            (void)m.write_ptr(device_out, device->second.device_interface);
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, linkage->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, linkage->second.private_data));
        return;
    case 6:
        m.set_return(
            set_private_data_interface(m, linkage->second.private_data));
        return;
    case 7:    // GetClassInstance(LPCSTR, UINT InstanceIndex, ppInstance)
    case 8: {  // CreateClassInstance(LPCSTR, UINT x4, ppInstance)
        // Measured (a12 [cl], refmodel [cl]): any name succeeds, even one
        // no shader declares and before any shader exists, and every call
        // makes a new object. A NULL name or out-parameter is E_INVALIDARG,
        // the out-parameter left untouched.
        const bool create = slot == 8;
        const GuestAddr name_address = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(create ? 6 : 3);
        std::string name;
        bool terminated = false;
        if (name_address != 0) {
            for (usize i = 0; i < kClassNameLimit; ++i) {
                const auto byte = m_memory.read_u8(name_address + i);
                if (byte.is_err()) break;
                if (byte.value() == 0) {
                    terminated = true;
                    break;
                }
                name.push_back(static_cast<char>(byte.value()));
            }
        }
        if (name_address == 0 || out == 0 || !terminated) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const GuestAddr address = allocate_object(m_class_instance_vtable);
        if (address == 0) {
            m.set_return(d3d11::E_OUTOFMEMORY);
            return;
        }
        ClassInstance instance{};
        instance.interface_address = address;
        instance.device_owner = linkage->second.device_owner;
        instance.linkage = self;
        if (create) {
            // D3D11_CLASS_INSTANCE_DESC echoes the four offsets (measured
            // CB=1 CBOffset=2 Tex=3 Sampler=4 for (1,2,3,4)); InstanceId and
            // InstanceIndex read 0; only the type name is known.
            // TypeId numbers the class-type names the linkage has seen
            // (measured: AddK 3 after a shader registered SampOp, BufOp,
            // MulK, AddK; a name no shader declares is appended).
            instance.type_name = name;
            instance.desc = {0u, 0u,
                             register_name(linkage->second.type_names, name),
                             static_cast<u32>(m.arg(2)),
                             static_cast<u32>(m.arg(3)),
                             static_cast<u32>(m.arg(4)),
                             static_cast<u32>(m.arg(5))};
            instance.created = true;
        } else {
            // InstanceId numbers the instance names this linkage has seen,
            // from shaders and requests alike (measured: "nope" 0, g_green
            // 1, g_red 2, "nope" again 0, g_abstract 3 with no shader;
            // g_add0 3 after one declared g_buf, g_samp, g_adds, g_add0);
            // InstanceIndex is the argument; TypeId and the offsets read 0
            // even for an instance a shader places (measured); only the
            // instance name is known.
            const u32 id =
                register_name(linkage->second.instance_names, name);
            instance.instance_name = name;
            instance.desc = {id, static_cast<u32>(m.arg(2)), 0u, 0u, 0u, 0u,
                             0u};
        }
        m_class_instances.emplace(address, std::move(instance));
        // The instance keeps its linkage with a runtime reference (measured:
        // the linkage's count does not move, and it outlives the
        // application's last Release while an instance lives).
        (void)hold_object(self);
        acquire_parent(linkage->second.device_owner);
        if (m.write_ptr(out, address).is_err()) {
            (void)release_object(address);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        unsupported(m, "ID3D11ClassLinkage", slot);
        return;
    }
}

void D3D11Manager::impl_ClassInstanceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto instance = m_class_instances.find(self);
    if (instance == m_class_instances.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    // GetInstanceName / GetTypeName(LPSTR, SIZE_T* pBufferLength): the
    // length written is always the whole name's plus its NUL; a buffer copies
    // as much as fits, NUL-terminated (measured "CR\0" into 3 bytes, length
    // 5); a NULL buffer is the size query.
    const auto write_name = [&](const std::string& name) {
        const GuestAddr buffer = m.arg_ptr(1);
        const GuestAddr length_address = m.arg_ptr(2);
        m.set_return(0);
        if (length_address == 0) return;
        GuestAddr capacity = 0;
        if (!read_pointer(length_address, capacity)) return;
        if (buffer != 0 && capacity != 0) {
            const usize copy = std::min<usize>(name.size(), capacity - 1);
            (void)m_memory.write_bytes(
                buffer, reinterpret_cast<const Byte*>(name.data()), copy);
            (void)m_memory.write_u8(buffer + copy, 0);
        }
        (void)write_pointer(length_address, name.size() + 1);
    };
    switch (slot) {
    case 0: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11CLASSINSTANCE))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        (void)m.write_ptr(out, self);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(instance->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            (void)m.write_ptr(device_out, device->second.device_interface);
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, instance->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, instance->second.private_data));
        return;
    case 6:
        m.set_return(
            set_private_data_interface(m, instance->second.private_data));
        return;
    case 7: {
        // GetClassLinkage: the application gets a reference of its own
        // (measured: count 1 after its last one was released).
        const GuestAddr out = m.arg_ptr(1);
        const GuestAddr linkage = instance->second.linkage;
        if (out != 0) {
            (void)retain_object(linkage);
            (void)m.write_ptr(out, linkage);
        }
        m.set_return(0);
        return;
    }
    case 8: {
        // GetDesc: seven UINTs and the BOOL Created.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            std::array<u32, 8> words{};
            std::copy(instance->second.desc.begin(),
                      instance->second.desc.end(), words.begin());
            words[7] = instance->second.created ? 1u : 0u;
            (void)m_memory.write_bytes(
                out, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    case 9:
        write_name(instance->second.instance_name);
        return;
    case 10:
        write_name(instance->second.type_name);
        return;
    default:
        unsupported(m, "ID3D11ClassInstance", slot);
        return;
    }
}

}  // namespace vitreo::winapi
