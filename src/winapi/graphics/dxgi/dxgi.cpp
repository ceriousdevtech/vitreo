// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/dxgi/dxgi_internal.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/guest_com.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/common/virtual_display_modes.hpp"
#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/common/live_diag.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

constexpr u32 OUTPUT_VTABLE_SLOTS = DxgiManager::OUTPUT_METHOD_COUNT;

constexpr std::array<u32, OUTPUT_VTABLE_SLOTS> OUTPUT_STDCALL_BYTES{{
    12, 4, 4, 16, 12, 16, 12, 8, 20, 16, 4, 12, 4, 8, 8, 8, 8, 8, 8,
    20, 16, 8, 12, 4, 16, 20, 24, 8, 8}};

/// Opt-in PCI identity override, mirroring VITREO_D3D9_ADAPTER_ID.
///
/// The truthful default is Apple's vendor ID with no device ID, because that is
/// what the backend actually is. Some guests gate features on the PCI identity
/// rather than on capabilities. GPU rasterization is gated on a vendor
/// allowlist (NVIDIA, Intel, AMD, some Qualcomm). Apple's vendor ID takes
/// the software-rasterization fallback, so every tile is rasterized on the
/// guest CPU.
///
/// Overriding lets a run present a known identity without baking a per-title
/// lie into the default path. The per-capsule knob is the display-class
/// registry key (see display_device_identity.hpp); VITREO_DXGI_ADAPTER_ID
/// ("vendor:device", hex) is the per-run override that wins over it.
[[nodiscard]] std::pair<u32, u32> dxgi_adapter_pci_identity() {
    namespace identity = display_device_identity;
    const auto spec =
        identity::parse_vendor_device_spec(std::getenv("VITREO_DXGI_ADAPTER_ID"));
    if (spec) return *spec;
    const auto& configured = identity::adapter_identity();
    return {configured.vendor_id.value_or(dxgi::VENDOR_ID_APPLE),
            configured.device_id.value_or(0u)};
}

[[nodiscard]] dxgi::AdapterInfo detect_adapter_info() {
    namespace identity = display_device_identity;
    dxgi::AdapterInfo info;
    const auto [vendor_id, device_id] = dxgi_adapter_pci_identity();
    const auto& configured = identity::adapter_identity();
    info.vendor_id = vendor_id;
    info.device_id = device_id;
    info.subsystem_id = configured.subsystem_id.value_or(0u);
    info.revision = configured.revision.value_or(0u);
    // A configured identity owns the whole tuple: reporting a Radeon PCI
    // pair next to "Apple M4 Pro" is a contradiction a guest can see. With
    // nothing configured this is the backend's device name; the registry
    // mirrors of the adapter name are published from the same resolution.
    info.description = identity::resolved_adapter_description();
    const auto* device = graphics::active_backend_device();
    if (device == nullptr || !device->available()) return info;
    // Adapter profile policy (.docs/contracts/dxgi.md "Adapter profile policy
    // (discrete-class)"): the adapter is presented as a discrete-class GPU.
    // Metal's recommended working set -- the ceiling of the LOCAL budget
    // QueryVideoMemoryInfo reports -- is its dedicated video memory. The host
    // has no second, slower pool behind it: memory the GPU uses beyond the
    // working set is the same memory, over budget. So no system memory is
    // reported as shared, matching the empty NON_LOCAL segment group (Learn
    // D3D12_MEMORY_POOL: on a non-UMA adapter NON_LOCAL is the system-memory
    // pool). Reporting the working set as shared as well would count the
    // same bytes twice. Do not derive a made-up fraction from total RAM.
    info.dedicated_video_memory = device->recommended_working_set_size();
    info.dedicated_system_memory = 0;
    info.shared_system_memory = 0;
    return info;
}

} // namespace

std::string dxgi_output_method_export_name(usize slot) {
    return std::string("_DXGI_Output_") + std::to_string(slot);
}

usize DxgiManager::GuidHash::operator()(const GuidBytes& guid) const noexcept {
    usize hash = 1469598103934665603ull;
    for (const u8 byte : guid) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

DxgiManager::DxgiManager(GuestMemory& memory, bool is_64bit,
                         dxgi::AdapterInfo adapter_info)
    : m_memory(memory)
    , m_is_64bit(is_64bit)
    , m_adapter_info(std::move(adapter_info)) {
    m_adapter_info_is_explicit = !m_adapter_info.description.empty();
    if (!m_adapter_info_is_explicit) {
        m_adapter_info = detect_adapter_info();
    }
}

DxgiManager::~DxgiManager() {
    // Guest execution is already shut down; do not re-enter it while tearing
    // down the process's object tables or the injected callback's captures.
    m_shutting_down = true;
}

int& DxgiManager::api_lock_depth() const noexcept {
    // Count the shared owner lock, not unrelated graphics managers on this
    // thread (tests and host tools can construct more than one owner).
    thread_local std::unordered_map<const DxgiManager*, int> depths;
    // WP-R1 (com_direct): every guest entry asks for its depth, so remember
    // the last owner's node. Exact: a node of `depths` is never erased and
    // never moves (unordered_map references survive rehashing), and an owner
    // re-created at a reused address keys the same node, as the lookup does.
    if (com_direct::active()) {
        struct Last {
            const DxgiManager* owner{nullptr};
            int* depth{nullptr};
        };
        thread_local Last last;
        if (last.owner == this) return *last.depth;
        int& depth = depths[this];
        last = Last{this, &depth};
        return depth;
    }
    return depths[this];
}

u32 DxgiManager::guest_reference(GuestAddr address, usize slot) {
    const auto caller = m_guest_call;
    ApiUnlock unlocked(*this);
    return static_cast<u32>(call_guest_com_slot(
        m_memory, m_is_64bit, caller, address, slot));
}

DxgiManager::PrivateInterface DxgiManager::own_private_interface(GuestAddr address) {
    return PrivateInterface(new GuestAddr(address), [this](GuestAddr* owned) {
        const GuestAddr pointer = *owned;
        delete owned;
        if (pointer != 0 && !m_shutting_down)
            (void)guest_reference(pointer, COM_SLOT_RELEASE);
    });
}

void DxgiManager::refresh_adapter_info() {
    ApiLock lock(*this);
    if (m_adapter_info_is_explicit) return;
    m_adapter_info = detect_adapter_info();
}

void DxgiManager::set_method_thunks(MethodThunks thunks) {
    ApiLock lock(*this);
    m_method_thunks = thunks;
}

void DxgiManager::set_swap_chain_creator(SwapChainCreator creator) {
    ApiLock lock(*this);
    m_swap_chain_creator = std::move(creator);
}

void DxgiManager::set_swap_chain_for_composition_creator(
    SwapChainCreator creator) {
    ApiLock lock(*this);
    m_swap_chain_for_composition_creator = std::move(creator);
}

void DxgiManager::set_swap_chain_for_hwnd_creator(SwapChainCreator creator) {
    ApiLock lock(*this);
    m_swap_chain_for_hwnd_creator = std::move(creator);
}

void DxgiManager::set_output_resolver(OutputResolver resolver) {
    ApiLock lock(*this);
    m_output_resolver = std::move(resolver);
}

void DxgiManager::set_guest_function_caller(GuestFunctionCaller caller) {
    ApiLock lock(*this);
    m_guest_call = std::move(caller);
}

void DxgiManager::create_factory(Marshaler& m) {
    create_factory_at(m, m.arg_ptr(0), m.arg_ptr(1));
}

void DxgiManager::create_factory_at(Marshaler& m, GuestAddr iid,
                                    GuestAddr factory_out) {
    if (factory_out == 0) {
        m.set_return(dxgi::E_POINTER);
        return;
    }
    (void)m.write_ptr(factory_out, 0);
    if (!iid_is_supported(iid, ObjectType::Factory)) {
        VITREO_TRACE("[dxgi] CreateDXGIFactory unsupported IID at 0x%llx\n",
                     static_cast<unsigned long long>(iid));
        dxgi_diag("CreateDXGIFactory unsupported IID address=0x%llx",
                  static_cast<unsigned long long>(iid));
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }

    // DXGI snapshots the available adapters when a factory is created. The
    // runtime publishes its graphics backend after manager members are
    // constructed, so the constructor's adapter probe can legitimately be
    // early. Retry at this Windows-visible snapshot boundary before deciding
    // that the system has no adapter.
    if (m_adapter_info.description.empty()) {
        m_adapter_info = detect_adapter_info();
    }

    const GuestAddr factory = create_factory_for_iid(iid);
    if (factory == 0 || m.write_ptr(factory_out, factory).is_err()) {
        m.set_return(dxgi::E_POINTER);
        return;
    }
    VITREO_TRACE("[dxgi] CreateDXGIFactory%s -> 0x%llx\n",
                 iid_is_factory1(iid) ? "1" : "",
                 static_cast<unsigned long long>(factory));
    dxgi_diag("CreateDXGIFactory%s -> 0x%llx",
              iid_is_factory1(iid) ? "1" : "",
              static_cast<unsigned long long>(factory));
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_CreateDXGIFactory(Marshaler& m) {
    // The DXGI 1.0 entry point refuses IDXGIFactory1 -- and only that IID:
    // IDXGIFactory2..7 are answered (measured, dxgi_a11_contracts_probe
    // [factory.create]; Learn: "use CreateDXGIFactory1 for IDXGIFactory1").
    const GuestAddr factory_out = m.arg_ptr(1);
    if (factory_out != 0 && iid_is_factory1(m.arg_ptr(0))) {
        (void)m.write_ptr(factory_out, 0);
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    create_factory(m);
}

void DxgiManager::impl_CreateDXGIFactory2(Marshaler& m) {
    // HRESULT CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
    // The only defined flag is DXGI_CREATE_FACTORY_DEBUG (0x1), which needs
    // the SDK debug layer (dxgidebug.dll). Measured on Windows 11 without it:
    // DXGI_ERROR_INVALID_CALL. Undefined bits are accepted and echoed by
    // GetCreationFlags (0x2, 0x4); bit 31 is dropped. Callers import this
    // entry point statically for both a D3D12 and a D3D11 backend, so a
    // missing export takes both down at once.
    const u32 flags = static_cast<u32>(m.arg(0));
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr factory_out = m.arg_ptr(2);
    if (factory_out != 0) (void)m.write_ptr(factory_out, 0);
    if ((flags & 0x1u) != 0) {
        dxgi_diag("CreateDXGIFactory2 debug layer requested (not installed) "
                  "-> DXGI_ERROR_INVALID_CALL");
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    create_factory_at(m, iid, factory_out);
    const GuestAddr factory =
        factory_out != 0 ? m.read_ptr(factory_out).value_or(0) : 0;
    if (auto created = m_objects.find(factory);
        created != m_objects.end() &&
        created->second.type == ObjectType::Factory) {
        created->second.creation_flags = flags & 0x7FFFFFFFu;
    }
}

void DxgiManager::impl_DXGIGetDebugInterface1(Marshaler& m) {
    // HRESULT DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug)
    // The debug interfaces live in dxgidebug.dll, an SDK component.
    (void)m.arg(0);
    (void)m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out != 0) (void)m.write_ptr(out, 0);
    m.set_return(dxgi::ERROR_SDK_COMPONENT_MISSING);
}

void DxgiManager::impl_DXGIDeclareAdapterRemovalSupport(Marshaler& m) {
    // HRESULT DXGIDeclareAdapterRemovalSupport(): the process declares it
    // handles DXGI_ERROR_DEVICE_REMOVED. There is nothing to configure.
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_CreateDXGIFactory1(Marshaler& m) {
    create_factory(m);
}

namespace {

/// A DXGI COM method as its export's direct entry (DirectHandler): the
/// shared graphics lock every guest entry takes (DxgiManager::ApiLock), then
/// the method -- the export's whole body, which make_direct_export also
/// makes its `handler` (cli::run_direct_thunk calls it without the general
/// dispatch).
template <void (DxgiManager::*Method)(Marshaler&)>
void dxgi_com_method(void* context, Marshaler& m, u32 /*slot*/) {
    auto& manager = *static_cast<DxgiManager*>(context);
    DxgiManager::ApiLock lock(manager);
    (manager.*Method)(m);
}

template <void (DxgiManager::*Method)(Marshaler&, usize)>
void dxgi_com_slot_method(void* context, Marshaler& m, u32 slot) {
    auto& manager = *static_cast<DxgiManager*>(context);
    DxgiManager::ApiLock lock(manager);
    (manager.*Method)(m, slot);
}

template <void (DxgiManager::*Method)(Marshaler&)>
[[nodiscard]] DirectHandler dxgi_com_entry(DxgiManager& manager) {
    return DirectHandler{&dxgi_com_method<Method>, &manager, 0};
}

template <void (DxgiManager::*Method)(Marshaler&, usize)>
[[nodiscard]] DirectHandler dxgi_com_slot_entry(DxgiManager& manager,
                                                usize slot) {
    return DirectHandler{&dxgi_com_slot_method<Method>, &manager,
                         static_cast<u32>(slot)};
}

}  // namespace

DllExports create_dxgi_thunks(DxgiManager& manager) {
    DllExports exports;
    exports.entries.push_back({"CreateDXGIFactory", 0,
        [&manager](Marshaler& m) { manager.impl_CreateDXGIFactory(m); }, 8});
    exports.entries.push_back({"CreateDXGIFactory1", 0,
        [&manager](Marshaler& m) { manager.impl_CreateDXGIFactory1(m); }, 8});
    exports.entries.push_back({"CreateDXGIFactory2", 0,
        [&manager](Marshaler& m) { manager.impl_CreateDXGIFactory2(m); }, 12});
    exports.entries.push_back({"DXGIGetDebugInterface1", 0,
        [&manager](Marshaler& m) { manager.impl_DXGIGetDebugInterface1(m); }, 12});
    exports.entries.push_back({"DXGIDeclareAdapterRemovalSupport", 0,
        [&manager](Marshaler& m) {
            manager.impl_DXGIDeclareAdapterRemovalSupport(m); }, 0});
    // HRESULT DXGIDisableVBlankVirtualization(void) (Learn): opts the
    // process out of the v-blank virtualisation Windows applies to
    // secondary displays. Vitreo has no such virtualisation (WaitForVBlank
    // paces from the output's own refresh rate), so there is none to turn
    // off and the call succeeds as it does on Windows.
    exports.entries.push_back({"DXGIDisableVBlankVirtualization", 0,
        [](Marshaler& m) { m.set_return(dxgi::S_OK); }, 0});
    exports.entries.push_back(make_direct_export(
        "_DXGI_QueryInterface",
        dxgi_com_entry<&DxgiManager::impl_QueryInterface>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_AddRef",
        dxgi_com_entry<&DxgiManager::impl_AddRef>(manager), 4));
    exports.entries.push_back(make_direct_export(
        "_DXGI_Release",
        dxgi_com_entry<&DxgiManager::impl_Release>(manager), 4));
    exports.entries.push_back(make_direct_export(
        "_DXGI_SetPrivateData",
        dxgi_com_entry<&DxgiManager::impl_SetPrivateData>(manager), 16));
    exports.entries.push_back(make_direct_export(
        "_DXGI_SetPrivateDataInterface",
        dxgi_com_entry<&DxgiManager::impl_SetPrivateDataInterface>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetPrivateData",
        dxgi_com_entry<&DxgiManager::impl_GetPrivateData>(manager), 16));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetParent",
        dxgi_com_entry<&DxgiManager::impl_GetParent>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumAdapters",
        dxgi_com_entry<&DxgiManager::impl_EnumAdapters>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_MakeWindowAssociation",
        dxgi_com_entry<&DxgiManager::impl_MakeWindowAssociation>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetWindowAssociation",
        dxgi_com_entry<&DxgiManager::impl_GetWindowAssociation>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_CreateSwapChain",
        dxgi_com_entry<&DxgiManager::impl_CreateSwapChain>(manager), 16));
    exports.entries.push_back(make_direct_export(
        "_DXGI_CreateSoftwareAdapter",
        dxgi_com_entry<&DxgiManager::impl_CreateSoftwareAdapter>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumAdapters1",
        dxgi_com_entry<&DxgiManager::impl_EnumAdapters1>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_IsCurrent",
        dxgi_com_entry<&DxgiManager::impl_IsCurrent>(manager), 4));
    exports.entries.push_back(make_direct_export(
        "_DXGI_IsWindowedStereoEnabled",
        dxgi_com_entry<&DxgiManager::impl_IsWindowedStereoEnabled>(manager), 4));
    exports.entries.push_back(make_direct_export(
        "_DXGI_CreateSwapChainForHwnd",
        dxgi_com_entry<&DxgiManager::impl_CreateSwapChainForHwnd>(manager), 28));
    // CoreWindow chains are UWP-only: a desktop process has no CoreWindow,
    // so every call is INVALID_CALL with *ppSwapChain untouched (measured).
    exports.entries.push_back({"_DXGI_CreateSwapChainForCoreWindow", 0,
        [&manager](Marshaler& m) {
            manager.impl_Factory2Unsupported(
                m, "CreateSwapChainForCoreWindow");
        }, 24});
    exports.entries.push_back(make_direct_export(
        "_DXGI_CreateSwapChainForComposition",
        dxgi_com_entry<&DxgiManager::impl_CreateSwapChainForComposition>(manager), 20));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetSharedResourceAdapterLuid",
        dxgi_com_entry<&DxgiManager::impl_GetSharedResourceAdapterLuid>(manager), 12));
    exports.entries.push_back({"_DXGI_RegisterStereoStatusWindow", 0,
        [&manager](Marshaler& m) {
            manager.impl_RegisterStatus(m, true, true);
        }, 16});
    exports.entries.push_back({"_DXGI_RegisterStereoStatusEvent", 0,
        [&manager](Marshaler& m) {
            manager.impl_RegisterStatus(m, true, false);
        }, 12});
    exports.entries.push_back({"_DXGI_UnregisterStereoStatus", 0,
        [&manager](Marshaler& m) {
            manager.impl_UnregisterStatus(m, true);
        }, 8});
    exports.entries.push_back({"_DXGI_RegisterOcclusionStatusWindow", 0,
        [&manager](Marshaler& m) {
            manager.impl_RegisterStatus(m, false, true);
        }, 16});
    exports.entries.push_back({"_DXGI_RegisterOcclusionStatusEvent", 0,
        [&manager](Marshaler& m) {
            manager.impl_RegisterStatus(m, false, false);
        }, 12});
    exports.entries.push_back({"_DXGI_UnregisterOcclusionStatus", 0,
        [&manager](Marshaler& m) {
            manager.impl_UnregisterStatus(m, false);
        }, 8});
    // IDXGIFactory3 through IDXGIFactory7. Stdcall cleanup is 4 for `this`
    // plus one slot per 32-bit argument; EnumAdapterByLuid's LUID is 8 bytes by
    // value, so it takes two.
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetCreationFlags",
        dxgi_com_entry<&DxgiManager::impl_GetCreationFlags>(manager), 4));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumAdapterByLuid",
        dxgi_com_entry<&DxgiManager::impl_EnumAdapterByLuid>(manager), 20));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumWarpAdapter",
        dxgi_com_entry<&DxgiManager::impl_EnumWarpAdapter>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_FactoryCheckFeatureSupport",
        dxgi_com_entry<&DxgiManager::impl_FactoryCheckFeatureSupport>(manager), 16));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumAdapterByGpuPreference",
        dxgi_com_entry<&DxgiManager::impl_EnumAdapterByGpuPreference>(manager), 20));
    exports.entries.push_back(make_direct_export(
        "_DXGI_RegisterAdaptersChangedEvent",
        dxgi_com_entry<&DxgiManager::impl_RegisterAdaptersChangedEvent>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_UnregisterAdaptersChangedEvent",
        dxgi_com_entry<&DxgiManager::impl_UnregisterAdaptersChangedEvent>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_EnumOutputs",
        dxgi_com_entry<&DxgiManager::impl_EnumOutputs>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetDesc",
        dxgi_com_entry<&DxgiManager::impl_GetDesc>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_CheckInterfaceSupport",
        dxgi_com_entry<&DxgiManager::impl_CheckInterfaceSupport>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetDesc1",
        dxgi_com_entry<&DxgiManager::impl_GetDesc1>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetDesc2",
        dxgi_com_entry<&DxgiManager::impl_GetDesc2>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_RegisterHardwareContentProtectionTeardownStatusEvent",
        dxgi_com_entry<&DxgiManager::impl_RegisterHardwareContentProtectionTeardownStatusEvent>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_UnregisterHardwareContentProtectionTeardownStatus",
        dxgi_com_entry<&DxgiManager::impl_UnregisterHardwareContentProtectionTeardownStatus>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_QueryVideoMemoryInfo",
        dxgi_com_entry<&DxgiManager::impl_QueryVideoMemoryInfo>(manager), 16));
    exports.entries.push_back(make_direct_export(
        "_DXGI_SetVideoMemoryReservation",
        dxgi_com_entry<&DxgiManager::impl_SetVideoMemoryReservation>(manager), 20));
    exports.entries.push_back(make_direct_export(
        "_DXGI_RegisterVideoMemoryBudgetChangeNotificationEvent",
        dxgi_com_entry<&DxgiManager::impl_RegisterVideoMemoryBudgetChangeNotificationEvent>(manager), 12));
    exports.entries.push_back(make_direct_export(
        "_DXGI_UnregisterVideoMemoryBudgetChangeNotification",
        dxgi_com_entry<&DxgiManager::impl_UnregisterVideoMemoryBudgetChangeNotification>(manager), 8));
    exports.entries.push_back(make_direct_export(
        "_DXGI_GetDesc3",
        dxgi_com_entry<&DxgiManager::impl_GetDesc3>(manager), 8));
    for (usize slot = 0; slot < DxgiManager::OUTPUT_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            dxgi_output_method_export_name(slot),
            dxgi_com_slot_entry<&DxgiManager::impl_OutputMethod>(manager,
                                                                 slot),
            static_cast<u16>(OUTPUT_STDCALL_BYTES[slot])));
    }
    for (ExportEntry& entry : exports.entries) {
        // A COM method's direct entry takes the lock itself.
        if (entry.direct.fn != nullptr) continue;
        entry.handler = [&manager, inner = std::move(entry.handler)](Marshaler& m) {
            DxgiManager::ApiLock lock(manager);
            inner(m);
        };
    }
    return exports;
}

} // namespace vitreo::winapi
