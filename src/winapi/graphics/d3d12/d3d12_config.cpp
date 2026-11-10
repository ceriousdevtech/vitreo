// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_config.cpp
/// @brief The configuration objects D3D12GetInterface hands out --
/// ID3D12SDKConfiguration1, ID3D12DeviceFactory,
/// ID3D12DeviceRemovedExtendedDataSettings2 -- and the device's DRED, tools,
/// configuration, compatibility and pageable-tools tear-offs.
///
/// Sources: Windows SDK 10.0.26100.0 um/d3d12.h and um/d3d12compatibility.h
/// (D3D12_SDK_VERSION 616), Microsoft Learn (ID3D12SDKConfiguration1,
/// ID3D12DeviceFactory, ID3D12DeviceRemovedExtendedData2,
/// ID3D12DeviceConfiguration1, ID3D12DeviceTools1, ID3D12PageableTools,
/// ID3D12CompatibilityDevice), and what winoracle (WARP) and winoracle-gpu
/// (NVIDIA) answer: tools/oracle_probe/d3d12_a01c_config_probe and
/// d3d12_a01d_dred_tools_probe, identical on both.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>

namespace vitreo::winapi::d3d12 {

namespace {

/// D3D12_SDK_VERSION of the Windows SDK 10.0.26100.0 um/d3d12.h, and what
/// every measured configuration reports (a01d config: sdk=616).
constexpr u32 kSdkVersion = 616;

/// D3D12_ERROR_INVALID_REDIST (Windows SDK winerror.h; not in the mingw-w64
/// header): measured for CreateDeviceFactory above the runtime's SDK
/// version (a01d factory_state: 617 and 100000).
constexpr u32 kD3D12ErrorInvalidRedist = 0x887E0003u;

constexpr u32 kFactoryFlagMask =
    D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE |
    D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE |
    D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON;

constexpr u32 kCompatibilityFlagMask =
    D3D12_COMPATIBILITY_SHARED_FLAG_NON_NT_HANDLE |
    D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX |
    D3D12_COMPATIBILITY_SHARED_FLAG_9_ON_12;

bool forced_on(u32 enablement) {
    // SYSTEM_CONTROLLED is off: no device measured with it reports a DRED
    // bit (a01d config: flags=0x40000000).
    return enablement == D3D12_DRED_ENABLEMENT_FORCED_ON;
}

/// Whether @p bytes is a DXBC container with a part named @p fourcc.
bool container_has_part(const std::vector<u8>& bytes, const char* fourcc) {
    if (bytes.size() < 32 || std::memcmp(bytes.data(), "DXBC", 4) != 0) {
        return false;
    }
    u32 count = 0;
    std::memcpy(&count, bytes.data() + 28, 4);
    if (count > (bytes.size() - 32) / 4) return false;
    for (u32 i = 0; i < count; ++i) {
        u32 offset = 0;
        std::memcpy(&offset, bytes.data() + 32 + i * 4, 4);
        if (offset > bytes.size() - 8) return false;
        if (std::memcmp(bytes.data() + offset, fourcc, 4) == 0) return true;
    }
    return false;
}

}  // namespace

u32 Frontend::device_configuration_flags(const DredConfig& dred) {
    // Measured (a01d dred_on, dred_markers, factory_state): each FORCED_ON
    // setting is its D3D12_DEVICE_FLAGS bit; markers-only adds its bit to
    // auto-breadcrumbs (0x88).
    u32 flags = 0;
    if (forced_on(dred.auto_breadcrumbs)) {
        flags |= D3D12_DEVICE_FLAG_DRED_AUTO_BREADCRUMBS_ENABLED;
        if (dred.markers_only) {
            flags |= D3D12_DEVICE_FLAG_DRED_USE_MARKERS_ONLY_BREADCRUMBS;
        }
    }
    if (forced_on(dred.page_fault)) {
        flags |= D3D12_DEVICE_FLAG_DRED_PAGE_FAULT_REPORTING_ENABLED;
    }
    if (forced_on(dred.watson)) {
        flags |= D3D12_DEVICE_FLAG_DRED_WATSON_REPORTING_ENABLED;
    }
    if (forced_on(dred.breadcrumb_context)) {
        flags |= D3D12_DEVICE_FLAG_DRED_BREADCRUMB_CONTEXT_ENABLED;
    }
    return flags;
}

// =============================================================================
// D3D12GetInterface / ID3D12DeviceFactory::GetConfigurationInterface
// =============================================================================

u32 Frontend::get_configuration_interface(const GuidBytes& clsid,
                                          GuestAddr iid_address, GuestAddr out,
                                          DeviceFactory* factory) {
    // Measured (a01 misc, a01c factory): a refused pair leaves the output
    // untouched; each successful call makes a new object ("second
    // GetInterface: same object: 0") holding one reference.
    if (guid_equal(clsid, CLSID_D3D12DEBUG)) {
        // The debug layer (Graphics Tools) is not installed: as
        // D3D12GetDebugInterface answers, whatever the IID.
        return DXGI_ERROR_SDK_COMPONENT_MISSING;
    }
    if (out == 0) return E_POINTER;
    std::unique_ptr<Object> object;
    if (guid_equal(clsid, CLSID_D3D12DEVICEREMOVEDEXTENDEDDATA)) {
        auto settings = std::make_unique<DredSettings>();
        settings->factory = factory != nullptr ? factory->address : 0;
        if (factory != nullptr) add_ref(*factory);
        object = std::move(settings);
    } else if (factory == nullptr &&
               guid_equal(clsid, CLSID_D3D12SDKCONFIGURATION)) {
        object = std::make_unique<SdkConfiguration>();
    } else if (factory == nullptr &&
               guid_equal(clsid, CLSID_D3D12DEVICEFACTORY)) {
        object = std::make_unique<DeviceFactory>();
    } else {
        // CLSID_D3D12Tools and CLSID_D3D12DSRDeviceFactory are refused on
        // WARP and NVIDIA too; a factory refuses SDKConfiguration and
        // DeviceFactory (a01c GetConfigurationInterface); any other CLSID is
        // unknown.
        return E_NOINTERFACE;
    }
    return return_new_object(std::move(object), iid_address, out);
}

DredConfig& Frontend::dred_config_of(const DredSettings& settings) {
    if (settings.factory != 0) {
        if (auto* factory =
                find<DeviceFactory>(settings.factory, Kind::DeviceFactory)) {
            return factory->dred;
        }
    }
    return m_dred;
}

void Frontend::sdk_configuration_method(Marshaler& m, SdkConfiguration& config,
                                        usize slot) {
    (void)config;
    switch (slot) {
    case SdkConfigurationSlot::SetSDKVersion:
        // Measured (a01c sdkconfig): every version and path, before or after
        // a device exists, is DXGI_ERROR_INVALID_CALL -- the process's SDK
        // version is fixed by the exports an executable carries
        // (D3D12SDKVersion), never by this call once d3d12.dll is loaded.
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    case SdkConfigurationSlot::CreateDeviceFactory: {
        // CreateDeviceFactory(UINT SDKVersion, LPCSTR SDKPath, REFIID riid,
        //     void **ppvFactory). Measured (a01c, a01d factory_state): any
        //     path -- one that does not exist included -- serves the
        //     system runtime; a version above it is
        //     D3D12_ERROR_INVALID_REDIST; the output is NULL on failure.
        const u32 version = m.arg32(1);
        const GuestAddr path = m.arg_ptr(2);
        const GuestAddr out = m.arg_ptr(4);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        (void)write_pointer(out, 0);
        if (path == 0) {
            // Windows dereferences it (an access violation in d3d12.dll).
            m.set_return(E_INVALIDARG);
            return;
        }
        if (version > kSdkVersion) {
            m.set_return(kD3D12ErrorInvalidRedist);
            return;
        }
        m.set_return(return_new_object(std::make_unique<DeviceFactory>(),
                                       m.arg_ptr(3), out));
        return;
    }
    case SdkConfigurationSlot::FreeUnusedSDKs:
        // void: one runtime, nothing to unload.
        m.set_return(0);
        return;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

void Frontend::device_factory_method(Marshaler& m, DeviceFactory& factory,
                                     usize slot) {
    switch (slot) {
    case DeviceFactorySlot::InitializeFromGlobalState:
        // Measured (a01d factory_state): the factory takes the global DRED
        // settings ...
        factory.dred = m_dred;
        m.set_return(S_OK);
        return;
    case DeviceFactorySlot::ApplyToGlobalState:
        // ... and gives its own to D3D12CreateDevice's later devices.
        m_dred = factory.dred;
        m.set_return(S_OK);
        return;
    case DeviceFactorySlot::SetFlags: {
        // Measured (a01c factory): 0..7 are taken, anything else is
        // E_INVALIDARG and keeps the flags.
        const u32 flags = m.arg32(1);
        if ((flags & ~kFactoryFlagMask) != 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        factory.flags = flags;
        m.set_return(S_OK);
        return;
    }
    case DeviceFactorySlot::GetFlags:
        m.set_return(factory.flags);
        return;
    case DeviceFactorySlot::GetConfigurationInterface: {
        GuidBytes clsid{};
        if (!read_guid(m.arg_ptr(1), clsid)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(get_configuration_interface(clsid, m.arg_ptr(2),
                                                 m.arg_ptr(3), &factory));
        return;
    }
    case DeviceFactorySlot::EnableExperimentalFeatures:
        // As D3D12EnableExperimentalFeatures: none is recognised (a01c
        // factory: 0 is S_OK, D3D12ExperimentalShaderModels E_NOINTERFACE).
        m.set_return(m.arg32(1) == 0 ? S_OK : E_NOINTERFACE);
        return;
    case DeviceFactorySlot::CreateDevice:
        m.set_return(create_device(m.arg_ptr(1), m.arg32(2), m.arg_ptr(3),
                                   m.arg_ptr(4), &factory));
        return;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

void Frontend::dred_settings_method(Marshaler& m, DredSettings& settings,
                                    usize slot) {
    // void setters: the value is kept as given; only FORCED_ON enables
    // (an out-of-range value returns too, a01c dred "setters: returned").
    DredConfig& dred = dred_config_of(settings);
    const u32 value = m.arg32(1);
    switch (slot) {
    case DredSettingsSlot::SetAutoBreadcrumbsEnablement:
        dred.auto_breadcrumbs = value;
        break;
    case DredSettingsSlot::SetPageFaultEnablement:
        dred.page_fault = value;
        break;
    case DredSettingsSlot::SetWatsonDumpEnablement:
        dred.watson = value;
        break;
    case DredSettingsSlot::SetBreadcrumbContextEnablement:
        dred.breadcrumb_context = value;
        break;
    case DredSettingsSlot::UseMarkersOnlyAutoBreadcrumbs:
        dred.markers_only = value != 0;
        break;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
    m.set_return(0);
}

// =============================================================================
// ID3D12DeviceRemovedExtendedData2 (a device tear-off)
// =============================================================================

void Frontend::dred_method(Marshaler& m, Device& device, usize slot) {
    // Measured on WARP and NVIDIA (a01d dred_*): an output whose DRED
    // feature the device was not created with is DXGI_ERROR_UNSUPPORTED; a
    // live device's is DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; once removed,
    // S_OK. A removal the runtime or the application causes (RemoveDevice,
    // SetStablePowerState outside developer mode) reports no breadcrumb node
    // and no page fault, with the device state UNKNOWN, even for a list
    // still waiting on the queue: nodes and faults come from GPU faults and
    // hangs (TDR), and every removal Vitreo raises is a runtime one.
    const GuestAddr out = m.arg_ptr(1);
    const bool removed = device.removed_reason != S_OK;
    const auto answer = [&](u32 feature_bit) -> u32 {
        if ((device.configuration_flags & feature_bit) == 0) {
            return DXGI_ERROR_UNSUPPORTED;
        }
        if (!removed) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        return out != 0 ? S_OK : E_INVALIDARG;
    };
    switch (slot) {
    case DredSlot::GetAutoBreadcrumbsOutput:
    case DredSlot::GetAutoBreadcrumbsOutput1: {
        const u32 hr = answer(D3D12_DEVICE_FLAG_DRED_AUTO_BREADCRUMBS_ENABLED);
        if (hr == S_OK) {
            // { const NODE *pHeadAutoBreadcrumbNode; }: the empty list.
            const auto& l =
                layout::D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1[m_is_64bit];
            (void)write_pointer(out + l.pHeadAutoBreadcrumbNode, 0);
        }
        m.set_return(hr);
        return;
    }
    case DredSlot::GetPageFaultAllocationOutput:
    case DredSlot::GetPageFaultAllocationOutput1:
    case DredSlot::GetPageFaultAllocationOutput2: {
        const u32 hr =
            answer(D3D12_DEVICE_FLAG_DRED_PAGE_FAULT_REPORTING_ENABLED);
        if (hr == S_OK) {
            // No fault: VA 0 and both allocation lists empty (a01d:
            // va=0x0 existing=0 freed=0 flags=0).
            const auto& l = layout::D3D12_DRED_PAGE_FAULT_OUTPUT2[m_is_64bit];
            (void)m_memory.write_u64(out + l.PageFaultVA, 0);
            (void)write_pointer(out + l.pHeadExistingAllocationNode, 0);
            (void)write_pointer(out + l.pHeadRecentFreedAllocationNode, 0);
            if (slot == DredSlot::GetPageFaultAllocationOutput2) {
                (void)m_memory.write_u32(out + l.PageFaultFlags,
                                         0 /* D3D12_DRED_PAGE_FAULT_FLAGS_NONE */);
            }
        }
        m.set_return(hr);
        return;
    }
    case DredSlot::GetDeviceState:
        m.set_return(D3D12_DRED_DEVICE_STATE_UNKNOWN);
        return;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12DeviceTools1, ID3D12PageableTools
// =============================================================================

void Frontend::device_tools_method(Marshaler& m, Device& device, usize slot) {
    switch (slot) {
    case DeviceToolsSlot::SetNextAllocationAddress: {
        // void (D3D12_GPU_VIRTUAL_ADDRESS): a tools-replay hint. Measured
        // (a01d tools): once set non-zero, every committed buffer created is
        // E_INVALIDARG -- a freed VA, a live one, an unused one 1 GiB away
        // -- until it is set back to 0; heaps are unaffected.
        usize next = 0;
        device.next_allocation_va = read_arg_u64(m, m_is_64bit, 1, next);
        m.set_return(0);
        return;
    }
    case DeviceToolsSlot::GetApplicationSpecificDriverState:
        // Measured (a01d tools): E_FAIL, *ppBlob untouched -- no driver
        // state blob was given to this process.
        m.set_return(E_FAIL);
        return;
    case DeviceToolsSlot::GetApplicationSpecificDriverBlobStatus:
        m.set_return(D3D12_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN);
        return;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

void Frontend::pageable_tools_method(Marshaler& m, Object& owner, usize slot) {
    (void)owner;
    if (slot == PageableToolsSlot::GetAllocation) {
        // Measured on WARP and NVIDIA (a01d tools): E_NOTIMPL for heaps,
        // committed and placed buffers and textures, the range untouched.
        m.set_return(E_NOTIMPL);
        return;
    }
    m.set_return(E_NOTIMPL);
}

// =============================================================================
// ID3D12DeviceConfiguration1 (device and factory tear-off)
// =============================================================================

void Frontend::device_configuration_method(Marshaler& m, Object& owner,
                                           usize slot) {
    switch (slot) {
    case DeviceConfigurationSlot::GetDesc: {
        // Hidden return pointer (This, RetVal), returned (a01d:
        // ret=RetVal). No debug layer or GPU-based validation, no
        // experimental feature, SDK 616.
        const GuestAddr out = m.arg_ptr(1);
        u32 flags = 0;
        if (owner.kind == Kind::Device) {
            flags = static_cast<Device&>(owner).configuration_flags;
        } else if (owner.kind == Kind::DeviceFactory) {
            flags = device_configuration_flags(
                static_cast<DeviceFactory&>(owner).dred);
        }
        if (out != 0) {
            const auto& l = layout::D3D12_DEVICE_CONFIGURATION_DESC[m_is_64bit];
            (void)m_memory.write_u32(out + l.Flags, flags);
            (void)m_memory.write_u32(out + l.GpuBasedValidationFlags, 0);
            (void)m_memory.write_u32(out + l.SDKVersion, kSdkVersion);
            (void)m_memory.write_u32(out + l.NumEnabledExperimentalFeatures, 0);
        }
        m.set_return(out);
        return;
    }
    case DeviceConfigurationSlot::GetEnabledExperimentalFeatures:
        // (GUID *pGuids, UINT NumGuids): none is enabled, so nothing is
        // written (a01d: S_OK, the buffer untouched).
        m.set_return(S_OK);
        return;
    case DeviceConfigurationSlot::SerializeVersionedRootSignature:
        // The same bytes as D3D12SerializeVersionedRootSignature (a01d
        // config: same bytes 1, version 9 E_INVALIDARG with no blobs).
        if (m_serialize_versioned_root_signature) {
            m_serialize_versioned_root_signature(m);
        } else {
            m.set_return(E_FAIL);
        }
        return;
    case DeviceConfigurationSlot::CreateVersionedRootSignatureDeserializer:
        if (m_create_versioned_deserializer) {
            m_create_versioned_deserializer(m);
        } else {
            m.set_return(E_FAIL);
        }
        return;
    case DeviceConfigurationSlot::
        CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary: {
        // (pLibraryBlob, Size, RootSignatureSubobjectName, riid, ppv).
        // Measured (a01d config): a blob that is not a DXIL library (a
        // serialized root signature) is E_INVALIDARG with *ppv NULL.
        const GuestAddr blob = m.arg_ptr(1);
        const u64 size = m.arg(2);
        const GuestAddr out = m.arg_ptr(5);
        if (out != 0) (void)write_pointer(out, 0);
        std::vector<u8> bytes;
        if (blob != 0 && size >= 32 && size <= (64ull << 20)) {
            bytes.resize(static_cast<usize>(size));
            if (m_memory.read_bytes(blob, reinterpret_cast<Byte*>(bytes.data()),
                                    bytes.size())
                    .is_err()) {
                bytes.clear();
            }
        }
        if (container_has_part(bytes, "RDAT")) {
            // A DXIL library's runtime data part holds its subobjects; this
            // frontend has no reader for it.
            unimplemented(
                "ID3D12DeviceConfiguration1::"
                "CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary",
                "root signature subobjects of DXIL libraries (RDAT)");
        }
        m.set_return(E_INVALIDARG);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12CompatibilityDevice
// =============================================================================

void Frontend::compatibility_device_method(Marshaler& m, Device& device,
                                           usize slot) {
    switch (slot) {
    case CompatibilityDeviceSlot::CreateSharedResource:
        create_committed_resource(m, device, 4);
        return;
    case CompatibilityDeviceSlot::CreateSharedHeap:
        create_heap(m, device, 2);
        return;
    case CompatibilityDeviceSlot::ReflectSharedProperties: {
        // ReflectSharedProperties(ID3D12Object *pHeapOrResource,
        //     D3D12_REFLECT_SHARED_PROPERTY ReflectType, void *pData,
        //     UINT DataSize). Measured (a01d compat).
        const GuestAddr target = m.arg_ptr(1);
        const u32 type = m.arg32(2);
        const GuestAddr data = m.arg_ptr(3);
        const u32 size = m.arg32(4);
        Heap* heap = find<Heap>(target, Kind::Heap);
        Resource* resource = find<Resource>(target, Kind::Resource);
        const bool shared =
            heap != nullptr
                ? (heap->flags & D3D12_HEAP_FLAG_SHARED) != 0
                : resource != nullptr &&
                      (resource->heap_flags & D3D12_HEAP_FLAG_SHARED) != 0;
        // A heap or resource that is not shared: E_INVALIDARG.
        if (!shared || data == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const u32 compat = heap != nullptr ? heap->compatibility_flags
                                           : resource->compatibility_flags;
        switch (type) {
        case D3D12_REFELCT_SHARED_PROPERTY_COMPATIBILITY_SHARED_FLAGS:
            // The flags it was created with (0 for CreateHeap's SHARED heap).
            if (size != 4) {
                m.set_return(E_INVALIDARG);
                return;
            }
            (void)m_memory.write_u32(data, compat);
            m.set_return(S_OK);
            return;
        case D3D12_REFLECT_SHARED_PROPERTY_D3D11_RESOURCE_FLAGS:
            // D3D11_RESOURCE_FLAGS {Bind, Misc, CPUAccess, Stride}: a heap
            // has none (E_INVALIDARG); a resource made without them reads
            // back zeros and E_FAIL. CreateSharedResource refuses
            // pFlags11, so no resource here has them.
            if (heap != nullptr || size != 16) {
                m.set_return(E_INVALIDARG);
                return;
            }
            for (u32 i = 0; i < 16; i += 4) (void)m_memory.write_u32(data + i, 0);
            m.set_return(E_FAIL);
            return;
        case D3D12_REFLECT_SHARED_PROPERTY_NON_NT_SHARED_HANDLE:
            // Only an object created NON_NT_HANDLE has one, and those are
            // refused at creation; every other object is E_INVALIDARG, the
            // handle untouched.
            m.set_return(E_INVALIDARG);
            return;
        default:
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

/// The D3D12_COMPATIBILITY_SHARED_FLAGS rules CreateSharedHeap and
/// CreateSharedResource share. S_OK, or the failure with its reason logged.
u32 compatibility_flags_error(u32 flags, u32 heap_flags, const char* api) {
    // Measured (a01d compat): a flag outside the three, or a heap that is not
    // SHARED, is E_INVALIDARG.
    if ((flags & ~kCompatibilityFlagMask) != 0 ||
        (heap_flags & D3D12_HEAP_FLAG_SHARED) == 0) {
        return E_INVALIDARG;
    }
    // NON_NT_HANDLE makes a global (D3DKMT) handle a D3D9/D3D11 device opens;
    // KEYED_MUTEX gives the D3D11 side an IDXGIKeyedMutex. Windows takes both
    // (a01d). Neither has a consumer here: D3D11's OpenSharedResource(1)
    // opens backend shared storage by its global id (d3d11_sharing.cpp,
    // metal_shared_resource.hpp), and a D3D12 shared heap or resource is
    // this frontend's own memory with no such id (kD3D12SharedObject,
    // d3d12_sharing.cpp), so a D3D11 device cannot open it by any handle.
    // Both flags need D3D12 shared objects placed in that shared storage
    // first; until then they are refused at the call.
    if ((flags & D3D12_COMPATIBILITY_SHARED_FLAG_NON_NT_HANDLE) != 0) {
        unimplemented(api, "D3D12_COMPATIBILITY_SHARED_FLAG_NON_NT_HANDLE "
                           "(global shared handles)");
        return E_NOTIMPL;
    }
    if ((flags & D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX) != 0) {
        unimplemented(api, "D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX "
                           "(D3D11 keyed mutex interop)");
        return E_NOTIMPL;
    }
    return S_OK;
}

}  // namespace vitreo::winapi::d3d12
