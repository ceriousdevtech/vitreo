// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12.cpp
/// @brief d3d12.dll — the module's exported entry points, the vtable thunks,
/// and D3D12Manager's forwarding to the Frontend.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/common/adapter_device_presence.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace vitreo::winapi {

namespace d3d12 {

bool d3d12_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D12");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void unimplemented(const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::string key = std::string(api) + ": " + what;
    // A pipeline creation in flight on this thread is refused for it.
    note_pipeline_refusal(("UNIMPLEMENTED " + key).c_str());
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
    FrontendLock lock(*this);
    m_tearing_down = true;
    // A completion event still registered with the GPU event bridge names
    // the fence's backend timeline; the bridge's thread must stop watching
    // it before it is freed.
    for (const auto& [address, object] : m_objects) {
        if (object->kind == Kind::Fence) {
            GpuEventSignaler::instance().cancel(
                static_cast<Fence&>(*object).timeline->backend.get());
        }
    }
    detach_shared_payloads();
    m_command_queues.clear();
    m_objects.clear();
    m_replay.reset();
}

void Frontend::set_method_thunks(const D3D12Manager::MethodThunks& thunks) {
    FrontendLock lock(*this);
    m_thunks = thunks.tables;
    m_vtables_ready = false;
}

void Frontend::set_guest_function_caller(GuestFunctionCaller caller) {
    FrontendLock lock(*this);
    m_guest_call = std::move(caller);
}

bool Frontend::is_device(GuestAddr address) {
    FrontendLock lock(*this);
    return find<Device>(address, Kind::Device) != nullptr;
}

bool Frontend::is_command_queue(GuestAddr address) {
    FrontendLock lock(*this);
    return find<CommandQueue>(address, Kind::CommandQueue) != nullptr;
}

// =============================================================================
// Exported entry points
// =============================================================================

void Frontend::D3D12CreateDevice(Marshaler& m) {
    // D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL
    //                   MinimumFeatureLevel, REFIID riid, void **ppDevice).
    FrontendLock lock(*this);
    m.set_return(create_device(m.arg_ptr(0), m.arg32(1), m.arg_ptr(2),
                               m.arg_ptr(3), nullptr));
}

u32 Frontend::create_device(GuestAddr adapter, u32 minimum_feature_level,
                            GuestAddr iid_address, GuestAddr device_out,
                            DeviceFactory* factory) {
    if (device_out != 0) (void)write_pointer(device_out, 0);

    if (!d3d12_enabled()) {
        // An explicit diagnostic opt-out still reports the real unsupported
        // result, so guests can choose another available graphics API.
        d3d12_diag("D3D12CreateDevice -> DXGI_ERROR_UNSUPPORTED "
                   "(disabled by VITREO_D3D12=0)");
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!m_backend.available()) {
        d3d12_diag("D3D12CreateDevice -> DXGI_ERROR_UNSUPPORTED (no backend)");
        return DXGI_ERROR_UNSUPPORTED;
    }
    // Measured (d3d12_a01_device_probe): NULL is the default adapter.
    // A live IDXGIAdapter is that adapter. A factory, or any other pointer,
    // is E_INVALIDARG and leaves *ppDevice NULL.
    if (adapter != 0 &&
        (m_presentation.dxgi == nullptr ||
         !m_presentation.dxgi->is_adapter_object(adapter))) {
        d3d12_diag("D3D12CreateDevice -> E_INVALIDARG (pAdapter 0x%llx is not "
                   "an adapter)",
                   static_cast<unsigned long long>(adapter));
        return E_INVALIDARG;
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
        return known ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    }
    if (device_out == 0) {
        // Learn: with ppDevice null the call only reports whether a device
        // could be created, S_FALSE when it could.
        return S_FALSE;
    }
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) {
        return E_INVALIDARG;
    }
    // Learn: "Direct3D 12 devices are singletons per adapter. If a Direct3D 12
    // device already exists in the current process for a given adapter, then
    // a subsequent call to D3D12CreateDevice returns the existing device."
    // A device factory's devices are its own: measured (a01c factory,
    // a01d factory_state), CreateDevice makes a new device beside a live
    // singleton -- even with ALLOW_RETURNING_EXISTING_DEVICE, and beside a
    // live device of the same factory -- and never becomes the singleton.
    if (factory == nullptr) {
        if (Device* existing = find<Device>(m_device, Kind::Device)) {
            const GuestAddr answer = query_interface(*existing, iid);
            if (answer == 0) return E_NOINTERFACE;
            if (!write_pointer(device_out, answer)) {
                (void)release(existing->address);
                return E_POINTER;
            }
            return S_OK;
        }
    }
    auto device = std::make_unique<Device>();
    device->feature_level = device_level;
    // Measured on WARP and NVIDIA (a01d config, factory_state): every
    // device's configuration reports bit 0x40000000 beside the DRED bits of
    // the state it was created under (undocumented; a factory's own
    // configuration reports the DRED bits alone).
    device->configuration_flags =
        0x40000000u |
        device_configuration_flags(factory != nullptr ? factory->dred : m_dred);
    if (!answers(*device, iid) &&
        tearoff_table(*device, iid) == INTERFACE_TABLE_COUNT) {
        return E_NOINTERFACE;
    }
    const GuestAddr address = publish(std::move(device));
    if (address == 0) return E_OUTOFMEMORY;
    if (factory == nullptr) m_device = address;
    Device* created = find<Device>(address, Kind::Device);
    // The creation reference moves to the interface asked for.
    const GuestAddr answer = query_interface(*created, iid);
    (void)release(address);
    if (answer == 0 || !write_pointer(device_out, answer)) {
        if (answer != 0) (void)release(address);
        return E_POINTER;
    }
    graphics_common::note_adapter_device_created();
    d3d12_diag("D3D12CreateDevice adapter=0x%llx level=0x%x -> 0x%llx (%s)",
               static_cast<unsigned long long>(adapter), minimum_feature_level,
               static_cast<unsigned long long>(address),
               m_backend.backend_device_name().c_str());
    return S_OK;
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
    FrontendLock lock(*this);
    GuidBytes clsid{};
    if (!read_guid(m.arg_ptr(0), clsid)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(get_configuration_interface(clsid, m.arg_ptr(1), m.arg_ptr(2),
                                             nullptr));
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

namespace {

std::mutex g_d3d12_by_memory_mu;
std::unordered_map<const GuestMemory*, D3D12Manager*> g_d3d12_by_memory;

}  // namespace

D3D12Manager::D3D12Manager(GuestMemory& memory, bool is_64bit,
                           graphics::GraphicsBackendDevice& backend)
    : m_frontend(std::make_unique<d3d12::Frontend>(memory, is_64bit,
                                                   backend)),
      m_memory(memory), m_is_64bit(is_64bit) {
    m_frontend->m_create_blob = [this](const u8* bytes, usize size) {
        return create_blob(bytes, size);
    };
    m_frontend->m_serialize_versioned_root_signature = [this](Marshaler& m) {
        serialize_root_signature_export(m, true, 1);
    };
    m_frontend->m_create_versioned_deserializer = [this](Marshaler& m) {
        create_root_signature_deserializer(m, true, 1);
    };
    // DirectComposition is constructed beside this manager and finds this
    // frontend by guest memory: SetContent binds composition chains, and
    // DCompositionCreateDevice retains an ID3D12Device or command queue.
    std::lock_guard lock(g_d3d12_by_memory_mu);
    g_d3d12_by_memory[&memory] = this;
}

D3D12Manager::~D3D12Manager() {
    std::lock_guard lock(g_d3d12_by_memory_mu);
    const auto found = g_d3d12_by_memory.find(&m_memory);
    if (found != g_d3d12_by_memory.end() && found->second == this) {
        g_d3d12_by_memory.erase(found);
    }
}

D3D12Manager* D3D12Manager::for_guest_memory(const GuestMemory& memory) {
    std::lock_guard lock(g_d3d12_by_memory_mu);
    const auto found = g_d3d12_by_memory.find(&memory);
    return found == g_d3d12_by_memory.end() ? nullptr : found->second;
}

bool D3D12Manager::is_swap_chain(GuestAddr address) const {
    d3d12::FrontendLock lock(*m_frontend);
    return m_frontend->find<d3d12::SwapChain>(address, d3d12::Kind::SwapChain) !=
           nullptr;
}

bool D3D12Manager::retain_swap_chain_reference(GuestAddr address) {
    d3d12::FrontendLock lock(*m_frontend);
    d3d12::SwapChain* chain =
        m_frontend->find<d3d12::SwapChain>(address, d3d12::Kind::SwapChain);
    if (chain == nullptr) return false;
    m_frontend->add_ref(*chain);
    return true;
}

void D3D12Manager::release_swap_chain_reference(GuestAddr address) {
    d3d12::FrontendLock lock(*m_frontend);
    if (m_frontend->find<d3d12::SwapChain>(address, d3d12::Kind::SwapChain) ==
        nullptr) {
        return;
    }
    (void)m_frontend->release(address);
}

bool D3D12Manager::bind_composition_swap_chain(
    GuestAddr address, GuestAddr hwnd,
    const graphics::BackendCompositionLayerDesc& layer) {
    d3d12::FrontendLock lock(*m_frontend);
    return m_frontend->bind_composition_swap_chain(address, hwnd, layer);
}

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

bool D3D12Manager::retain_rendering_device(GuestAddr address) {
    d3d12::FrontendLock lock(*m_frontend);
    if (d3d12::Device* device =
            m_frontend->find<d3d12::Device>(address, d3d12::Kind::Device)) {
        m_frontend->add_ref(*device);
        return true;
    }
    if (d3d12::CommandQueue* queue = m_frontend->find<d3d12::CommandQueue>(
            address, d3d12::Kind::CommandQueue)) {
        m_frontend->add_ref(*queue);
        return true;
    }
    return false;
}

void D3D12Manager::release_rendering_device(GuestAddr address) {
    d3d12::FrontendLock lock(*m_frontend);
    if (m_frontend->find<d3d12::Device>(address, d3d12::Kind::Device) ==
            nullptr &&
        m_frontend->find<d3d12::CommandQueue>(
            address, d3d12::Kind::CommandQueue) == nullptr) {
        return;
    }
    (void)m_frontend->release(address);
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
// -----------------------------------------------------------------------------
// Runtime-internal exports. The mingw-w64 (and Windows SDK) import library
// binds them (libd3d12.a: _D3D12PIXGetThreadInfo@0,
// _D3D12PIXEventsReplaceBlock@4, _D3D12PIXNotifyWakeFromFenceSignal@4,
// _D3D12PIXReportCounter@8, _GetBehaviorValue@8, _SetAppCompatStringPointer@8,
// _D3D12CoreCreateLayeredDevice@20, _D3D12CoreGetLayeredDeviceSize@8,
// _D3D12CoreRegisterLayers@8), so a module importing one must load.
// Behaviour measured on WARP and NVIDIA, no PIX attached
// (d3d12_a12_sessions_probe exports).
// -----------------------------------------------------------------------------

void D3D12Manager::impl_D3D12PIXGetThreadInfo(Marshaler& m) {
    // PIXEventsThreadInfo *(void): the WinPixEventRuntime's per-thread
    // capture block. Measured: x64 answers a per-thread block (the same
    // pointer on every call of a thread) whose block / limit / destination
    // pointers are all NULL -- nothing is captured; x86 answers NULL.
    if (!m_is_64bit) {
        m.set_return(0);
        return;
    }
    thread_local std::unordered_map<const D3D12Manager*, GuestAddr> blocks;
    GuestAddr& block = blocks[this];
    if (block == 0) {
        // A zeroed page of the thread's own, kept for the process.
        block = m_memory.map_first_fit(0x10000, 0x1000,
                                       MemoryProtection::ReadWrite);
        if (block != 0) {
            for (u32 i = 0; i < 64; i += 8) (void)m_memory.write_u64(block + i, 0);
        }
    }
    m.set_return(block);
}

void D3D12Manager::impl_D3D12PIXEventsReplaceBlock(Marshaler& m) {
    // UINT64 *(bool getEarliestTime): asked for only when a capture block
    // fills, which never happens with the NULL blocks above (Windows faults
    // when it is called with no capture attached).
    m.set_return(0);
}

void D3D12Manager::impl_D3D12PIXNotifyWakeFromFenceSignal(Marshaler& m) {
    // void (HANDLE): a PIX timing note; the event is not touched (measured).
    m.set_return(0);
}

void D3D12Manager::impl_D3D12PIXReportCounter(Marshaler& m) {
    // void (PCWSTR name, float value): PIX counters; nothing records them.
    m.set_return(0);
}

void D3D12Manager::impl_D3D12RuntimeInternal(Marshaler& m, const char* name) {
    // GetBehaviorValue, SetAppCompatStringPointer and the D3D12Core* layer
    // entry points: undocumented runtime / debug-layer plumbing with no
    // public contract to build from.
    d3d12::unimplemented(name, "undocumented runtime-internal entry point");
    m.set_return(d3d12::E_NOTIMPL);
}

GuestAddr D3D12Manager::device_removed_extended_data(
    const std::function<GuestAddr(usize)>& allocate) {
    // D3D12_VERSIONED_DEVICE_REMOVED_EXTENDED_DATA (Windows SDK
    // 10.0.26100.0 um/d3d12.h). Measured (a12 exports): Version 3
    // (D3D12_DRED_VERSION_1_2), the rest zero, in read-write memory.
    const GuestAddr data = allocate(256);
    if (data == 0) return 0;
    for (u32 i = 0; i < 256; i += 4) (void)m_memory.write_u32(data + i, 0);
    (void)m_memory.write_u32(data, 3 /* D3D12_DRED_VERSION_1_2 */);
    return data;
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
    // Runtime-internal, by name (cleanup from libd3d12.a's decorations).
    add("D3D12PIXGetThreadInfo", 0, 0, &D3D12Manager::impl_D3D12PIXGetThreadInfo);
    add("D3D12PIXEventsReplaceBlock", 0, 4,
        &D3D12Manager::impl_D3D12PIXEventsReplaceBlock);
    add("D3D12PIXNotifyWakeFromFenceSignal", 0, 4,
        &D3D12Manager::impl_D3D12PIXNotifyWakeFromFenceSignal);
    add("D3D12PIXReportCounter", 0, 8,
        &D3D12Manager::impl_D3D12PIXReportCounter);
    const auto internal = [&](const char* name, u16 stdcall_bytes) {
        exports.entries.push_back({
            name, 0,
            [&manager, name](Marshaler& m) {
                manager.impl_D3D12RuntimeInternal(m, name);
            },
            stdcall_bytes});
    };
    internal("GetBehaviorValue", 8);
    internal("SetAppCompatStringPointer", 8);
    internal("D3D12CoreCreateLayeredDevice", 20);
    internal("D3D12CoreGetLayeredDeviceSize", 8);
    internal("D3D12CoreRegisterLayers", 8);

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
