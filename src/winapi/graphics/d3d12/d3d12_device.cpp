// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_device.cpp
/// @brief ID3D12Device (through ID3D12Device14): the dispatch of every slot,
/// and the small objects created here (query heaps, command signatures).
///
/// Each slot is implemented or fails the way Learn documents, with a one-time
/// UNIMPLEMENTED line. Capability and layout queries (CheckFeatureSupport,
/// GetResourceAllocationInfo*, GetCopyableFootprints*, GetResourceTiling) are
/// the caps/layout pass's (d3d12_caps.*, d3d12_layout.*).

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>

namespace vitreo::winapi::d3d12 {
namespace {

/// The device slots that create or open an object and return an HRESULT,
/// with the out pointer as their last parameter (d3d12.h); the view
/// creators return void and are not among them.
[[nodiscard]] bool creator_slot(usize slot) {
    if (slot >= DEVICE_METHOD_COUNT) return false;
    const std::string_view name = DEVICE_METHOD_NAMES[slot];
    if (name.ends_with("View") || name == "CreateSampler" ||
        name == "CreateSampler2") {
        return false;
    }
    return name.starts_with("Create") || name.starts_with("Open") ||
           name == "AddToStateObject";
}

/// UINT64 parameters of a creator (one x64 slot, two x86 slots).
[[nodiscard]] usize creator_u64_args(usize slot) {
    switch (slot) {
    case DeviceSlot::CreatePlacedResource:
    case DeviceSlot::CreatePlacedResource1:
    case DeviceSlot::CreatePlacedResource2:
    case DeviceSlot::CreateFence:
        return 1;
    default:
        return 0;
    }
}

}  // namespace

bool Frontend::device_removed() const {
    const auto it = m_objects.find(m_device);
    return it != m_objects.end() && it->second->kind == Kind::Device &&
           static_cast<const Device&>(*it->second).removed_reason != S_OK;
}

void Frontend::remove_device(u32 reason, const char* why) {
    Device* device = find<Device>(m_device, Kind::Device);
    if (device == nullptr || device->removed_reason != S_OK) return;
    device->removed_reason = reason;
    std::fprintf(stderr, "[vitreo] d3d12: device removed (0x%08x): %s\n",
                 reason, why);
    // Learn (ID3D12Fence::GetCompletedValue): "If the device has been
    // removed, the return value will be UINT64_MAX" -- and every event
    // waiting on a fence is set.
    for (auto& [address, object] : m_objects) {
        if (object->kind != Kind::Fence) continue;
        auto& fence = static_cast<Fence&>(*object);
        if (fence.backend) (void)fence.backend->signal_cpu(~0ull);
    }
}

void Frontend::device_method(Marshaler& m, Device& device, usize slot) {
    if (device.removed_reason != S_OK && creator_slot(slot)) {
        // Measured (a03 heaps after a removal): every create answers
        // DXGI_ERROR_DEVICE_REMOVED with a null out pointer, before any
        // argument validation.
        const usize slots =
            DEVICE_STDCALL_BYTES[slot] / 4 - (m_is_64bit ? creator_u64_args(slot) : 0);
        const GuestAddr out = m.arg_ptr(slots - 1);
        if (out != 0) (void)write_pointer(out, 0);
        m.set_return(DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    switch (slot) {
    case DeviceSlot::GetNodeCount:
        // One adapter, one node.
        m.set_return(1);
        return;
    case DeviceSlot::CreateCommandQueue:
        create_command_queue(m, device);
        return;
    case DeviceSlot::CreateCommandAllocator:
        create_command_allocator(m, device);
        return;
    case DeviceSlot::CreateGraphicsPipelineState:
        create_graphics_pipeline_state(m, device);
        return;
    case DeviceSlot::CreateComputePipelineState:
        create_compute_pipeline_state(m, device);
        return;
    case DeviceSlot::CreateCommandList:
        create_command_list(m, device, false);
        return;
    case DeviceSlot::CheckFeatureSupport:
        // Every D3D12_FEATURE, from what this device implements (caps()).
        marshal_check_feature_support(m, m_memory, caps());
        return;
    case DeviceSlot::CreateDescriptorHeap:
        create_descriptor_heap(m, device);
        return;
    case DeviceSlot::GetDescriptorHandleIncrementSize: {
        // One record stride for every heap type (PLAN decision 5). Any value
        // is conforming -- WARP's differ by type and bitness
        // (d12core_facts_probe increment_*) -- because a guest must step
        // handles by what this returns.
        const u32 heap_type = m.arg32(1);
        m.set_return(heap_type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES
                         ? descriptor_increment()
                         : 0u);
        return;
    }
    case DeviceSlot::CreateRootSignature:
        create_root_signature(m, device);
        return;
    case DeviceSlot::CreateConstantBufferView:
        create_view(m, DescriptorType::Cbv);
        return;
    case DeviceSlot::CreateShaderResourceView:
        create_view(m, DescriptorType::Srv);
        return;
    case DeviceSlot::CreateUnorderedAccessView:
        create_view(m, DescriptorType::Uav);
        return;
    case DeviceSlot::CreateRenderTargetView:
        create_view(m, DescriptorType::Rtv);
        return;
    case DeviceSlot::CreateDepthStencilView:
        create_view(m, DescriptorType::Dsv);
        return;
    case DeviceSlot::CreateSampler:
        create_sampler(m, false);
        return;
    case DeviceSlot::CopyDescriptors:
        copy_descriptors(m);
        return;
    case DeviceSlot::CopyDescriptorsSimple:
        copy_descriptors_simple(m);
        return;
    case DeviceSlot::GetResourceAllocationInfo:
        marshal_get_resource_allocation_info(m, m_memory, layout(), m_is_64bit,
                                             0);
        return;
    case DeviceSlot::GetResourceAllocationInfo1:
        marshal_get_resource_allocation_info(m, m_memory, layout(), m_is_64bit,
                                             1);
        return;
    case DeviceSlot::GetResourceAllocationInfo2:
        marshal_get_resource_allocation_info(m, m_memory, layout(), m_is_64bit,
                                             2);
        return;
    case DeviceSlot::GetResourceAllocationInfo3:
        marshal_get_resource_allocation_info(m, m_memory, layout(), m_is_64bit,
                                             3);
        return;
    case DeviceSlot::GetCustomHeapProperties: {
        // Hidden return pointer: This, RetVal, nodeMask, heapType.
        const GuestAddr out = m.arg_ptr(1);
        const u32 heap_type = m.arg32(3);
        HeapProperties props{};
        // Learn (GetCustomHeapProperties), UMA + CacheCoherentUMA -- what
        // this Apple GPU is, and what the caps pass reports through
        // D3D12_FEATURE_ARCHITECTURE: UPLOAD and READBACK are WRITE_BACK
        // in L0, DEFAULT is NOT_AVAILABLE in L0; GPU_UPLOAD is
        // WRITE_COMBINE in L0 (resolve_custom). Node masks read back as 1
        // (measured, d12core_facts_probe custom_heap).
        switch (heap_type) {
        case D3D12_HEAP_TYPE_DEFAULT:
        case D3D12_HEAP_TYPE_UPLOAD:
        case D3D12_HEAP_TYPE_READBACK:
        case D3D12_HEAP_TYPE_GPU_UPLOAD: {
            HeapProperties in{};
            in.type = heap_type;
            props = resolve_custom(in);
            props.type = D3D12_HEAP_TYPE_CUSTOM;
            break;
        }
        default:
            // Learn: "D3D12_HEAP_TYPE_CUSTOM is not supported as a parameter
            // value." Measured (a02 desc "GetCustomHeapProperties(CUSTOM)"):
            // an all-zero struct but for the node masks.
            break;
        }
        props.creation_node_mask = 1;
        props.visible_node_mask = 1;
        if (out != 0) write_heap_properties(out, props);
        m.set_return(out);
        return;
    }
    case DeviceSlot::CreateCommittedResource:
        create_committed_resource(m, device, 0);
        return;
    case DeviceSlot::CreateHeap:
        create_heap(m, device, false);
        return;
    case DeviceSlot::CreatePlacedResource:
        create_placed_resource(m, device, 0);
        return;
    case DeviceSlot::CreateReservedResource:
        create_reserved_resource(m, device, 0);
        return;
    case DeviceSlot::CreateSharedHandle: {
        // CreateSharedHandle(ID3D12DeviceChild *pObject, const
        //     SECURITY_ATTRIBUTES *, DWORD Access, LPCWSTR Name,
        //     HANDLE *pHandle). Learn: the object must have been created
        //     shareable -- a heap or committed resource with
        //     D3D12_HEAP_FLAG_SHARED, a fence with D3D12_FENCE_FLAG_SHARED;
        //     anything else is E_INVALIDARG (measured, a01 misc
        //     "CreateSharedHandle(non-shared heap)").
        const GuestAddr object = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(5);
        if (out != 0) (void)write_pointer(out, 0);
        bool shareable = false;
        if (const Heap* heap = find<Heap>(object, Kind::Heap)) {
            shareable = (heap->flags & D3D12_HEAP_FLAG_SHARED) != 0;
        } else if (const Resource* resource =
                       find<Resource>(object, Kind::Resource)) {
            shareable = resource->committed &&
                        (resource->heap_flags & D3D12_HEAP_FLAG_SHARED) != 0;
        } else if (const Fence* fence = find<Fence>(object, Kind::Fence)) {
            shareable = (fence->flags & D3D12_FENCE_FLAG_SHARED) != 0;
        }
        if (!shareable || out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        unimplemented("ID3D12Device::CreateSharedHandle",
                      "NT handles for shared heaps / resources / fences");
        m.set_return(E_NOTIMPL);
        return;
    }
    case DeviceSlot::OpenSharedHandle: {
        // OpenSharedHandle(HANDLE NTHandle, REFIID riid, void **ppvObj). No
        // D3D12 shared handle is ever created here, so a handle is not one
        // this device can open (measured for a bogus handle, a01 misc:
        // E_HANDLE); handles from other APIs or processes are the OPEN part.
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device::OpenSharedHandle",
                      "handles shared by another device, API or process");
        m.set_return(E_HANDLE);
        return;
    }
    case DeviceSlot::OpenSharedHandleByName: {
        // OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE *): no
        // named D3D12 object exists -- as a failed OpenEvent/OpenMutex
        // reports, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND).
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device::OpenSharedHandleByName",
                      "named shared objects");
        m.set_return(0x80070002u);
        return;
    }
    case DeviceSlot::MakeResident:
    case DeviceSlot::Evict: {
        // MakeResident / Evict(UINT NumObjects, ID3D12Pageable *const *).
        // Metal keeps every allocation resident; eviction is the OS's, so
        // both are complete once the objects are known to be pageables.
        const u32 count = m.arg32(1);
        const GuestAddr array = m.arg_ptr(2);
        if (count != 0 && array == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const usize pointer_size = m_is_64bit ? 8u : 4u;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr pageable = 0;
            if (!read_pointer(array + index * pointer_size, pageable) ||
                m_objects.find(pageable) == m_objects.end()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        m.set_return(S_OK);
        return;
    }
    case DeviceSlot::CreateFence:
        create_fence(m, device);
        return;
    case DeviceSlot::GetDeviceRemovedReason:
        m.set_return(device.removed_reason);
        return;
    case DeviceSlot::GetCopyableFootprints:
        marshal_get_copyable_footprints(m, m_memory, layout(), m_is_64bit,
                                        false);
        return;
    case DeviceSlot::GetCopyableFootprints1:
        marshal_get_copyable_footprints(m, m_memory, layout(), m_is_64bit,
                                        true);
        return;
    case DeviceSlot::CreateQueryHeap:
        create_query_heap(m, device);
        return;
    case DeviceSlot::SetStablePowerState:
        // Learn: "Calling this method when not in Developer Mode results in
        // the device being removed." There is no developer mode (nor GPU
        // clock control) here; measured on WARP outside developer mode
        // (a01 stablepower): E_FAIL and removal with DXGI_ERROR_INVALID_CALL.
        remove_device(DXGI_ERROR_INVALID_CALL,
                      "SetStablePowerState outside developer mode");
        m.set_return(E_FAIL);
        return;
    case DeviceSlot::CreateCommandSignature:
        create_command_signature(m, device);
        return;
    case DeviceSlot::GetResourceTiling: {
        // GetResourceTiling(pTiledResource, ...): void. The layout pass
        // answers from the resource's description; no reserved resource can
        // exist yet (tiled tier NOT_SUPPORTED), so every resource is
        // answered as a non-tiled one.
        Resource* resource = find<Resource>(m.arg_ptr(1), Kind::Resource);
        ResourceDesc desc{};
        if (resource != nullptr) {
            const ResourceDescription& d = resource->desc;
            desc.dimension = d.dimension;
            desc.alignment = d.alignment;
            desc.width = d.width;
            desc.height = d.height;
            desc.depth_or_array_size = static_cast<u16>(d.depth_or_array_size);
            desc.mip_levels = static_cast<u16>(d.mip_levels);
            desc.format = d.format;
            desc.sample_count = d.sample_count;
            desc.sample_quality = d.sample_quality;
            desc.layout = d.layout;
            desc.flags = d.flags;
        }
        marshal_get_resource_tiling(m, m_memory, desc,
                                    resource != nullptr && resource->reserved,
                                    TiledLayout{});
        return;
    }
    case DeviceSlot::GetAdapterLuid: {
        // Hidden return pointer. The LUID of the DXGI adapter the device is
        // on (measured: GetAdapterLuid equals EnumAdapters1(0)'s
        // DXGI_ADAPTER_DESC1::AdapterLuid, d3d12_a01_device_probe [create]).
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            (void)m_memory.write_u32(out, dxgi::ADAPTER_LUID_LOW);
            (void)m_memory.write_u32(out + 4, dxgi::ADAPTER_LUID_HIGH);
        }
        m.set_return(out);
        return;
    }
    // ---- ID3D12Device1 ---------------------------------------------------
    case DeviceSlot::CreatePipelineLibrary: {
        // CreatePipelineLibrary(const void *pLibraryBlob, SIZE_T BlobLength,
        //     REFIID riid, void **ppPipelineLibrary). Learn: "DXGI_ERROR_
        //     UNSUPPORTED: the driver doesn't support pipeline libraries" --
        //     D3D12_FEATURE_SHADER_CACHE reports no LIBRARY support here.
        const GuestAddr out = m.arg_ptr(4);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device1::CreatePipelineLibrary",
                      "pipeline libraries (SHADER_CACHE_SUPPORT_LIBRARY)");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    case DeviceSlot::SetEventOnMultipleFenceCompletion:
        set_event_on_multiple_fences(m);
        return;
    case DeviceSlot::SetResidencyPriority: {
        // SetResidencyPriority(UINT NumObjects, ID3D12Pageable *const *,
        //     const D3D12_RESIDENCY_PRIORITY *). Every allocation stays
        //     resident on this unified-memory device, so a priority orders
        //     nothing; the objects must still be pageables.
        const u32 count = m.arg32(1);
        const GuestAddr array = m.arg_ptr(2);
        const GuestAddr priorities = m.arg_ptr(3);
        if (count != 0 && (array == 0 || priorities == 0)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const usize pointer_size = m_is_64bit ? 8u : 4u;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr pageable = 0;
            if (!read_pointer(array + index * pointer_size, pageable) ||
                m_objects.find(pageable) == m_objects.end()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        m.set_return(S_OK);
        return;
    }
    // ---- ID3D12Device2 ---------------------------------------------------
    case DeviceSlot::CreatePipelineState:
        create_pipeline_state(m, device);
        return;
    // ---- ID3D12Device3 ---------------------------------------------------
    case DeviceSlot::OpenExistingHeapFromAddress:
    case DeviceSlot::OpenExistingHeapFromFileMapping:
    case DeviceSlot::OpenExistingHeapFromAddress1: {
        // D3D12_FEATURE_EXISTING_HEAPS reports Supported FALSE: the backend
        // has no heap over memory it did not allocate.
        const usize out_index =
            slot == DeviceSlot::OpenExistingHeapFromAddress1 ? 4 : 3;
        const GuestAddr out = m.arg_ptr(out_index);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented(slot == DeviceSlot::OpenExistingHeapFromFileMapping
                          ? "ID3D12Device3::OpenExistingHeapFromFileMapping"
                          : slot == DeviceSlot::OpenExistingHeapFromAddress
                                ? "ID3D12Device3::OpenExistingHeapFromAddress"
                                : "ID3D12Device13::OpenExistingHeapFromAddress1",
                      "existing heaps (EXISTING_HEAPS Supported FALSE)");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    case DeviceSlot::EnqueueMakeResident: {
        // EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects,
        //     ID3D12Pageable *const *, ID3D12Fence *pFenceToSignal,
        //     UINT64 FenceValueToSignal). Everything is resident, so the
        //     fence is signalled at once (measured, a01 misc: signalled=1
        //     value=7); a flag other than DENY_OVERBUDGET is E_INVALIDARG.
        const u32 flags = m.arg32(1);
        const u32 count = m.arg32(2);
        const GuestAddr array = m.arg_ptr(3);
        Fence* fence = find<Fence>(m.arg_ptr(4), Kind::Fence);
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 5, next);
        if ((flags & ~D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET) != 0 ||
            fence == nullptr || (count != 0 && array == 0)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const usize pointer_size = m_is_64bit ? 8u : 4u;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr pageable = 0;
            if (!read_pointer(array + index * pointer_size, pageable) ||
                m_objects.find(pageable) == m_objects.end()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        if (!cpu_signal(*fence, value)) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        pump_all_queues();
        m.set_return(S_OK);
        return;
    }
    // ---- ID3D12Device4 ---------------------------------------------------
    case DeviceSlot::CreateCommandList1:
        create_command_list(m, device, true);
        return;
    case DeviceSlot::CreateProtectedResourceSession:
    case DeviceSlot::CreateProtectedResourceSession1: {
        // (desc, riid, ppSession). D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_
        // SUPPORT reports NONE: there is no protected content path.
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented(slot == DeviceSlot::CreateProtectedResourceSession
                          ? "ID3D12Device4::CreateProtectedResourceSession"
                          : "ID3D12Device7::CreateProtectedResourceSession1",
                      "protected resource sessions (support NONE)");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    case DeviceSlot::CreateCommittedResource1:
        create_committed_resource(m, device, 1);
        return;
    case DeviceSlot::CreateHeap1:
        create_heap(m, device, true);
        return;
    case DeviceSlot::CreateReservedResource1:
        create_reserved_resource(m, device, 1);
        return;
    // ---- ID3D12Device5 ---------------------------------------------------
    case DeviceSlot::CreateLifetimeTracker: {
        // (ID3D12LifetimeOwner *, riid, ppvTracker): lifetime tracking of
        // protected-session content, which does not exist here.
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device5::CreateLifetimeTracker",
                      "protected content lifetime tracking");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    case DeviceSlot::RemoveDevice:
        // Learn (ID3D12Device5::RemoveDevice): the device is removed as if by
        // a hardware fault; GetDeviceRemovedReason reports
        // DXGI_ERROR_DEVICE_REMOVED and fences read UINT64_MAX.
        remove_device(DXGI_ERROR_DEVICE_REMOVED, "ID3D12Device5::RemoveDevice");
        m.set_return(0);
        return;
    case DeviceSlot::EnumerateMetaCommands: {
        // (UINT *pNumMetaCommands, D3D12_META_COMMAND_DESC *pDescs). The
        // driver exposes no meta commands.
        const GuestAddr count = m.arg_ptr(1);
        if (count == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u32(count, 0);
        m.set_return(S_OK);
        return;
    }
    case DeviceSlot::EnumerateMetaCommandParameters:
    case DeviceSlot::CreateMetaCommand: {
        // No meta command exists, so every CommandId is unknown.
        if (slot == DeviceSlot::CreateMetaCommand) {
            const GuestAddr out = m.arg_ptr(6);
            if (out != 0) (void)write_pointer(out, 0);
        }
        d3d12_diag("%s: unknown meta command",
                   slot == DeviceSlot::CreateMetaCommand
                       ? "ID3D12Device5::CreateMetaCommand"
                       : "ID3D12Device5::EnumerateMetaCommandParameters");
        m.set_return(E_INVALIDARG);
        return;
    }
    case DeviceSlot::CreateStateObject:
    case DeviceSlot::AddToStateObject: {
        // State objects hold DXIL libraries (raytracing, work graphs,
        // generic programs): RaytracingTier NOT_SUPPORTED and SM 5.1 here
        // (PLAN decision 1).
        const GuestAddr out =
            m.arg_ptr(slot == DeviceSlot::CreateStateObject ? 3 : 4);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented(slot == DeviceSlot::CreateStateObject
                          ? "ID3D12Device5::CreateStateObject"
                          : "ID3D12Device7::AddToStateObject",
                      "state objects (DXIL libraries)");
        m.set_return(E_INVALIDARG);
        return;
    }
    case DeviceSlot::GetRaytracingAccelerationStructurePrebuildInfo: {
        // void (const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *,
        //     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *):
        //     RaytracingTier NOT_SUPPORTED, so nothing can be built; the
        //     three UINT64 sizes are zero.
        const GuestAddr info = m.arg_ptr(2);
        if (info != 0) {
            for (u32 index = 0; index < 3; ++index) {
                (void)m_memory.write_u64(info + index * 8, 0);
            }
        }
        unimplemented(
            "ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo",
            "raytracing (tier NOT_SUPPORTED)");
        m.set_return(0);
        return;
    }
    case DeviceSlot::CheckDriverMatchingIdentifier:
        // (D3D12_SERIALIZED_DATA_TYPE, const
        // D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *): the one
        // serialized type is a raytracing acceleration structure, which this
        // driver does not support.
        m.set_return(D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE);
        return;
    // ---- ID3D12Device6 ---------------------------------------------------
    case DeviceSlot::SetBackgroundProcessingMode: {
        // SetBackgroundProcessingMode(Mode, MeasurementsAction,
        //     HANDLE hEventToSignalUponCompletion,
        //     BOOL *pbFurtherMeasurementsDesired). This driver does no
        //     background shader optimisation: there is nothing to wait for,
        //     so the event is set at once and no measurements are wanted.
        const u32 mode = m.arg32(1);
        const u32 action = m.arg32(2);
        const GuestAddr event = m.arg_ptr(3);
        const GuestAddr further = m.arg_ptr(4);
        if (mode > D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM ||
            action > D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (further != 0) (void)m_memory.write_u32(further, 0);
        if (event != 0) GpuEventSignaler::instance().signal_now(event);
        m.set_return(S_OK);
        return;
    }
    // ---- ID3D12Device8 ---------------------------------------------------
    case DeviceSlot::CreateCommittedResource2:
        create_committed_resource(m, device, 2);
        return;
    case DeviceSlot::CreatePlacedResource1:
        create_placed_resource(m, device, 1);
        return;
    case DeviceSlot::CreateSamplerFeedbackUnorderedAccessView:
        // void (pTargetedResource, pFeedbackResource, DestDescriptor):
        // SamplerFeedbackTier NOT_SUPPORTED, so no feedback resource exists.
        unimplemented("ID3D12Device8::CreateSamplerFeedbackUnorderedAccessView",
                      "sampler feedback (tier NOT_SUPPORTED)");
        m.set_return(0);
        return;
    // ---- ID3D12Device9 ---------------------------------------------------
    case DeviceSlot::CreateShaderCacheSession: {
        // (const D3D12_SHADER_CACHE_SESSION_DESC *, riid, ppvSession).
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device9::CreateShaderCacheSession",
                      "application shader cache sessions");
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    case DeviceSlot::ShaderCacheControl:
        // ShaderCacheControl(Kinds, Control): Learn -- "requires Developer
        // Mode"; there is none here, and no driver cache to control.
        d3d12_diag("ID3D12Device9::ShaderCacheControl outside developer mode");
        m.set_return(E_FAIL);
        return;
    case DeviceSlot::CreateCommandQueue1:
        create_command_queue(m, device, true);
        return;
    // ---- ID3D12Device10 --------------------------------------------------
    case DeviceSlot::CreateCommittedResource3:
        create_committed_resource(m, device, 3);
        return;
    case DeviceSlot::CreatePlacedResource2:
        create_placed_resource(m, device, 2);
        return;
    case DeviceSlot::CreateReservedResource2:
        create_reserved_resource(m, device, 2);
        return;
    // ---- ID3D12Device11 --------------------------------------------------
    case DeviceSlot::CreateSampler2:
        create_sampler(m, true);
        return;
    // ---- ID3D12Device14 --------------------------------------------------
    case DeviceSlot::CreateRootSignatureFromSubobjectInLibrary: {
        // (nodeMask, pLibraryBlob, blobLength, subobjectName, riid, ppv): a
        // DXIL library (PLAN decision 1: no DXIL here).
        const GuestAddr out = m.arg_ptr(6);
        if (out != 0) (void)write_pointer(out, 0);
        unimplemented("ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary",
                      "DXIL libraries");
        m.set_return(E_INVALIDARG);
        return;
    }
    default:
        break;
    }
    // Every ID3D12Device..Device14 slot has a case above; the IUnknown and
    // ID3D12Object ones are d3d12_objects.cpp's and never reach here.
    const char* name = slot < DEVICE_METHOD_COUNT ? DEVICE_METHOD_NAMES[slot]
                                                  : "<slot>";
    std::string api = std::string("ID3D12Device::") + name;
    unimplemented(api.c_str(), "not implemented");
    m.set_return(E_NOTIMPL);
}

// =============================================================================
// Capabilities
// =============================================================================

const CapsContext& Frontend::caps() {
    if (!m_caps) {
        auto context = std::make_unique<CapsContext>(
            backend_caps_context(m_backend, m_is_64bit));
        // Only what this frontend serves. One ID3D12Heap holds buffers and
        // every texture kind (a Metal placement heap).
        context->resource_heap_tier = 2;
        // HIGH-priority queues are accepted; every queue shares the one
        // backend stream, so priority is the scheduling hint it is on
        // Windows and nothing more.
        context->high_priority_queues = true;
        context->simd_width = m_backend.simd_group_width();
        // Output-merger logic ops: a pixel-shader variant through framebuffer
        // fetch (d3d12_pipeline.cpp logic_op_variant), as D3D11 reports them.
        context->logic_ops = m_backend.supports_framebuffer_fetch();
        // WriteBufferImmediate is a GPU copy from upload space on any list
        // (d3d12_replay.cpp write_immediate): D3D12_COMMAND_LIST_SUPPORT_FLAG
        // DIRECT | BUNDLE | COMPUTE | COPY (1 << D3D12_COMMAND_LIST_TYPE).
        context->write_buffer_immediate_support =
            (1u << D3D12_COMMAND_LIST_TYPE_DIRECT) |
            (1u << D3D12_COMMAND_LIST_TYPE_BUNDLE) |
            (1u << D3D12_COMMAND_LIST_TYPE_COMPUTE) |
            (1u << D3D12_COMMAND_LIST_TYPE_COPY);
        m_caps = std::move(context);
    }
    return *m_caps;
}

bool Frontend::texture_placement(const ResourceDescription& desc, u64& size,
                                 u64& alignment) {
    const graphics::PixelFormat format =
        texture_format_for(desc.format, desc.flags);
    const bool shader_resource =
        (desc.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    if (desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        graphics::BackendTexture3DDesc query{};
        query.width = static_cast<unsigned>(desc.width);
        query.height = desc.height;
        query.depth = desc.depth_or_array_size;
        query.mip_levels = desc.mip_levels;
        query.pixel_format = format;
        query.shader_resource = shader_resource;
        query.render_target =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        query.unordered_access =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        query.pixel_format_view = texture_is_typeless(desc.format);
        return m_backend.heap_texture_placement(query, size, alignment);
    }
    graphics::BackendTexture2DDesc query{};
    query.width = static_cast<unsigned>(desc.width);
    query.height = desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                       ? 1u
                       : desc.height;
    query.mip_levels = desc.mip_levels;
    query.array_size = desc.depth_or_array_size;
    query.pixel_format = format;
    query.pixel_format_view = texture_is_typeless(desc.format);
    query.shader_resource = shader_resource;
    query.render_target =
        (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    query.depth_stencil =
        (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    query.unordered_access =
        (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    query.sample_count = std::max(1u, desc.sample_count);
    return m_backend.heap_texture_placement(query, size, alignment);
}

const LayoutCaps& Frontend::layout() {
    if (!m_layout) {
        auto rules = std::make_unique<LayoutCaps>(layout_caps(caps()));
        // A texture's allocation covers what the backend needs to place it
        // (heapTextureSizeAndAlignWithDescriptor on Metal), so an offset
        // GetResourceAllocationInfo allows is one CreatePlacedResource
        // accepts.
        rules->texture_heap_requirement = [this](const ResourceDesc& d,
                                                 u32 mip_levels) {
            ResourceDescription desc{};
            desc.dimension = d.dimension;
            desc.width = d.width;
            desc.height = d.height;
            desc.depth_or_array_size = d.depth_or_array_size;
            desc.mip_levels = mip_levels;
            desc.format = d.format;
            desc.sample_count = d.sample_count;
            desc.flags = d.flags;
            TextureHeapRequirement requirement{};
            requirement.supported =
                texture_placement(desc, requirement.size,
                                  requirement.alignment);
            return requirement;
        };
        m_layout = std::move(rules);
    }
    return *m_layout;
}

// =============================================================================
// Query heaps and command signatures
// =============================================================================

void Frontend::create_query_heap(Marshaler& m, Device& device) {
    // CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid,
    //                 void **ppvHeap). ppvHeap may be null (a validation-only
    //                 call answering S_FALSE).
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& layout = layout::D3D12_QUERY_HEAP_DESC[m_is_64bit];
    const u32 type = read_u32(desc + layout.Type);
    const u32 count = read_u32(desc + layout.Count);
    const u32 node_mask = read_u32(desc + layout.NodeMask);
    if (type > D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 || count == 0 ||
        node_mask > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (type == D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS ||
        type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1) {
        unimplemented("ID3D12Device::CreateQueryHeap",
                      "video decode / pipeline statistics 1 heaps");
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        // Measured (a06 query_heap "ppv NULL"): no validation-only S_FALSE.
        m.set_return(E_UNEXPECTED);
        return;
    }
    auto heap = std::make_unique<QueryHeap>();
    heap->device = device.address;
    heap->type = type;
    heap->count = count;
    heap->node_mask = node_mask;
    heap->slots.resize(count);
    m.set_return(return_new_object(std::move(heap), m.arg_ptr(2), out));
}

void Frontend::create_command_signature(Marshaler& m, Device& device) {
    // CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *pDesc,
    //     ID3D12RootSignature *pRootSignature, REFIID riid,
    //     void **ppvCommandSignature).
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr root_address = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(4);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& layout = layout::D3D12_COMMAND_SIGNATURE_DESC[m_is_64bit];
    const auto& arg_layout = layout::D3D12_INDIRECT_ARGUMENT_DESC[m_is_64bit];
    auto signature = std::make_unique<CommandSignature>();
    signature->device = device.address;
    signature->byte_stride = read_u32(desc + layout.ByteStride);
    signature->node_mask = read_u32(desc + layout.NodeMask);
    const u32 count = read_u32(desc + layout.NumArgumentDescs);
    GuestAddr arguments = 0;
    if (!read_pointer(desc + layout.pArgumentDescs, arguments) ||
        arguments == 0 || count == 0 || signature->node_mask > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    RootSignature* root = nullptr;
    if (root_address != 0) {
        root = find<RootSignature>(root_address, Kind::RootSignature);
        if (root == nullptr) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    // Learn (D3D12_COMMAND_SIGNATURE_DESC, "Indirect drawing"): the
    // arguments are packed in order, exactly one draw / dispatch and it
    // last; a signature that changes root arguments needs the root signature
    // they index, one that does not must not name one; ByteStride covers
    // the arguments and is 4-byte aligned. Measured (a06 indirect_sig).
    u32 total = 0;
    bool changes_root = false;
    for (u32 index = 0; index < count; ++index) {
        const GuestAddr at = arguments + index * arg_layout.size;
        CommandSignature::Argument argument{};
        argument.type = read_u32(at + arg_layout.Type);
        // The union's UINTs, whichever member the type selects.
        argument.slot = read_u32(at + arg_layout.VertexBuffer_Slot);
        argument.root_index =
            read_u32(at + arg_layout.Constant_RootParameterIndex);
        argument.dest_offset =
            read_u32(at + arg_layout.Constant_DestOffsetIn32BitValues);
        argument.num_values =
            read_u32(at + arg_layout.Constant_Num32BitValuesToSet);
        const bool action =
            argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
            argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
            argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH ||
            argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS ||
            argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        if (action != (index + 1 == count)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        u32 bytes = 0;
        u32 root_type = ~0u;
        switch (argument.type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: bytes = 16; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: bytes = 20; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: bytes = 12; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            if (argument.slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
                m.set_return(E_INVALIDARG);
                return;
            }
            bytes = 16;  // D3D12_VERTEX_BUFFER_VIEW
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
            bytes = 16;  // D3D12_INDEX_BUFFER_VIEW
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            bytes = argument.num_values * 4u;
            root_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            bytes = 8;
            root_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            bytes = 8;
            root_type = D3D12_ROOT_PARAMETER_TYPE_SRV;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            bytes = 8;
            root_type = D3D12_ROOT_PARAMETER_TYPE_UAV;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
            // RaytracingTier / MeshShaderTier NOT_SUPPORTED (caps pass).
            unimplemented("ID3D12Device::CreateCommandSignature",
                          "DISPATCH_RAYS / DISPATCH_MESH arguments");
            m.set_return(E_INVALIDARG);
            return;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT:
            // D3D12_OPTIONS21 ExecuteIndirectTier 1.1 only; this device
            // reports 1.0.
            m.set_return(E_INVALIDARG);
            return;
        default:
            m.set_return(E_INVALIDARG);
            return;
        }
        if (root_type != ~0u) {
            changes_root = true;
            const RootLayout* layout_of =
                root != nullptr ? root->layout.get() : nullptr;
            if (layout_of == nullptr ||
                argument.root_index >= layout_of->parameters.size()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            const BindingParameter& parameter =
                layout_of->parameters[argument.root_index];
            if (parameter.type != root_type ||
                (root_type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                 (argument.num_values == 0 ||
                  argument.dest_offset > parameter.num_32bit_values ||
                  argument.num_values >
                      parameter.num_32bit_values - argument.dest_offset))) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        total += bytes;
        signature->arguments.push_back(argument);
    }
    if ((root != nullptr) != changes_root ||
        signature->byte_stride < total || signature->byte_stride % 4 != 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    signature->root_signature = root_address;
    signature->root = root != nullptr ? root->layout : nullptr;
    if (out == 0) {
        // Measured (a06 indirect_sig "ppv NULL").
        m.set_return(E_UNEXPECTED);
        return;
    }
    m.set_return(return_new_object(std::move(signature), m.arg_ptr(3), out));
}

}  // namespace vitreo::winapi::d3d12
