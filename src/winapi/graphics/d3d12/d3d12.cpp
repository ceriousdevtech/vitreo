// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12.cpp
/// @brief d3d12.dll — the module's exported entry points, the vtable thunks,
/// and D3D12Manager's forwarding to the Frontend.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {

namespace d3d12 {

bool d3d12_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D12");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void unimplemented(const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::string key = std::string(api) + ": " + what;
    std::lock_guard lock(mutex);
    if (!logged.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d3d12!%s\n", key.c_str());
}

Frontend::Frontend(GuestMemory& memory, bool is_64bit,
                   graphics::GraphicsBackendDevice& backend)
    : m_memory(memory), m_is_64bit(is_64bit), m_backend(backend) {}

Frontend::~Frontend() {
    // Objects first, while the maps their memory retires from still exist.
    // The guest's memory may already be gone (the runtime context tears the
    // address space down first at process exit), so nothing here may touch
    // it: see retire_memory.
    std::lock_guard lock(m_mutex);
    m_tearing_down = true;
    // A completion event still registered with the GPU event bridge names
    // the fence's backend timeline; the bridge's thread must stop watching
    // it before it is freed.
    for (const auto& [address, object] : m_objects) {
        if (object->kind == Kind::Fence) {
            GpuEventSignaler::instance().cancel(
                static_cast<Fence&>(*object).backend.get());
        }
    }
    m_objects.clear();
    m_replay.reset();
}

void Frontend::set_method_thunks(const D3D12Manager::MethodThunks& thunks) {
    std::lock_guard lock(m_mutex);
    m_thunks = thunks.tables;
    m_vtables_ready = false;
}

void Frontend::set_guest_function_caller(GuestFunctionCaller caller) {
    std::lock_guard lock(m_mutex);
    m_guest_call = std::move(caller);
}

bool Frontend::is_device(GuestAddr address) {
    std::lock_guard lock(m_mutex);
    return find<Device>(address, Kind::Device) != nullptr;
}

bool Frontend::is_command_queue(GuestAddr address) {
    std::lock_guard lock(m_mutex);
    return find<CommandQueue>(address, Kind::CommandQueue) != nullptr;
}

// =============================================================================
// Exported entry points
// =============================================================================

void Frontend::D3D12CreateDevice(Marshaler& m) {
    // D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL
    //                   MinimumFeatureLevel, REFIID riid, void **ppDevice).
    std::lock_guard lock(m_mutex);
    const GuestAddr adapter = m.arg_ptr(0);
    const u32 minimum_feature_level = m.arg32(1);
    const GuestAddr iid_address = m.arg_ptr(2);
    const GuestAddr device_out = m.arg_ptr(3);
    if (device_out != 0) (void)write_pointer(device_out, 0);

    if (!d3d12_enabled()) {
        // Honest refusal, and deliberately the code a guest checks before
        // falling back: a title with both back ends must keep using D3D11
        // until this frontend has been validated on one.
        d3d12_diag("D3D12CreateDevice -> DXGI_ERROR_UNSUPPORTED "
                   "(set VITREO_D3D12=1 to develop against this frontend)");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    if (!m_backend.available()) {
        d3d12_diag("D3D12CreateDevice -> DXGI_ERROR_UNSUPPORTED (no backend)");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    // Learn (D3D12CreateDevice): MinimumFeatureLevel is the lowest the caller
    // accepts. Measured on WARP (d3d12_a01_device_probe [create]): a level
    // D3D12 does not serve -- 0, 9_x, 10_x, any other value -- is
    // E_INVALIDARG; 1_0_GENERIC, 1_0_CORE and 11_0 up are valid, and one
    // above the device's is DXGI_ERROR_UNSUPPORTED. The device offers the
    // highest level whose every required feature it implements
    // (max_feature_level, the answer D3D12_FEATURE_FEATURE_LEVELS gives).
    const u32 device_level = max_feature_level(caps());
    const bool known = minimum_feature_level == D3D_FEATURE_LEVEL_1_0_GENERIC ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_1_0_CORE ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_11_0 ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_11_1 ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_12_0 ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_12_1 ||
                       minimum_feature_level == D3D_FEATURE_LEVEL_12_2;
    if (!known || minimum_feature_level > device_level) {
        d3d12_diag("D3D12CreateDevice level=0x%x (device 0x%x) -> %s",
                   minimum_feature_level, device_level,
                   known ? "DXGI_ERROR_UNSUPPORTED" : "E_INVALIDARG");
        m.set_return(known ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG);
        return;
    }
    if (device_out == 0) {
        // Learn: with ppDevice null the call only reports whether a device
        // could be created, S_FALSE when it could.
        m.set_return(S_FALSE);
        return;
    }
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Learn: "Direct3D 12 devices are singletons per adapter. If a Direct3D 12
    // device already exists in the current process for a given adapter, then
    // a subsequent call to D3D12CreateDevice returns the existing device."
    if (Device* existing = find<Device>(m_device, Kind::Device)) {
        if (!answers(*existing, iid)) {
            m.set_return(E_NOINTERFACE);
            return;
        }
        add_ref(*existing);
        if (!write_pointer(device_out, existing->address)) {
            --existing->ref_count;
            m.set_return(E_POINTER);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    auto device = std::make_unique<Device>();
    device->feature_level = device_level;
    Device* raw = device.get();
    if (!answers(*raw, iid)) {
        m.set_return(E_NOINTERFACE);
        return;
    }
    const GuestAddr address = publish(std::move(device));
    if (address == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_device = address;
    if (!write_pointer(device_out, address)) {
        (void)release(address);
        m.set_return(E_POINTER);
        return;
    }
    d3d12_diag("D3D12CreateDevice adapter=0x%llx level=0x%x -> 0x%llx (%s)",
               static_cast<unsigned long long>(adapter), minimum_feature_level,
               static_cast<unsigned long long>(address),
               m_backend.backend_device_name().c_str());
    m.set_return(S_OK);
}

void Frontend::D3D12GetDebugInterface(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out != 0) (void)write_pointer(out, 0);
    // Learn: returns DXGI_ERROR_SDK_COMPONENT_MISSING when the debug layer
    // (the Graphics Tools optional feature) is not installed. There is no
    // debug layer here, and a fabricated one would report no errors for work
    // it never validated.
    d3d12_diag("D3D12GetDebugInterface -> DXGI_ERROR_SDK_COMPONENT_MISSING");
    m.set_return(DXGI_ERROR_SDK_COMPONENT_MISSING);
}

// D3D12SerializeRootSignature, D3D12SerializeVersionedRootSignature and the
// two root-signature deserializer exports live in d3d12_root_signature.cpp.

void Frontend::D3D12EnableExperimentalFeatures(Marshaler& m) {
    // D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID *pIIDs,
    //     void *pConfigurationStructs, UINT *pConfigurationStructSizes).
    // Learn: E_NOINTERFACE when a requested feature is unrecognised. Vitreo
    // recognises none, and claiming one would have the caller build on a
    // behaviour that is not there; asking for none is a success (measured,
    // a01 misc "D3D12EnableExperimentalFeatures(0)": S_OK).
    const u32 count = m.arg32(0);
    if (count == 0) {
        m.set_return(S_OK);
        return;
    }
    d3d12_diag("D3D12EnableExperimentalFeatures(%u) -> E_NOINTERFACE", count);
    m.set_return(E_NOINTERFACE);
}

void Frontend::D3D12GetInterface(Marshaler& m) {
    // D3D12GetInterface(REFCLSID rclsid, REFIID riid, void **ppvDebug).
    // Measured (a01 misc): a refused pair leaves *ppvDebug untouched.
    GuidBytes clsid{};
    if (!read_guid(m.arg_ptr(0), clsid)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (guid_equal(clsid, CLSID_D3D12DEBUG)) {
        // The debug layer (Graphics Tools) is not installed: as
        // D3D12GetDebugInterface answers, whatever the IID.
        m.set_return(DXGI_ERROR_SDK_COMPONENT_MISSING);
        return;
    }
    if (guid_equal(clsid, CLSID_D3D12DEVICEREMOVEDEXTENDEDDATA) ||
        guid_equal(clsid, CLSID_D3D12SDKCONFIGURATION) ||
        guid_equal(clsid, CLSID_D3D12DEVICEFACTORY)) {
        unimplemented("D3D12GetInterface",
                      "DRED settings / SDK configuration / device factory "
                      "objects");
    }
    // CLSID_D3D12Tools and CLSID_D3D12DSRDeviceFactory are refused on WARP
    // too; any other CLSID is unknown.
    m.set_return(E_NOINTERFACE);
}

}  // namespace d3d12

// =============================================================================
// D3D12Manager
// =============================================================================

std::string d3d12_method_export_name(const char* interface_name, usize slot) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "__vitreo_d3d12_%s_%zu",
                  interface_name, slot);
    return buffer;
}

std::string d3d12_device_method_export_name(usize slot) {
    return d3d12_method_export_name("Device", slot);
}

D3D12Manager::D3D12Manager(GuestMemory& memory, bool is_64bit,
                           graphics::GraphicsBackendDevice& backend)
    : m_frontend(std::make_unique<d3d12::Frontend>(memory, is_64bit,
                                                   backend)),
      m_memory(memory), m_is_64bit(is_64bit) {
    m_frontend->m_create_blob = [this](const u8* bytes, usize size) {
        return create_blob(bytes, size);
    };
}

D3D12Manager::~D3D12Manager() = default;

void D3D12Manager::set_method_thunks(MethodThunks thunks) {
    m_frontend->set_method_thunks(thunks);
    std::lock_guard lock(m_object_pool_mutex);
    m_method_thunks = std::move(thunks);
    m_storage_ready = false;
}

// -- The root-signature pass's object pool (ID3DBlob, deserializers) --------

bool D3D12Manager::write_pointer(GuestAddr address, GuestAddr value) {
    return m_is_64bit
               ? m_memory.write_u64(address, value).is_ok()
               : m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

bool D3D12Manager::read_guid(GuestAddr address, GuidBytes& out) const {
    if (address == 0) return false;
    return m_memory.read_bytes(address, out.data(), out.size()).is_ok();
}

bool D3D12Manager::ensure_storage() {
    // Guest storage for the three root-signature interfaces: their vtables,
    // then a pool of 0x40-byte objects, recycled on final Release.
    constexpr usize kStorageSize = 0x20000;
    constexpr usize kVtableRegion = 0x1000;
    if (m_storage_ready) return true;
    if (m_storage == 0) {
        const GuestAddr base = m_memory.map_first_fit(
            0x64000000ull, kStorageSize, MemoryProtection::ReadWrite);
        if (base == 0) return false;
        m_storage = base;
        m_blob_vtable = base + 0x000;
        m_root_signature_deserializer_vtable = base + 0x040;
        m_versioned_root_signature_deserializer_vtable = base + 0x080;
        m_next_object = base + kVtableRegion;
    }
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    const auto write_vtable = [&](GuestAddr vtable, const GuestAddr* thunks,
                                  usize count) {
        for (usize slot = 0; slot < count; ++slot) {
            if (thunks[slot] == 0 ||
                !write_pointer(vtable + slot * pointer_size, thunks[slot])) {
                return false;
            }
        }
        return true;
    };
    if (!write_vtable(m_blob_vtable, m_method_thunks.blob.data(),
                      d3d12::BLOB_METHOD_COUNT) ||
        !write_vtable(m_root_signature_deserializer_vtable,
                      m_method_thunks.root_signature_deserializer.data(),
                      d3d12::ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT) ||
        !write_vtable(
            m_versioned_root_signature_deserializer_vtable,
            m_method_thunks.versioned_root_signature_deserializer.data(),
            d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT)) {
        return false;
    }
    m_storage_ready = true;
    return true;
}

GuestAddr D3D12Manager::allocate_object(GuestAddr D3D12Manager::* vtable) {
    constexpr usize kObjectSize = 0x40;
    constexpr usize kStorageSize = 0x20000;
    const std::lock_guard<std::mutex> lock(m_object_pool_mutex);
    if (!ensure_storage()) return 0;
    const GuestAddr vtable_address = this->*vtable;
    if (vtable_address == 0) return 0;
    GuestAddr address = 0;
    if (!m_free_objects.empty()) {
        address = m_free_objects.back();
        m_free_objects.pop_back();
    } else {
        address = m_next_object;
        if (address + kObjectSize > m_storage + kStorageSize) return 0;
        m_next_object += kObjectSize;
    }
    if (!write_pointer(address, vtable_address)) return 0;
    return address;
}

void D3D12Manager::free_object(GuestAddr address) {
    if (address == 0) return;
    const std::lock_guard<std::mutex> lock(m_object_pool_mutex);
    // Null the vtable pointer, so a use after the final Release faults at
    // the mistake instead of dispatching into the next object.
    (void)write_pointer(address, 0);
    m_free_objects.push_back(address);
}

void D3D12Manager::set_guest_function_caller(GuestFunctionCaller caller) {
    m_frontend->set_guest_function_caller(std::move(caller));
}

bool D3D12Manager::is_device(GuestAddr address) const {
    return m_frontend->is_device(address);
}

bool D3D12Manager::is_command_queue(GuestAddr address) const {
    return m_frontend->is_command_queue(address);
}

void D3D12Manager::impl_D3D12CreateDevice(Marshaler& m) {
    m_frontend->D3D12CreateDevice(m);
}
void D3D12Manager::impl_D3D12GetDebugInterface(Marshaler& m) {
    m_frontend->D3D12GetDebugInterface(m);
}
void D3D12Manager::impl_D3D12EnableExperimentalFeatures(Marshaler& m) {
    m_frontend->D3D12EnableExperimentalFeatures(m);
}
void D3D12Manager::impl_D3D12GetInterface(Marshaler& m) {
    m_frontend->D3D12GetInterface(m);
}
void D3D12Manager::impl_Method(Marshaler& m, usize table, usize slot) {
    m_frontend->method(m, table, slot);
}

DllExports create_d3d12_thunks(D3D12Manager& manager) {
    DllExports exports;
    const auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes,
                         void (D3D12Manager::*method)(Marshaler&)) {
        exports.entries.push_back({
            name, ordinal,
            [&manager, method](Marshaler& m) { (manager.*method)(m); },
            stdcall_bytes});
    };
    // Cleanup counts from the SDK prototypes: every parameter is a pointer or a
    // 32-bit value, so each is one x86 stack slot. These are module exports,
    // not vtable slots, so there is no `this`.
    //
    // Ordinals are the real d3d12.dll's, read off its export table on Windows
    // (ordinal base 99). They are not decoration: the SDK's d3d12.lib binds
    // D3D12CreateDevice and D3D12GetDebugInterface BY ORDINAL, so a
    // delay-import of #101 and #102 cannot find them by name at all.
    add("D3D12CreateDevice", 101, 16, &D3D12Manager::impl_D3D12CreateDevice);
    add("D3D12GetDebugInterface", 102, 8,
        &D3D12Manager::impl_D3D12GetDebugInterface);
    add("D3D12SerializeRootSignature", 115, 16,
        &D3D12Manager::impl_D3D12SerializeRootSignature);
    add("D3D12SerializeVersionedRootSignature", 116, 12,
        &D3D12Manager::impl_D3D12SerializeVersionedRootSignature);
    // Four parameters each: the SDK's x86 d3d12.lib decorates both @16.
    add("D3D12CreateRootSignatureDeserializer", 106, 16,
        &D3D12Manager::impl_D3D12CreateRootSignatureDeserializer);
    add("D3D12CreateVersionedRootSignatureDeserializer", 107, 16,
        &D3D12Manager::impl_D3D12CreateVersionedRootSignatureDeserializer);
    add("D3D12EnableExperimentalFeatures", 109, 16,
        &D3D12Manager::impl_D3D12EnableExperimentalFeatures);
    // Ordinal 110 (Windows dumpbin /exports); three pointers.
    add("D3D12GetInterface", 110, 12, &D3D12Manager::impl_D3D12GetInterface);

    // Every slot of every interface, stdcall cleanup from the generated
    // tables (d3d12_vtables.hpp).
    for (usize table = 0; table < d3d12::INTERFACE_TABLE_COUNT; ++table) {
        const d3d12::InterfaceTable& row = d3d12::INTERFACE_TABLES[table];
        for (usize slot = 0; slot < row.method_count; ++slot) {
            exports.entries.push_back({
                d3d12_method_export_name(row.export_name, slot), 0,
                [&manager, table, slot](Marshaler& m) {
                    manager.impl_Method(m, table, slot);
                },
                row.stdcall_bytes[slot]});
        }
    }
    append_d3d12_root_signature_thunks(exports, manager);
    return exports;
}

}  // namespace vitreo::winapi
