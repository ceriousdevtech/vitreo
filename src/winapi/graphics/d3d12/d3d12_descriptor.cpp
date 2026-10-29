// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_descriptor.cpp
/// @brief ID3D12DescriptorHeap, the Create*View / CreateSampler family and
/// CopyDescriptors.
///
/// PLAN decision 5. A descriptor heap is a host array of records; its CPU
/// handles are addresses in a reserved guest range nothing is mapped at (a
/// guest that dereferences one faults at the mistake), and every heap has a
/// parallel range of GPU handles (WARP answers a non-zero GPU start even for a
/// heap that is not shader visible, d12core_facts_probe descriptor_heap).
/// Create*View writes a record; nothing is resolved until a command list that
/// reads it executes.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi::d3d12 {
namespace {

// A default (null-description) view's dimension is spelled with the SRV
// values; the RTV and UAV enums number the shared texture dimensions alike.
static_assert(D3D12_SRV_DIMENSION_TEXTURE1D == D3D12_RTV_DIMENSION_TEXTURE1D &&
              D3D12_SRV_DIMENSION_TEXTURE1D == D3D12_UAV_DIMENSION_TEXTURE1D &&
              D3D12_SRV_DIMENSION_TEXTURE2DARRAY ==
                  D3D12_RTV_DIMENSION_TEXTURE2DARRAY &&
              D3D12_SRV_DIMENSION_TEXTURE2DARRAY ==
                  D3D12_UAV_DIMENSION_TEXTURE2DARRAY &&
              D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY ==
                  D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY &&
              D3D12_SRV_DIMENSION_TEXTURE3D == D3D12_RTV_DIMENSION_TEXTURE3D &&
              D3D12_SRV_DIMENSION_TEXTURE3D == D3D12_UAV_DIMENSION_TEXTURE3D);

constexpr GuestAddr DESCRIPTOR_RANGE_HINT = 0x70000000ull;

[[nodiscard]] u32 heap_type_for(DescriptorType type) {
    switch (type) {
    case DescriptorType::Cbv:
    case DescriptorType::Srv:
    case DescriptorType::Uav:
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    case DescriptorType::Sampler:
        return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    case DescriptorType::Rtv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case DescriptorType::Dsv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    case DescriptorType::None:
        break;
    }
    return D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
}

}  // namespace

void Frontend::create_descriptor_heap(Marshaler& m, Device& device) {
    // CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[m_is_64bit];
    auto heap = std::make_unique<DescriptorHeap>();
    heap->device = device.address;
    heap->type = read_u32(desc + l.Type);
    const u32 count = read_u32(desc + l.NumDescriptors);
    heap->flags = read_u32(desc + l.Flags);
    heap->node_mask = read_u32(desc + l.NodeMask);
    const bool visible =
        (heap->flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
    // Learn (D3D12_DESCRIPTOR_HEAP_DESC): only CBV/SRV/UAV and sampler heaps
    // may be shader visible (measured: an RTV one is E_INVALIDARG,
    // d12core_facts_probe rtv_heap_shader_visible), within the sizes Learn
    // gives for shader-visible heaps (resource binding tier 1/2).
    // Measured (a03 heaps): an empty heap is fine, and GetDesc reports a
    // NodeMask of 0 as 1.
    if (heap->type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ||
        heap->node_mask > 1 ||
        (heap->flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 ||
        (visible && heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
         heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ||
        (visible && heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
         count > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1) ||
        (visible && heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
         count > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (heap->node_mask == 0) heap->node_mask = 1;
    if (out == 0) {
        // Measured (a03 heaps "ppvHeap=NULL"): a validation-only call.
        m.set_return(S_FALSE);
        return;
    }
    const usize bytes =
        (std::max<usize>(1, static_cast<usize>(count) * descriptor_increment()) +
         0xFFFFu) &
        ~usize{0xFFFFu};
    // Reserved, never committed and never accessible: CPU handles are names.
    heap->cpu_base = m_memory.map_first_fit(DESCRIPTOR_RANGE_HINT, bytes,
                                            MemoryProtection::None,
                                            /*committed=*/false, 0x10000u);
    if (heap->cpu_base == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    heap->cpu_reserved = bytes;
    heap->gpu_base = m_next_gpu_handle;
    m_next_gpu_handle += bytes + 0x10000u;
    heap->records.resize(count);
    DescriptorHeap* raw = heap.get();
    const u32 result = return_new_object(std::move(heap), m.arg_ptr(2), out);
    if (result == S_OK) {
        m_cpu_heaps.emplace(raw->cpu_base, raw);
        m_gpu_heaps.emplace(raw->gpu_base, raw);
    }
    m.set_return(result);
}

void Frontend::descriptor_heap_method(Marshaler& m, DescriptorHeap& heap,
                                      usize slot) {
    switch (slot) {
    case DescriptorHeapSlot::GetDesc: {
        // Hidden return pointer (_WIN32 C interface): This, RetVal.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[m_is_64bit];
            (void)m_memory.write_u32(out + l.Type, heap.type);
            (void)m_memory.write_u32(out + l.NumDescriptors,
                                     static_cast<u32>(heap.records.size()));
            (void)m_memory.write_u32(out + l.Flags, heap.flags);
            (void)m_memory.write_u32(out + l.NodeMask, heap.node_mask);
        }
        m.set_return(out);
        return;
    }
    case DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart: {
        // D3D12_CPU_DESCRIPTOR_HANDLE { SIZE_T ptr }, through the hidden
        // return pointer on both bitnesses (SDK C interface, _WIN32 branch).
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) (void)write_pointer(out, heap.cpu_base);
        m.set_return(out);
        return;
    }
    case DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart: {
        // D3D12_GPU_DESCRIPTOR_HANDLE { UINT64 ptr }.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) (void)m_memory.write_u64(out, heap.gpu_base);
        m.set_return(out);
        return;
    }
    default:
        unimplemented("ID3D12DescriptorHeap", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

Descriptor* Frontend::cpu_descriptor(GuestAddr handle, DescriptorHeap** out) {
    auto entry = m_cpu_heaps.upper_bound(handle);
    if (entry == m_cpu_heaps.begin()) return nullptr;
    --entry;
    DescriptorHeap* heap = entry->second;
    const u64 delta = handle - heap->cpu_base;
    if (handle < heap->cpu_base || delta % descriptor_increment() != 0) {
        return nullptr;
    }
    const u64 index = delta / descriptor_increment();
    if (index >= heap->records.size()) return nullptr;
    if (out != nullptr) *out = heap;
    return &heap->records[static_cast<usize>(index)];
}

DescriptorHeap* Frontend::gpu_descriptor(u64 handle, u32& index) {
    auto entry = m_gpu_heaps.upper_bound(handle);
    if (entry == m_gpu_heaps.begin()) return nullptr;
    --entry;
    DescriptorHeap* heap = entry->second;
    const u64 delta = handle - heap->gpu_base;
    if (handle < heap->gpu_base || delta % descriptor_increment() != 0) {
        return nullptr;
    }
    const u64 slot = delta / descriptor_increment();
    if (slot >= heap->records.size()) return nullptr;
    index = static_cast<u32>(slot);
    return heap;
}

// =============================================================================
// Views
// =============================================================================

void Frontend::create_view(Marshaler& m, DescriptorType type) {
    // CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *, dest)
    // CreateShaderResourceView(ID3D12Resource *, const ..._DESC *, dest)
    // CreateUnorderedAccessView(ID3D12Resource *, ID3D12Resource *counter,
    //                           const ..._DESC *, dest)
    // CreateRenderTargetView / CreateDepthStencilView(resource, desc, dest)
    // Every one returns void; an invalid call leaves the destination as it
    // was (the runtime drops it; the debug layer is where it would be
    // reported).
    GuestAddr resource_address = 0;
    GuestAddr counter_address = 0;
    GuestAddr desc = 0;
    GuestAddr dest = 0;
    switch (type) {
    case DescriptorType::Cbv:
        desc = m.arg_ptr(1);
        dest = m.arg_ptr(2);
        break;
    case DescriptorType::Uav:
        resource_address = m.arg_ptr(1);
        counter_address = m.arg_ptr(2);
        desc = m.arg_ptr(3);
        dest = m.arg_ptr(4);
        break;
    default:
        resource_address = m.arg_ptr(1);
        desc = m.arg_ptr(2);
        dest = m.arg_ptr(3);
        break;
    }
    m.set_return(0);
    const char* api = type == DescriptorType::Cbv   ? "CreateConstantBufferView"
                      : type == DescriptorType::Srv ? "CreateShaderResourceView"
                      : type == DescriptorType::Uav ? "CreateUnorderedAccessView"
                      : type == DescriptorType::Rtv ? "CreateRenderTargetView"
                                                    : "CreateDepthStencilView";
    DescriptorHeap* heap = nullptr;
    Descriptor* record = cpu_descriptor(dest, &heap);
    if (record == nullptr || heap->type != heap_type_for(type)) {
        d3d12_diag("%s: 0x%llx is not a descriptor of a matching heap", api,
                   static_cast<unsigned long long>(dest));
        return;
    }
    Descriptor view{};
    view.type = type;
    Resource* resource = nullptr;
    if (resource_address != 0) {
        resource = find<Resource>(resource_address, Kind::Resource);
        if (resource == nullptr) {
            d3d12_diag("%s: pResource 0x%llx is not a resource", api,
                       static_cast<unsigned long long>(resource_address));
            return;
        }
        view.resource = resource->address;
        view.resource_serial = resource->serial;
    }
    if (type == DescriptorType::Cbv) {
        if (desc == 0) return;
        const auto& l = layout::D3D12_CONSTANT_BUFFER_VIEW_DESC[m_is_64bit];
        view.first_element = read_u64(desc + l.BufferLocation);
        view.num_elements = read_u32(desc + l.SizeInBytes);
        // Learn (D3D12_CONSTANT_BUFFER_VIEW_DESC): SizeInBytes is a multiple
        // of 256; BufferLocation 256-aligned (0 = a null view).
        if (view.num_elements % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT !=
                0 ||
            view.first_element %
                    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0) {
            d3d12_diag("%s: unaligned CBV", api);
            return;
        }
        *record = view;
        return;
    }
    if (desc == 0) {
        // A null description views the whole resource as it was created;
        // with no resource either there is nothing to describe.
        if (resource == nullptr) return;
        view.default_view = true;
        view.format = resource->desc.format;
        view.mip_levels = resource->desc.mip_levels;
        view.array_size = resource->array_size();
        view.component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (resource->desc.dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:
            // Learn: a buffer needs a description for every view type.
            d3d12_diag("%s: buffer view without a description", api);
            return;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            view.dimension = view.array_size > 1
                                 ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY
                                 : D3D12_SRV_DIMENSION_TEXTURE1D;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (resource->desc.sample_count > 1) {
                view.dimension =
                    type == DescriptorType::Dsv
                        ? (view.array_size > 1
                               ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY
                               : D3D12_DSV_DIMENSION_TEXTURE2DMS)
                        : (view.array_size > 1
                               ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                               : D3D12_SRV_DIMENSION_TEXTURE2DMS);
            } else if (type == DescriptorType::Dsv) {
                view.dimension = view.array_size > 1
                                     ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY
                                     : D3D12_DSV_DIMENSION_TEXTURE2D;
            } else {
                view.dimension = view.array_size > 1
                                     ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                     : D3D12_SRV_DIMENSION_TEXTURE2D;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            view.dimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            // An RTV/UAV of a volume covers every W slice of mip 0.
            view.array_size = resource->desc.depth_or_array_size;
            break;
        default:
            return;
        }
        if (type == DescriptorType::Dsv &&
            resource->desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            view.dimension = view.array_size > 1
                                 ? D3D12_DSV_DIMENSION_TEXTURE1DARRAY
                                 : D3D12_DSV_DIMENSION_TEXTURE1D;
        }
        if (type != DescriptorType::Srv) view.mip_levels = 1;
        *record = view;
        return;
    }
    // A described view (the resource may be null: a null descriptor of that
    // dimension, which reads 0 and drops writes).
    switch (type) {
    case DescriptorType::Srv: {
        const auto& l = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[m_is_64bit];
        view.format = read_u32(desc + l.Format);
        view.dimension = read_u32(desc + l.ViewDimension);
        view.component_mapping = read_u32(desc + l.Shader4ComponentMapping);
        switch (view.dimension) {
        case D3D12_SRV_DIMENSION_BUFFER:
            view.first_element = read_u64(desc + l.Buffer_FirstElement);
            view.num_elements = read_u32(desc + l.Buffer_NumElements);
            view.structure_stride = read_u32(desc + l.Buffer_StructureByteStride);
            view.flags = read_u32(desc + l.Buffer_Flags);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            view.most_detailed_mip = read_u32(desc + l.Texture1D_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.Texture1D_MipLevels);
            view.min_lod = read_f32(desc + l.Texture1D_ResourceMinLODClamp);
            view.array_size = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            view.most_detailed_mip =
                read_u32(desc + l.Texture1DArray_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.Texture1DArray_MipLevels);
            view.first_slice = read_u32(desc + l.Texture1DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture1DArray_ArraySize);
            view.min_lod = read_f32(desc + l.Texture1DArray_ResourceMinLODClamp);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            view.most_detailed_mip = read_u32(desc + l.Texture2D_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.Texture2D_MipLevels);
            view.plane_slice = read_u32(desc + l.Texture2D_PlaneSlice);
            view.min_lod = read_f32(desc + l.Texture2D_ResourceMinLODClamp);
            view.array_size = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            view.most_detailed_mip =
                read_u32(desc + l.Texture2DArray_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.Texture2DArray_MipLevels);
            view.first_slice = read_u32(desc + l.Texture2DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DArray_ArraySize);
            view.plane_slice = read_u32(desc + l.Texture2DArray_PlaneSlice);
            view.min_lod = read_f32(desc + l.Texture2DArray_ResourceMinLODClamp);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            view.mip_levels = 1;
            view.array_size = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            view.mip_levels = 1;
            view.first_slice = read_u32(desc + l.Texture2DMSArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DMSArray_ArraySize);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            view.most_detailed_mip = read_u32(desc + l.Texture3D_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.Texture3D_MipLevels);
            view.min_lod = read_f32(desc + l.Texture3D_ResourceMinLODClamp);
            view.array_size = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            view.most_detailed_mip =
                read_u32(desc + l.TextureCube_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.TextureCube_MipLevels);
            view.min_lod = read_f32(desc + l.TextureCube_ResourceMinLODClamp);
            view.array_size = 6;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            view.most_detailed_mip =
                read_u32(desc + l.TextureCubeArray_MostDetailedMip);
            view.mip_levels = read_u32(desc + l.TextureCubeArray_MipLevels);
            view.first_slice =
                read_u32(desc + l.TextureCubeArray_First2DArrayFace);
            view.array_size =
                read_u32(desc + l.TextureCubeArray_NumCubes) * 6u;
            view.min_lod =
                read_f32(desc + l.TextureCubeArray_ResourceMinLODClamp);
            break;
        case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
            unimplemented("ID3D12Device::CreateShaderResourceView",
                          "raytracing acceleration structures");
            return;
        default:
            return;
        }
        // MipLevels -1 means "every mip from MostDetailedMip down".
        if (resource != nullptr && view.mip_levels == 0xFFFFFFFFu) {
            view.mip_levels =
                resource->desc.mip_levels > view.most_detailed_mip
                    ? resource->desc.mip_levels - view.most_detailed_mip
                    : 0u;
        }
        break;
    }
    case DescriptorType::Uav: {
        const auto& l = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[m_is_64bit];
        view.format = read_u32(desc + l.Format);
        view.dimension = read_u32(desc + l.ViewDimension);
        view.mip_levels = 1;
        switch (view.dimension) {
        case D3D12_UAV_DIMENSION_BUFFER:
            view.first_element = read_u64(desc + l.Buffer_FirstElement);
            view.num_elements = read_u32(desc + l.Buffer_NumElements);
            view.structure_stride = read_u32(desc + l.Buffer_StructureByteStride);
            view.counter_offset = read_u64(desc + l.Buffer_CounterOffsetInBytes);
            view.flags = read_u32(desc + l.Buffer_Flags);
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1D:
            view.most_detailed_mip = read_u32(desc + l.Texture1D_MipSlice);
            view.array_size = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture1DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture1DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture1DArray_ArraySize);
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2D:
            view.most_detailed_mip = read_u32(desc + l.Texture2D_MipSlice);
            view.plane_slice = read_u32(desc + l.Texture2D_PlaneSlice);
            view.array_size = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture2DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture2DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DArray_ArraySize);
            view.plane_slice = read_u32(desc + l.Texture2DArray_PlaneSlice);
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            view.most_detailed_mip = read_u32(desc + l.Texture3D_MipSlice);
            view.first_slice = read_u32(desc + l.Texture3D_FirstWSlice);
            view.array_size = read_u32(desc + l.Texture3D_WSize);
            break;
        default:
            // TEXTURE2DMS / TEXTURE2DMSARRAY UAVs (SDK 26100, 6 and 7) need
            // multisample UAV support, which the caps pass reports absent.
            unimplemented("ID3D12Device::CreateUnorderedAccessView",
                          "multisample UAV dimensions");
            return;
        }
        if (counter_address != 0) {
            Resource* counter = find<Resource>(counter_address, Kind::Resource);
            if (counter == nullptr || !counter->is_buffer()) return;
            view.counter = counter->address;
            view.counter_serial = counter->serial;
        }
        break;
    }
    case DescriptorType::Rtv: {
        const auto& l = layout::D3D12_RENDER_TARGET_VIEW_DESC[m_is_64bit];
        view.format = read_u32(desc + l.Format);
        view.dimension = read_u32(desc + l.ViewDimension);
        view.mip_levels = 1;
        switch (view.dimension) {
        case D3D12_RTV_DIMENSION_BUFFER:
            view.first_element = read_u64(desc + l.Buffer_FirstElement);
            view.num_elements = read_u32(desc + l.Buffer_NumElements);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1D:
            view.most_detailed_mip = read_u32(desc + l.Texture1D_MipSlice);
            view.array_size = 1;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture1DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture1DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture1DArray_ArraySize);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2D:
            view.most_detailed_mip = read_u32(desc + l.Texture2D_MipSlice);
            view.plane_slice = read_u32(desc + l.Texture2D_PlaneSlice);
            view.array_size = 1;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture2DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture2DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DArray_ArraySize);
            view.plane_slice = read_u32(desc + l.Texture2DArray_PlaneSlice);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS:
            view.array_size = 1;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
            view.first_slice = read_u32(desc + l.Texture2DMSArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DMSArray_ArraySize);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            view.most_detailed_mip = read_u32(desc + l.Texture3D_MipSlice);
            view.first_slice = read_u32(desc + l.Texture3D_FirstWSlice);
            view.array_size = read_u32(desc + l.Texture3D_WSize);
            break;
        default:
            return;
        }
        break;
    }
    case DescriptorType::Dsv: {
        const auto& l = layout::D3D12_DEPTH_STENCIL_VIEW_DESC[m_is_64bit];
        view.format = read_u32(desc + l.Format);
        view.dimension = read_u32(desc + l.ViewDimension);
        view.flags = read_u32(desc + l.Flags);
        view.mip_levels = 1;
        switch (view.dimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D:
            view.most_detailed_mip = read_u32(desc + l.Texture1D_MipSlice);
            view.array_size = 1;
            break;
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture1DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture1DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture1DArray_ArraySize);
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2D:
            view.most_detailed_mip = read_u32(desc + l.Texture2D_MipSlice);
            view.array_size = 1;
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            view.most_detailed_mip = read_u32(desc + l.Texture2DArray_MipSlice);
            view.first_slice = read_u32(desc + l.Texture2DArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DArray_ArraySize);
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMS:
            view.array_size = 1;
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
            view.first_slice = read_u32(desc + l.Texture2DMSArray_FirstArraySlice);
            view.array_size = read_u32(desc + l.Texture2DMSArray_ArraySize);
            break;
        default:
            return;
        }
        break;
    }
    default:
        return;
    }
    // UINT_MAX array / mip counts: every slice or mip from the first on
    // (Learn, D3D12_TEX2D_ARRAY_RTV ArraySize, D3D12_TEX2D_SRV MipLevels).
    if (resource != nullptr && !resource->is_buffer()) {
        const u32 slices =
            resource->desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? std::max(1u, resource->desc.depth_or_array_size >>
                                   std::min(view.most_detailed_mip, 31u))
                : resource->array_size();
        if (view.array_size == 0xFFFFFFFFu) {
            view.array_size = slices > view.first_slice ? slices - view.first_slice : 0u;
        }
        const u32 mips = std::max(1u, resource->desc.mip_levels);
        if (view.mip_levels == 0xFFFFFFFFu) {
            view.mip_levels =
                mips > view.most_detailed_mip ? mips - view.most_detailed_mip : 0u;
        }
    }
    // A view format of UNKNOWN takes the resource's (Learn, the *_VIEW_DESC
    // Format members).
    if (resource != nullptr && view.format == DXGI_FORMAT_UNKNOWN &&
        !(type == DescriptorType::Srv &&
          view.dimension == D3D12_SRV_DIMENSION_BUFFER) &&
        !(type == DescriptorType::Uav &&
          view.dimension == D3D12_UAV_DIMENSION_BUFFER)) {
        view.format = resource->desc.format;
    }
    // Measured (a03 invalid_view "SRV RGBA8 texture as BGRA8"): a texture
    // view in a format outside the resource's cast family is an invalid
    // call that removes the device.
    if (type == DescriptorType::Srv && resource != nullptr &&
        !resource->is_buffer() && view.format != resource->desc.format &&
        !same_cast_family(view.format, resource->desc.format)) {
        remove_device(DXGI_ERROR_INVALID_CALL,
                      "a shader resource view format outside the resource's "
                      "cast family");
        return;
    }
    *record = view;
}

bool Frontend::read_sampler(GuestAddr address, bool desc2,
                            SamplerState& out) const {
    if (address == 0) return false;
    // D3D12_SAMPLER_DESC2 is D3D12_SAMPLER_DESC plus Flags, at identical
    // offsets (its border colour is a union of float and uint arrays).
    const auto& l = layout::D3D12_SAMPLER_DESC2[m_is_64bit];
    static_assert(layout::D3D12_SAMPLER_DESC[1].MaxLOD ==
                  layout::D3D12_SAMPLER_DESC2[1].MaxLOD);
    out.filter = read_u32(address + l.Filter);
    out.address_u = read_u32(address + l.AddressU);
    out.address_v = read_u32(address + l.AddressV);
    out.address_w = read_u32(address + l.AddressW);
    out.mip_lod_bias = read_f32(address + l.MipLODBias);
    out.max_anisotropy = read_u32(address + l.MaxAnisotropy);
    out.comparison_func = read_u32(address + l.ComparisonFunc);
    for (u32 c = 0; c < 4; ++c) {
        out.border[c] = read_u32(address + l.FloatBorderColor + c * 4);
    }
    out.min_lod = read_f32(address + l.MinLOD);
    out.max_lod = read_f32(address + l.MaxLOD);
    out.flags = desc2 ? read_u32(address + l.Flags) : 0u;
    return true;
}

void Frontend::create_sampler(Marshaler& m, bool desc2) {
    // CreateSampler(const D3D12_SAMPLER_DESC *, dest) / CreateSampler2.
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr dest = m.arg_ptr(2);
    m.set_return(0);
    DescriptorHeap* heap = nullptr;
    Descriptor* record = cpu_descriptor(dest, &heap);
    if (record == nullptr || heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        return;
    }
    Descriptor view{};
    view.type = DescriptorType::Sampler;
    if (!read_sampler(desc, desc2, view.sampler)) return;
    // What a descriptor-table sampler cannot carry here (dxbc_to_spirv.hpp
    // register_remap; D3D11 emulates both per s# register): a border colour
    // other than Metal's three, and MINIMUM / MAXIMUM reduction. The
    // sampler is kept -- the nearest Metal border / a standard filter -- and
    // the gap reported where it is created.
    const SamplerState& state = view.sampler;
    const bool border = state.address_u == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                        state.address_v == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                        state.address_w == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    if (border) {
        std::array<float, 4> colour{};
        for (u32 c = 0; c < 4; ++c) std::memcpy(&colour[c], &state.border[c], 4);
        const bool uint_colour =
            (state.flags & 0x1u) != 0;  // D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR
        const bool standard =
            !uint_colour && colour[0] == colour[1] && colour[1] == colour[2] &&
            (colour[0] == 0.0f || colour[0] == 1.0f) &&
            (colour[3] == 0.0f || colour[3] == 1.0f) &&
            !(colour[0] == 1.0f && colour[3] == 0.0f);
        if (!standard) {
            unimplemented("ID3D12Device::CreateSampler",
                          "a border colour other than transparent black, "
                          "opaque black or opaque white");
        }
    }
    const u32 reduction = (state.filter >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) &
                          D3D12_FILTER_REDUCTION_TYPE_MASK;
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ||
        reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM) {
        unimplemented("ID3D12Device::CreateSampler",
                      "MINIMUM / MAXIMUM filter reduction in a descriptor heap");
    }
    *record = view;
}

void Frontend::copy_descriptors(Marshaler& m) {
    // CopyDescriptors(UINT NumDestDescriptorRanges,
    //     const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts,
    //     const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
    //     const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts,
    //     const UINT *pSrcDescriptorRangeSizes,
    //     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType).
    const u32 dest_ranges = m.arg32(1);
    const GuestAddr dest_starts = m.arg_ptr(2);
    const GuestAddr dest_sizes = m.arg_ptr(3);
    const u32 src_ranges = m.arg32(4);
    const GuestAddr src_starts = m.arg_ptr(5);
    const GuestAddr src_sizes = m.arg_ptr(6);
    const u32 heap_type = m.arg32(7);
    m.set_return(0);
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    // Gather the source records first: the ranges may overlap (Learn leaves
    // overlapping source and destination undefined; copying through a
    // snapshot is one of the defined outcomes).
    std::vector<Descriptor> source;
    for (u32 range = 0; range < src_ranges; ++range) {
        GuestAddr start = 0;
        if (!read_pointer(src_starts + range * pointer_size, start)) return;
        // Learn: a null sizes array means every range is one descriptor.
        const u32 size = src_sizes != 0 ? read_u32(src_sizes + range * 4) : 1u;
        for (u32 index = 0; index < size; ++index) {
            DescriptorHeap* heap = nullptr;
            const Descriptor* record = cpu_descriptor(
                start + static_cast<GuestAddr>(index) * descriptor_increment(),
                &heap);
            if (record == nullptr || heap->type != heap_type) return;
            source.push_back(*record);
        }
    }
    usize cursor = 0;
    for (u32 range = 0; range < dest_ranges; ++range) {
        GuestAddr start = 0;
        if (!read_pointer(dest_starts + range * pointer_size, start)) return;
        const u32 size =
            dest_sizes != 0 ? read_u32(dest_sizes + range * 4) : 1u;
        for (u32 index = 0; index < size; ++index) {
            if (cursor >= source.size()) return;
            DescriptorHeap* heap = nullptr;
            Descriptor* record = cpu_descriptor(
                start + static_cast<GuestAddr>(index) * descriptor_increment(),
                &heap);
            if (record == nullptr || heap->type != heap_type) return;
            *record = source[cursor++];
        }
    }
}

void Frontend::copy_descriptors_simple(Marshaler& m) {
    // CopyDescriptorsSimple(UINT NumDescriptors,
    //     D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
    //     D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
    //     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType).
    const u32 count = m.arg32(1);
    const GuestAddr dest = m.arg_ptr(2);
    const GuestAddr src = m.arg_ptr(3);
    const u32 heap_type = m.arg32(4);
    m.set_return(0);
    std::vector<Descriptor> source;
    source.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        DescriptorHeap* heap = nullptr;
        const Descriptor* record = cpu_descriptor(
            src + static_cast<GuestAddr>(index) * descriptor_increment(), &heap);
        if (record == nullptr || heap->type != heap_type) return;
        source.push_back(*record);
    }
    for (u32 index = 0; index < count; ++index) {
        DescriptorHeap* heap = nullptr;
        Descriptor* record = cpu_descriptor(
            dest + static_cast<GuestAddr>(index) * descriptor_increment(),
            &heap);
        if (record == nullptr || heap->type != heap_type) return;
        *record = source[index];
    }
}

}  // namespace vitreo::winapi::d3d12
