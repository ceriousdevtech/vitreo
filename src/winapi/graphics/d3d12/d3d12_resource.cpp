// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_resource.cpp
/// @brief ID3D12Heap and ID3D12Resource: memory, GPU virtual addresses,
/// committed / placed / reserved creation, Map, GetDesc and friends.
///
/// PLAN decisions 3 and 4. Every heap -- an ID3D12Heap, or the implicit heap a
/// committed resource gets -- is a BackendHeap exposing one buffer; a buffer
/// resource is a window (heap buffer, offset) of it and its GPU VA is the
/// heap's VA plus the offset, so placed buffers alias exactly as D3D12 says.
/// CPU-visible heaps (UPLOAD, READBACK, CUSTOM with CPU pages) are host memory
/// mapped into the guest once, at creation: Map hands out that mapping, and
/// the GPU reads and writes the same bytes.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <vector>

namespace vitreo::winapi::d3d12 {
namespace {

constexpr u64 KB64 = 64u * 1024u;

[[nodiscard]] constexpr u64 round_up(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] u32 full_mip_count(u64 width, u32 height, u32 depth) {
    u64 largest = std::max<u64>({width, height, depth});
    u32 levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

/// Depth-stencil formats (and their typeless families) hold a stencil plane:
/// D3D12 counts it as a second plane of subresources.
[[nodiscard]] u32 plane_count(u32 format) {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return 2;
    default:
        return 1;
    }
}

/// The heap flags this runtime defines (d3d12.h D3D12_HEAP_FLAGS without
/// TOOLS_USE_MANUAL_WRITE_TRACKING, which needs OPTIONS17
/// ManualWriteTrackingResourceSupported, reported FALSE).
constexpr u32 kDefinedHeapFlags =
    D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_DENY_BUFFERS |
    D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
    D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES |
    D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES |
    D3D12_HEAP_FLAG_HARDWARE_PROTECTED | D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH |
    D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS |
    D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

/// Creation-time flags GetDesc / GetHeapProperties do not report (measured,
/// a02 heaps "CREATE_NOT_RESIDENT" / "CREATE_NOT_ZEROED": flags=0x0).
constexpr u32 kCreationOnlyHeapFlags =
    D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

constexpr u32 kDenyHeapFlags = D3D12_HEAP_FLAG_DENY_BUFFERS |
                               D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES |
                               D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;

/// The single-bit write states (Learn, "Using resource barriers": a
/// subresource is in any combination of read states or exactly one write
/// state).
constexpr u32 kWriteStates =
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
    D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
    D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;

/// Metal's 2D / 1D texture size limit (Apple GPU families: 16384). D3D12
/// accepts a single-mip 2D texture up to 2^23 (measured, a02 textures
/// "2D 16385x1"); the backend cannot make one.
constexpr u64 kBackendMaxTexture2D = 16384;

[[nodiscard]] ResourceDesc to_layout_desc(const ResourceDescription& d) {
    ResourceDesc out{};
    out.dimension = d.dimension;
    out.alignment = d.alignment;
    out.width = d.width;
    out.height = d.height;
    out.depth_or_array_size = static_cast<u16>(d.depth_or_array_size);
    out.mip_levels = static_cast<u16>(d.mip_levels);
    out.format = d.format;
    out.sample_count = d.sample_count;
    out.sample_quality = d.sample_quality;
    out.layout = d.layout;
    out.flags = d.flags;
    out.mip_region_width = d.mip_region[0];
    out.mip_region_height = d.mip_region[1];
    out.mip_region_depth = d.mip_region[2];
    return out;
}

}  // namespace

bool texture_is_typeless(u32 format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}


// =============================================================================
// Formats
// =============================================================================

graphics::PixelFormat texture_format_for(u32 dxgi_format, u32 resource_flags) {
    if ((resource_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        // A depth-stencil resource of a typeless family is stored as its
        // depth format; its SRVs read the depth (or stencil) plane.
        switch (dxgi_format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return graphics::PixelFormat::D32Float_S8Uint;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return graphics::PixelFormat::D32Float;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return graphics::PixelFormat::D24Unorm_S8Uint;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return graphics::PixelFormat::D16Unorm;
        default:
            break;
        }
    }
    return d3d11::texture_storage_format(dxgi_format);
}

FormatBlock format_block_info(u32 dxgi_format) {
    FormatBlock block{};
    const graphics::PixelFormat storage =
        d3d11::texture_storage_format(dxgi_format);
    const graphics::FormatInfo info = graphics::format_table(storage);
    if (info.is_compressed) {
        block.width = 4;
        block.height = 4;
        block.bytes = graphics::compressed_block_bytes(storage);
    } else {
        block.bytes = info.bytes_per_pixel;
    }
    return block;
}

u32 dxgi_element_bytes(u32 dxgi_format) {
    const FormatBlock block = format_block_info(dxgi_format);
    return block.width == 1 ? block.bytes : 0u;
}

u32 Resource::subresource_count() const {
    if (is_buffer()) return 1;
    return std::max(1u, desc.mip_levels) * std::max(1u, array_size()) *
           plane_count(desc.format);
}

// =============================================================================
// Guest structs
// =============================================================================

bool Frontend::read_heap_properties(GuestAddr address,
                                    HeapProperties& out) const {
    if (address == 0) return false;
    const auto& l = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
    out.type = read_u32(address + l.Type);
    out.cpu_page_property = read_u32(address + l.CPUPageProperty);
    out.memory_pool_preference = read_u32(address + l.MemoryPoolPreference);
    out.creation_node_mask = read_u32(address + l.CreationNodeMask);
    out.visible_node_mask = read_u32(address + l.VisibleNodeMask);
    return true;
}

void Frontend::write_heap_properties(GuestAddr address,
                                     const HeapProperties& p) {
    const auto& l = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
    (void)m_memory.write_u32(address + l.Type, p.type);
    (void)m_memory.write_u32(address + l.CPUPageProperty, p.cpu_page_property);
    (void)m_memory.write_u32(address + l.MemoryPoolPreference,
                             p.memory_pool_preference);
    (void)m_memory.write_u32(address + l.CreationNodeMask,
                             p.creation_node_mask);
    (void)m_memory.write_u32(address + l.VisibleNodeMask, p.visible_node_mask);
}

bool Frontend::read_resource_desc(GuestAddr address, bool desc1,
                                  ResourceDescription& out) const {
    if (address == 0) return false;
    const auto& l = layout::D3D12_RESOURCE_DESC1[m_is_64bit];
    // D3D12_RESOURCE_DESC is D3D12_RESOURCE_DESC1 without its last member,
    // at identical offsets.
    static_assert(layout::D3D12_RESOURCE_DESC[0].Flags ==
                  layout::D3D12_RESOURCE_DESC1[0].Flags);
    static_assert(layout::D3D12_RESOURCE_DESC[1].Flags ==
                  layout::D3D12_RESOURCE_DESC1[1].Flags);
    out.dimension = read_u32(address + l.Dimension);
    out.alignment = read_u64(address + l.Alignment);
    out.width = read_u64(address + l.Width);
    out.height = read_u32(address + l.Height);
    const auto depth = m_memory.read_u16(address + l.DepthOrArraySize);
    const auto mips = m_memory.read_u16(address + l.MipLevels);
    out.depth_or_array_size = depth.is_ok() ? depth.value() : 0;
    out.mip_levels = mips.is_ok() ? mips.value() : 0;
    out.format = read_u32(address + l.Format);
    out.sample_count = read_u32(address + l.SampleDesc_Count);
    out.sample_quality = read_u32(address + l.SampleDesc_Quality);
    out.layout = read_u32(address + l.Layout);
    out.flags = read_u32(address + l.Flags);
    if (desc1) {
        out.mip_region[0] = read_u32(address + l.SamplerFeedbackMipRegion_Width);
        out.mip_region[1] =
            read_u32(address + l.SamplerFeedbackMipRegion_Height);
        out.mip_region[2] = read_u32(address + l.SamplerFeedbackMipRegion_Depth);
    }
    return true;
}

void Frontend::write_resource_desc(GuestAddr address, const ResourceDescription& d,
                                   bool desc1) {
    const auto& l = layout::D3D12_RESOURCE_DESC1[m_is_64bit];
    (void)m_memory.write_u32(address + l.Dimension, d.dimension);
    (void)m_memory.write_u32(address + l.Dimension + 4, 0);
    (void)m_memory.write_u64(address + l.Alignment, d.alignment);
    (void)m_memory.write_u64(address + l.Width, d.width);
    (void)m_memory.write_u32(address + l.Height, d.height);
    (void)m_memory.write_u16(address + l.DepthOrArraySize,
                             static_cast<u16>(d.depth_or_array_size));
    (void)m_memory.write_u16(address + l.MipLevels,
                             static_cast<u16>(d.mip_levels));
    (void)m_memory.write_u32(address + l.Format, d.format);
    (void)m_memory.write_u32(address + l.SampleDesc_Count, d.sample_count);
    (void)m_memory.write_u32(address + l.SampleDesc_Quality, d.sample_quality);
    (void)m_memory.write_u32(address + l.Layout, d.layout);
    (void)m_memory.write_u32(address + l.Flags, d.flags);
    if (desc1) {
        (void)m_memory.write_u32(address + l.SamplerFeedbackMipRegion_Width,
                                 d.mip_region[0]);
        (void)m_memory.write_u32(address + l.SamplerFeedbackMipRegion_Height,
                                 d.mip_region[1]);
        (void)m_memory.write_u32(address + l.SamplerFeedbackMipRegion_Depth,
                                 d.mip_region[2]);
    }
}

HeapProperties Frontend::resolve_custom(const HeapProperties& p) const {
    // The CUSTOM equivalent of a heap type, as GetCustomHeapProperties
    // divulges it (d3d12_device.cpp).
    HeapProperties out = p;
    switch (p.type) {
    case D3D12_HEAP_TYPE_DEFAULT:
        out.cpu_page_property = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        out.memory_pool_preference = D3D12_MEMORY_POOL_L0;
        break;
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        out.cpu_page_property = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        out.memory_pool_preference = D3D12_MEMORY_POOL_L0;
        break;
    case D3D12_HEAP_TYPE_GPU_UPLOAD:
        // Learn (D3D12_HEAP_TYPE_GPU_UPLOAD): write-combined CPU pages on
        // GPU-local memory, which on this unified-memory adapter is L0.
        out.cpu_page_property = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        out.memory_pool_preference = D3D12_MEMORY_POOL_L0;
        break;
    default:
        break;
    }
    return out;
}

bool Frontend::cpu_visible(const HeapProperties& p) const {
    const HeapProperties resolved = resolve_custom(p);
    return resolved.cpu_page_property == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
           resolved.cpu_page_property ==
               D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
}

/// Learn (D3D12_HEAP_PROPERTIES): CUSTOM requires an explicit page property
/// and pool; the other types require UNKNOWN for both. Node masks may be 0
/// (meaning node 0) or name the one node.
[[nodiscard]] static bool heap_properties_valid(const HeapProperties& p,
                                                bool gpu_upload) {
    if (p.creation_node_mask > 1 || p.visible_node_mask > 1) return false;
    switch (p.type) {
    case D3D12_HEAP_TYPE_GPU_UPLOAD:
        // D3D12_OPTIONS16 GPUUploadHeapSupported.
        if (!gpu_upload) return false;
        [[fallthrough]];
    case D3D12_HEAP_TYPE_DEFAULT:
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        return p.cpu_page_property == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
               p.memory_pool_preference == D3D12_MEMORY_POOL_UNKNOWN;
    case D3D12_HEAP_TYPE_CUSTOM:
        return p.cpu_page_property != D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
               p.cpu_page_property <= D3D12_CPU_PAGE_PROPERTY_WRITE_BACK &&
               p.memory_pool_preference != D3D12_MEMORY_POOL_UNKNOWN &&
               p.memory_pool_preference <= D3D12_MEMORY_POOL_L1 &&
               // L1 is video memory, which a UMA adapter does not have.
               p.memory_pool_preference == D3D12_MEMORY_POOL_L0;
    default:
        return false;
    }
}

// =============================================================================
// Memory and GPU virtual addresses
// =============================================================================

std::shared_ptr<Memory> Frontend::allocate_memory(u64 size, bool cpu,
                                                  bool buffers_only) {
    if (size == 0) return nullptr;
    graphics::BackendHeapDesc desc{};
    desc.size = round_up(size, KB64);
    desc.cpu_visible = cpu;
    desc.buffers_only = buffers_only || cpu;
    auto backend = m_backend.create_heap(desc);
    if (!backend || !backend->valid() || backend->buffer() == nullptr) {
        return nullptr;
    }
    std::shared_ptr<Memory> memory(new Memory, [this](Memory* retired) {
        retire_memory(retired);
        delete retired;
    });
    memory->size = desc.size;
    memory->cpu_visible = cpu;
    memory->backend = std::move(backend);
    if (cpu) {
        // The persistent guest mapping every Map of this memory hands out.
        void* host = memory->backend->host_pointer();
        const usize bytes = static_cast<usize>(desc.size);
        GuestAddr base = 0;
        for (int attempt = 0; attempt < 8 && base == 0 && host != nullptr;
             ++attempt) {
            const GuestAddr candidate =
                m_memory.find_free_gap(m_guest_map_hint, bytes, KB64);
            if (candidate == 0) break;
            if (m_memory
                    .map_region_backed(candidate, bytes, host,
                                       MemoryProtection::ReadWrite)
                    .is_ok()) {
                base = candidate;
            } else {
                m_guest_map_hint = candidate + bytes;
            }
        }
        if (base == 0) {
            d3d12_diag("no guest range for a %llu-byte CPU-visible heap",
                       static_cast<unsigned long long>(desc.size));
            return nullptr;
        }
        memory->guest = base;
        memory->guest_size = bytes;
    }
    // VAs are 64 KiB aligned and never reused, with a 64 KiB gap between
    // allocations so a VA one past a buffer never names the next one.
    memory->va = m_next_va;
    m_next_va += desc.size + KB64;
    m_va_ranges.emplace(memory->va, memory.get());
    return memory;
}

void Frontend::retire_memory(Memory* memory) {
    if (memory == nullptr) return;
    m_va_ranges.erase(memory->va);
    if (memory->guest != 0 && !m_tearing_down) {
        (void)m_memory.unmap_region(memory->guest, memory->guest_size);
        memory->guest = 0;
    }
}

bool Frontend::resolve_va(u64 va, u64 bytes, graphics::BackendBuffer*& buffer,
                          u64& offset) {
    buffer = nullptr;
    offset = 0;
    auto range = m_va_ranges.upper_bound(va);
    if (range == m_va_ranges.begin()) return false;
    --range;
    const Memory* memory = range->second;
    if (va < memory->va || va - memory->va > memory->size ||
        bytes > memory->size - (va - memory->va)) {
        return false;
    }
    buffer = memory->backend->buffer();
    offset = va - memory->va;
    return buffer != nullptr;
}

// =============================================================================
// ID3D12Heap
// =============================================================================

void Frontend::create_heap(Marshaler& m, Device& device, bool heap1) {
    // CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppvHeap);
    // CreateHeap1 inserts ID3D12ProtectedResourceSession *pProtectedSession.
    const GuestAddr desc = m.arg_ptr(1);
    const usize riid_index = heap1 ? 3 : 2;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    if (heap1 && m.arg_ptr(2) != 0) {
        unimplemented("ID3D12Device4::CreateHeap1", "protected sessions");
        m.set_return(E_INVALIDARG);
        return;
    }
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& l = layout::D3D12_HEAP_DESC[m_is_64bit];
    auto heap = std::make_unique<Heap>();
    heap->device = device.address;
    heap->size = read_u64(desc + l.SizeInBytes);
    (void)read_heap_properties(desc + l.Properties, heap->properties);
    heap->alignment = read_u64(desc + l.Alignment);
    heap->flags = read_u32(desc + l.Flags);
    // Learn (D3D12_HEAP_DESC): Alignment is 0 (64 KiB), 64 KiB, or 4 MiB for
    // MSAA; SizeInBytes is non-zero.
    if (heap->alignment == 0) {
        heap->alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    if (heap->size == 0 ||
        !heap_properties_valid(heap->properties, gpu_upload_heaps()) ||
        (heap->alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
         heap->alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (u32 error = heap_flags_error(heap->flags, heap->properties, true,
                                     heap1 ? "ID3D12Device4::CreateHeap1"
                                           : "ID3D12Device::CreateHeap");
        error != S_OK) {
        m.set_return(error);
        return;
    }
    // Measured (a02 heaps "flags=SHARED", "HARDWARE_PROTECTED",
    // "ALLOW_SHADER_ATOMICS"): accepted and reported. A SHARED heap is
    // ordinary memory here; exporting it (CreateSharedHandle) is where
    // cross-process sharing fails loudly. HARDWARE_PROTECTED needs no
    // protected session to create (CreateHeap1 with one fails loudly).
    heap->flags &= ~kCreationOnlyHeapFlags;
    // Learn (D3D12_HEAP_DESC): the normalised properties read back from
    // GetDesc: node masks of 0 become 1 (measured, d12core_facts_probe
    // heap_desc).
    if (heap->properties.creation_node_mask == 0) {
        heap->properties.creation_node_mask = 1;
    }
    if (heap->properties.visible_node_mask == 0) {
        heap->properties.visible_node_mask = 1;
    }
    const bool cpu = cpu_visible(heap->properties);
    const bool buffers_only =
        (heap->flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
        D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (out == 0) {
        // Validation-only call.
        m.set_return(S_FALSE);
        return;
    }
    heap->memory = allocate_memory(heap->size, cpu, buffers_only);
    if (!heap->memory) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(return_new_object(std::move(heap), m.arg_ptr(riid_index),
                                   out));
}

void Frontend::heap_method(Marshaler& m, Heap& heap, usize slot) {
    switch (slot) {
    case HeapSlot::GetDesc: {
        // Hidden return pointer (_WIN32 C interface): This, RetVal.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            const auto& l = layout::D3D12_HEAP_DESC[m_is_64bit];
            (void)m_memory.write_u64(out + l.SizeInBytes, heap.size);
            write_heap_properties(out + l.Properties, heap.properties);
            (void)m_memory.write_u64(out + l.Alignment, heap.alignment);
            (void)m_memory.write_u32(out + l.Flags, heap.flags);
        }
        m.set_return(out);
        return;
    }
    case HeapSlot::GetProtectedResourceSession: {
        // ID3D12Heap1::GetProtectedResourceSession(REFIID, void **). No heap
        // here has a protected session: Learn says the call then succeeds
        // only for sessions that exist, so it answers E_NOINTERFACE-free
        // failure with a null output.
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)write_pointer(out, 0);
        m.set_return(E_INVALIDARG);
        return;
    }
    default:
        unimplemented("ID3D12Heap", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// Resource creation
// =============================================================================


u32 Frontend::heap_flags_error(u32 flags, const HeapProperties& props,
                               bool heap_object, const char* api) {
    // Measured (a02 heaps / buffers "heap-flags=..."): undefined bits,
    // ALLOW_DISPLAY, and SHARED_CROSS_ADAPTER without SHARED are refused;
    // a heap may not deny all three categories; a committed resource names
    // none (its implicit heap gets the right one) and may not ask for
    // cross-adapter atomics; write watch needs CPU pages.
    if ((flags & ~kDefinedHeapFlags) != 0 ||
        (flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0) {
        return E_INVALIDARG;
    }
    if ((flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) != 0 &&
        (flags & D3D12_HEAP_FLAG_SHARED) == 0) {
        return E_INVALIDARG;
    }
    if (heap_object && (flags & kDenyHeapFlags) == kDenyHeapFlags) {
        return E_INVALIDARG;
    }
    if (!heap_object &&
        (flags & (kDenyHeapFlags | D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS)) != 0) {
        return E_INVALIDARG;
    }
    if ((flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) != 0) {
        if (!cpu_visible(props)) return E_INVALIDARG;
        // GetWriteWatch tracks VirtualAlloc(MEM_WRITE_WATCH) regions; the
        // persistent mapping of a heap is not one, so a write-watched heap
        // would report no writes.
        unimplemented(api, "D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH (GetWriteWatch "
                           "over a heap mapping)");
        return E_INVALIDARG;
    }
    return S_OK;
}

u32 Frontend::validate_resource_desc(ResourceDescription& d,
                                     const HeapProperties& props,
                                     u32 initial_state, GuestAddr clear_value,
                                     bool committed, const char* api) {
    const bool is_buffer = d.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    // Measured (a02 buffers "flags=0x100"): a buffer created with
    // RAYTRACING_ACCELERATION_STRUCTURE is an ordinary buffer whose GetDesc
    // no longer names the flag.
    if (is_buffer) {
        d.flags &= ~D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    if (const char* rule = resource_desc_violation(to_layout_desc(d), layout());
        rule != nullptr) {
        d3d12_diag("%s: %s", api, rule);
        return E_INVALIDARG;
    }
    // Cross-adapter resources (and ROW_MAJOR textures, which exist only for
    // them): D3D12_OPTIONS CrossNodeSharingTier NOT_SUPPORTED and
    // CrossAdapterRowMajorTextureSupported FALSE on this device; measured
    // refused on WARP too (a02 "ALLOW_CROSS_ADAPTER + heap SHARED|
    // SHARED_CROSS_ADAPTER", "2D ROW_MAJOR").
    if ((d.flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0 ||
        (!is_buffer && d.layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)) {
        d3d12_diag("%s: cross-adapter resources are not supported", api);
        return E_INVALIDARG;
    }
    if (!is_buffer && d.layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
        // D3D12_OPTIONS StandardSwizzle64KBSupported is FALSE: the backend's
        // texture memory has no standard-swizzle layout to Map.
        unimplemented(api, "D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE");
        return E_INVALIDARG;
    }
    const HeapProperties resolved = resolve_custom(props);
    if (props.type == D3D12_HEAP_TYPE_UPLOAD ||
        props.type == D3D12_HEAP_TYPE_READBACK) {
        // Learn (D3D12_HEAP_TYPE): upload and readback heaps hold buffers
        // (measured: a02 textures "2D on UPLOAD" / "on READBACK").
        if (!is_buffer) return E_INVALIDARG;
        if ((d.flags & (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
            return E_INVALIDARG;
        }
    }
    (void)resolved;
    if (committed) {
        // Measured (a02 buffers "UPLOAD state=" / "READBACK state="): an
        // UPLOAD buffer starts in any subset of GENERIC_READ, a READBACK
        // buffer in COPY_DEST / RESOLVE_DEST (or COMMON); a DEFAULT buffer
        // in anything. A placed buffer's state is not checked against its
        // heap (a02 placed "UPLOAD heap: buffer COPY_DEST").
        if (props.type == D3D12_HEAP_TYPE_UPLOAD &&
            (initial_state & ~D3D12_RESOURCE_STATE_GENERIC_READ) != 0) {
            return E_INVALIDARG;
        }
        if (props.type == D3D12_HEAP_TYPE_READBACK &&
            (initial_state & ~(D3D12_RESOURCE_STATE_COPY_DEST |
                               D3D12_RESOURCE_STATE_RESOLVE_DEST)) != 0) {
            return E_INVALIDARG;
        }
        if (!is_buffer) {
            // Measured (a02 textures "state="): a write state needs its
            // usage flag and stands alone; VERTEX_AND_CONSTANT_BUFFER is not
            // a texture state (GENERIC_READ, which contains it, is).
            const u32 writes = initial_state & kWriteStates;
            if ((writes != 0 && writes != initial_state) ||
                (writes & (writes - 1)) != 0) {
                return E_INVALIDARG;
            }
            if (((initial_state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0 &&
                 (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) ||
                ((initial_state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 &&
                 (d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) ||
                ((initial_state & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0 &&
                 (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0)) {
                return E_INVALIDARG;
            }
            if (initial_state != D3D12_RESOURCE_STATE_GENERIC_READ &&
                (initial_state &
                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) != 0) {
                return E_INVALIDARG;
            }
        }
    }
    if (clear_value != 0) {
        // Measured (a02 textures "clear ..."; buffers / placed "with clear
        // value"): an optimized clear value is for render targets and depth
        // stencils only, in the resource's cast family and usable as that
        // kind of view.
        const bool rt = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        const bool ds = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        if (is_buffer || (!rt && !ds)) return E_INVALIDARG;
        const u32 format = read_u32(clear_value);
        if (format == DXGI_FORMAT_UNKNOWN || !same_cast_family(format, d.format)) {
            return E_INVALIDARG;
        }
        const u32 support = layout().format_support1(format);
        if ((rt && (support & FORMAT_SUPPORT1_RENDER_TARGET) == 0) ||
            (ds && (support & FORMAT_SUPPORT1_DEPTH_STENCIL) == 0)) {
            return E_INVALIDARG;
        }
    }
    if (!is_buffer) {
        // Descriptions D3D12 accepts that the backend cannot make.
        if (d.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
            (d.width > kBackendMaxTexture2D || d.height > kBackendMaxTexture2D)) {
            unimplemented(api, "textures above Metal's 16384 texel limit");
            return E_INVALIDARG;
        }
        const u32 full = full_mip_count(
            d.width, d.height,
            d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? d.depth_or_array_size
                : 1u);
        if (d.mip_levels > full) {
            unimplemented(api, "MipLevels beyond the full mip chain");
            return E_INVALIDARG;
        }
        if (texture_format_for(d.format, d.flags) ==
            graphics::PixelFormat::Unknown) {
            unimplemented(api, "a DXGI format with no backend storage");
            return E_INVALIDARG;
        }
    }
    return S_OK;
}

u32 Frontend::create_resource_backend(Resource& resource,
                                      const graphics::BackendHeap* heap,
                                      u64 heap_offset, bool cpu) {
    const ResourceDescription& d = resource.desc;
    resource.pixel_format = texture_format_for(d.format, d.flags);
    const bool shader_resource =
        (d.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    if (d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        graphics::BackendTexture3DDesc desc{};
        desc.width = static_cast<unsigned>(d.width);
        desc.height = d.height;
        desc.depth = d.depth_or_array_size;
        desc.mip_levels = d.mip_levels;
        desc.pixel_format = resource.pixel_format;
        desc.shader_resource = shader_resource;
        desc.render_target =
            (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        desc.unordered_access =
            (d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        desc.cpu_writable = cpu;
        desc.pixel_format_view = texture_is_typeless(d.format);
        desc.heap = heap;
        desc.heap_offset = heap_offset;
        resource.texture3d = m_backend.create_texture_3d(desc, nullptr, 0, 0);
        return resource.texture3d ? S_OK : E_OUTOFMEMORY;
    }
    if (graphics::is_planar_format(resource.pixel_format) && heap != nullptr) {
        unimplemented("ID3D12Device::CreatePlacedResource",
                      "planar (video) formats in a heap");
        return E_INVALIDARG;
    }
    graphics::BackendTexture2DDesc desc{};
    desc.width = static_cast<unsigned>(d.width);
    desc.height = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u
                                                                    : d.height;
    desc.mip_levels = d.mip_levels;
    desc.array_size = d.depth_or_array_size;
    desc.pixel_format = resource.pixel_format;
    // A typeless family is viewed as its typed members; a UAV of a typeless
    // resource stores through a typed view too.
    desc.pixel_format_view = texture_is_typeless(d.format);
    desc.shader_resource = shader_resource;
    desc.render_target =
        (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    desc.depth_stencil =
        (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    desc.unordered_access =
        (d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    desc.cpu_writable = cpu;
    desc.sample_count = std::max(1u, d.sample_count);
    desc.heap = heap;
    desc.heap_offset = heap_offset;
    resource.texture = m_backend.create_texture_2d(desc, nullptr, 0);
    return resource.texture ? S_OK : E_OUTOFMEMORY;
}

void Frontend::create_committed_resource(Marshaler& m, Device& device,
                                         int variant) {
    // variant 0: CreateCommittedResource(pHeapProperties, HeapFlags, pDesc,
    //     InitialResourceState, pOptimizedClearValue, riid, ppvResource)
    // variant 1: CreateCommittedResource1(..., pOptimizedClearValue,
    //     pProtectedSession, riid, ppv)
    // variant 2: CreateCommittedResource2(props, flags, D3D12_RESOURCE_DESC1*,
    //     state, clear, pProtectedSession, riid, ppv)
    // variant 3: CreateCommittedResource3(props, flags, DESC1*, InitialLayout,
    //     clear, pProtectedSession, NumCastableFormats, pCastableFormats,
    //     riid, ppv)
    const GuestAddr props_address = m.arg_ptr(1);
    const u32 heap_flags = m.arg32(2);
    const GuestAddr desc_address = m.arg_ptr(3);
    const u32 initial_state = m.arg32(4);
    const usize session_index = 6;
    const usize riid_index = variant == 0 ? 6 : variant == 3 ? 9 : 7;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    const char* api = variant == 0   ? "ID3D12Device::CreateCommittedResource"
                      : variant == 1 ? "ID3D12Device4::CreateCommittedResource1"
                      : variant == 2 ? "ID3D12Device8::CreateCommittedResource2"
                                     : "ID3D12Device10::CreateCommittedResource3";
    if (variant == 3) {
        unimplemented(api, "enhanced-barrier initial layouts");
        m.set_return(E_INVALIDARG);
        return;
    }
    if (variant != 0 && m.arg_ptr(session_index) != 0) {
        unimplemented(api, "protected resource sessions");
        m.set_return(E_INVALIDARG);
        return;
    }
    HeapProperties props{};
    ResourceDescription desc{};
    if (!read_heap_properties(props_address, props) ||
        !read_resource_desc(desc_address, variant >= 2, desc)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (!heap_properties_valid(props, gpu_upload_heaps())) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (u32 error = validate_resource_desc(desc, props, initial_state,
                                           m.arg_ptr(5), true, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (u32 error = heap_flags_error(heap_flags, props, false, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (desc.dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        desc.mip_levels == 0) {
        desc.mip_levels = full_mip_count(
            desc.width, desc.height,
            desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? desc.depth_or_array_size
                : 1u);
    }
    // GetDesc reports the alignment the resource got (measured: 0 in, 65536
    // out for a buffer and a texture, d12core_facts_probe *_desc).
    if (desc.alignment == 0) {
        desc.alignment = desc.sample_count > 1
                             ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                             : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    auto resource = std::make_unique<Resource>();
    resource->device = device.address;
    resource->desc = desc;
    resource->committed = true;
    resource->heap_properties = props;
    if (resource->heap_properties.creation_node_mask == 0) {
        resource->heap_properties.creation_node_mask = 1;
    }
    if (resource->heap_properties.visible_node_mask == 0) {
        resource->heap_properties.visible_node_mask = 1;
    }
    // The implicit heap's category (measured for buffers: 0xC0,
    // ALLOW_ONLY_BUFFERS, d12core_facts_probe upload_heap_props), without
    // the creation-only flags (a02 desc "(NOT_ZEROED) GetHeapProperties").
    resource->heap_flags =
        (heap_flags & ~kCreationOnlyHeapFlags) |
        (desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER
             ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
             : (desc.flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0
                   ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                   : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
    resource->initial_state = initial_state;
    const bool cpu = cpu_visible(props);
    if (desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        resource->memory = allocate_memory(desc.width, cpu, true);
        if (!resource->memory) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        resource->offset = 0;
    } else if (u32 error = create_resource_backend(*resource, nullptr, 0, cpu);
               error != S_OK) {
        m.set_return(error);
        return;
    }
    m.set_return(return_new_object(std::move(resource), m.arg_ptr(riid_index),
                                   out));
}

void Frontend::create_placed_resource(Marshaler& m, Device& device,
                                      int variant) {
    // variant 0: CreatePlacedResource(ID3D12Heap *pHeap, UINT64 HeapOffset,
    //     const D3D12_RESOURCE_DESC *pDesc, InitialState,
    //     pOptimizedClearValue, riid, ppvResource)
    // variant 1: CreatePlacedResource1 (a D3D12_RESOURCE_DESC1)
    // variant 2: CreatePlacedResource2(heap, offset, DESC1, InitialLayout,
    //     clear, NumCastableFormats, pCastableFormats, riid, ppv)
    const GuestAddr heap_address = m.arg_ptr(1);
    usize next = 0;
    const u64 heap_offset = read_arg_u64(m, m_is_64bit, 2, next);
    const GuestAddr desc_address = m.arg_ptr(next);
    const u32 initial_state = m.arg32(next + 1);
    const usize riid_index = variant == 2 ? next + 5 : next + 3;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    if (variant == 2) {
        unimplemented("ID3D12Device10::CreatePlacedResource2",
                      "enhanced-barrier initial layouts");
        m.set_return(E_INVALIDARG);
        return;
    }
    Heap* heap = find<Heap>(heap_address, Kind::Heap);
    ResourceDescription desc{};
    if (heap == nullptr || !read_resource_desc(desc_address, variant >= 1, desc)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const char* api = variant == 0 ? "ID3D12Device::CreatePlacedResource"
                                   : "ID3D12Device8::CreatePlacedResource1";
    if (u32 error = validate_resource_desc(desc, heap->properties,
                                           initial_state, m.arg_ptr(next + 2),
                                           false, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (desc.dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        desc.mip_levels == 0) {
        desc.mip_levels = full_mip_count(
            desc.width, desc.height,
            desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? desc.depth_or_array_size
                : 1u);
    }
    // The resource category must be one the heap allows (Learn,
    // D3D12_HEAP_FLAGS).
    const bool is_buffer = desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const bool rt_ds = (desc.flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    if ((is_buffer && (heap->flags & D3D12_HEAP_FLAG_DENY_BUFFERS) != 0) ||
        (!is_buffer && rt_ds &&
         (heap->flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) != 0) ||
        (!is_buffer && !rt_ds &&
         (heap->flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) != 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Placement: the resource's alignment (64 KiB by default, 4 MiB for MSAA,
    // or the small alignments when asked for), and it must lie inside the
    // heap (measured: an offset of 1000 and one at the heap's end are
    // E_INVALIDARG, the last 64 KiB is fine, d12core_facts_probe placed_*).
    u64 alignment = desc.alignment;
    if (alignment == 0) {
        alignment = desc.sample_count > 1
                        ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    u64 size = round_up(desc.width, KB64);
    if (!is_buffer) {
        if (heap->memory->cpu_visible) {
            unimplemented("ID3D12Device::CreatePlacedResource",
                          "textures in CPU-visible heaps");
            m.set_return(E_INVALIDARG);
            return;
        }
        if ((heap->flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) {
            m.set_return(E_INVALIDARG);
            return;
        }
        // The backend's own placement size and alignment for the texture: the
        // same callback GetResourceAllocationInfo answers from (layout()).
        u64 backend_size = 0;
        u64 backend_alignment = 0;
        const bool placeable =
            texture_placement(desc, backend_size, backend_alignment);
        if (!placeable) {
            unimplemented("ID3D12Device::CreatePlacedResource",
                          "a texture the backend cannot place in a heap");
            m.set_return(E_INVALIDARG);
            return;
        }
        if (backend_alignment > alignment) {
            unimplemented("ID3D12Device::CreatePlacedResource",
                          "backend placement alignment above D3D12's");
            m.set_return(E_INVALIDARG);
            return;
        }
        size = round_up(backend_size, alignment);
    }
    if (heap_offset % alignment != 0 || heap_offset >= heap->size ||
        size > heap->size - heap_offset) {
        m.set_return(E_INVALIDARG);
        return;
    }
    desc.alignment = alignment;
    if (out == 0) {
        // Measured (a02 placed "ppvResource=NULL"): unlike the committed
        // and heap creators, no validation-only S_FALSE here.
        m.set_return(E_UNEXPECTED);
        return;
    }
    auto resource = std::make_unique<Resource>();
    resource->device = device.address;
    resource->desc = desc;
    resource->placed = true;
    // The resource keeps the heap's memory alive, but holds no COM reference
    // on the heap (measured: the heap's count is unchanged by placing,
    // d12core_facts_probe heap_addref_with_placed).
    resource->heap_object = heap->address;
    resource->heap_properties = heap->properties;
    resource->heap_flags = heap->flags;
    resource->initial_state = initial_state;
    resource->memory = heap->memory;
    resource->offset = heap_offset;
    if (!is_buffer) {
        if (u32 error = create_resource_backend(
                *resource, heap->memory->backend.get(), heap_offset, false);
            error != S_OK) {
            m.set_return(error);
            return;
        }
    }
    m.set_return(return_new_object(std::move(resource), m.arg_ptr(riid_index),
                                   out));
}

void Frontend::create_reserved_resource(Marshaler& m, Device& device,
                                        int variant) {
    // CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid,
    //     ppvResource); CreateReservedResource1 adds pProtectedSession;
    //     CreateReservedResource2 takes a layout and castable formats.
    (void)device;
    const usize out_index = variant == 0 ? 5 : variant == 1 ? 6 : 8;
    const GuestAddr out = m.arg_ptr(out_index);
    if (out != 0) (void)write_pointer(out, 0);
    // Reserved (tiled) resources are reported as
    // D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED (caps pass); Learn: creating
    // one on such a device fails.
    unimplemented("ID3D12Device::CreateReservedResource",
                  "tiled resources (tier NOT_SUPPORTED)");
    m.set_return(E_INVALIDARG);
}

// =============================================================================
// ID3D12Resource
// =============================================================================

void Frontend::resource_method(Marshaler& m, Resource& resource, usize slot) {
    switch (slot) {
    case ResourceSlot::Map: {
        // Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData).
        const u32 subresource = m.arg32(1);
        const GuestAddr data_out = m.arg_ptr(3);
        const bool cpu = resource.memory ? resource.memory->cpu_visible
                                         : cpu_visible(resource.heap_properties);
        // Measured: Map of a DEFAULT buffer is E_INVALIDARG and leaves
        // *ppData as it was; so is a subresource past the buffer's one
        // (d12core_facts_probe default_map, map_subresource1).
        if (!cpu || subresource >= resource.subresource_count()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        // Measured (a02 map "read range past end"): a read range ending past
        // the buffer fails and hands out no pointer; an inverted one is an
        // empty range and fine.
        if (const GuestAddr range = m.arg_ptr(2);
            range != 0 && resource.is_buffer()) {
            // D3D12_RANGE { SIZE_T Begin; SIZE_T End; }.
            GuestAddr begin = 0;
            GuestAddr end = 0;
            (void)read_pointer(range, begin);
            (void)read_pointer(range + (m_is_64bit ? 8u : 4u), end);
            if (end > begin && end > resource.desc.width) {
                if (data_out != 0) (void)write_pointer(data_out, 0);
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        if (!resource.is_buffer()) {
            // Learn (Map): a texture with the UNKNOWN layout cannot hand out
            // a pointer; mapping it with ppData null enables
            // Write/ReadFromSubresource.
            if (data_out != 0) {
                m.set_return(E_INVALIDARG);
                return;
            }
            ++resource.map_count;
            m.set_return(S_OK);
            return;
        }
        ++resource.map_count;
        if (data_out != 0) {
            // The persistent mapping: the same pointer for every Map, and
            // after Unmap (measured, d12core_facts_probe map_same_pointer /
            // map_again_same_pointer).
            (void)write_pointer(data_out,
                                resource.memory->guest + resource.offset);
        }
        m.set_return(S_OK);
        return;
    }
    case ResourceSlot::Unmap:
        // Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange). The
        // memory is coherent; an extra Unmap is harmless (measured: the
        // probe unmaps three times after two maps).
        if (resource.map_count > 0) --resource.map_count;
        m.set_return(0);
        return;
    case ResourceSlot::GetDesc: {
        // Hidden return pointer: This, RetVal.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) write_resource_desc(out, resource.desc, false);
        m.set_return(out);
        return;
    }
    case ResourceSlot::GetGPUVirtualAddress:
        // A texture has no VA (measured 0, d12core_facts_probe texture_va).
        m.set_return_i64(resource.gpu_va());
        return;
    case ResourceSlot::WriteToSubresource:
    case ResourceSlot::ReadFromSubresource: {
        // WriteToSubresource(DstSubresource, const D3D12_BOX *pDstBox,
        //     const void *pSrcData, SrcRowPitch, SrcDepthPitch);
        // ReadFromSubresource(void *pDstData, DstRowPitch, DstDepthPitch,
        //     SrcSubresource, const D3D12_BOX *pSrcBox).
        cpu_subresource_copy(m, resource,
                             slot == ResourceSlot::WriteToSubresource);
        return;
    }
    case ResourceSlot::GetHeapProperties: {
        // GetHeapProperties(D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS *).
        const GuestAddr props = m.arg_ptr(1);
        const GuestAddr flags = m.arg_ptr(2);
        if (resource.reserved) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (props != 0) write_heap_properties(props, resource.heap_properties);
        if (flags != 0) (void)m_memory.write_u32(flags, resource.heap_flags);
        m.set_return(S_OK);
        return;
    }
    case ResourceSlot::GetProtectedResourceSession: {
        // GetProtectedResourceSession(REFIID, void **). No resource here
        // has a session (measured, a02 desc: DXGI_ERROR_NOT_FOUND, NULL).
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)write_pointer(out, 0);
        m.set_return(DXGI_ERROR_NOT_FOUND);
        return;
    }
    case ResourceSlot::GetDesc1: {
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) write_resource_desc(out, resource.desc, true);
        m.set_return(out);
        return;
    }
    default:
        unimplemented("ID3D12Resource", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

}  // namespace vitreo::winapi::d3d12

namespace vitreo::winapi::d3d12 {
namespace {

[[nodiscard]] bool block_compressed(u32 format) {
    return (format >= DXGI_FORMAT_BC1_TYPELESS &&
            format <= DXGI_FORMAT_BC5_SNORM) ||
           (format >= DXGI_FORMAT_BC6H_TYPELESS &&
            format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

}  // namespace

void Frontend::cpu_subresource_copy(Marshaler& m, Resource& resource,
                                    bool write) {
    // WriteToSubresource(UINT DstSubresource, const D3D12_BOX *pDstBox,
    //     const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch);
    // ReadFromSubresource(void *pDstData, UINT DstRowPitch,
    //     UINT DstDepthPitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox).
    const u32 subresource = write ? m.arg32(1) : m.arg32(4);
    const GuestAddr box_address = write ? m.arg_ptr(2) : m.arg_ptr(5);
    const GuestAddr data = write ? m.arg_ptr(3) : m.arg_ptr(1);
    const u32 row_pitch = write ? m.arg32(4) : m.arg32(2);
    const u32 depth_pitch = write ? m.arg32(5) : m.arg32(3);
    const char* api = write ? "ID3D12Resource::WriteToSubresource"
                            : "ID3D12Resource::ReadFromSubresource";
    // Learn (WriteToSubresource): a texture in a CPU-accessible heap. Measured
    // (a02 map): no Map is needed (before Map and after Unmap both work); a
    // DEFAULT texture and a subresource out of range are E_INVALIDARG.
    const bool cpu = resource.memory ? resource.memory->cpu_visible
                                     : cpu_visible(resource.heap_properties);
    if (resource.is_buffer() || !cpu || data == 0 ||
        subresource >= resource.subresource_count()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const ResourceDescription& d = resource.desc;
    const u32 mips = std::max(1u, d.mip_levels);
    const u32 mip = subresource % mips;
    const u32 slice = (subresource / mips) % std::max(1u, resource.array_size());
    if (plane_count(d.format) != 1) {
        unimplemented(api, "multi-plane (depth-stencil / video) formats");
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 element = format_element_bytes(d.format);
    const bool bc = block_compressed(d.format);
    if (element == 0) {
        unimplemented(api, "a format with no element size");
        m.set_return(E_INVALIDARG);
        return;
    }
    const bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const u32 level_w = static_cast<u32>(std::max<u64>(1, d.width >> mip));
    const u32 level_h =
        d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
            ? 1u
            : std::max(1u, d.height >> mip);
    const u32 level_d = volume ? std::max(1u, d.depth_or_array_size >> mip) : 1u;
    u32 left = 0, top = 0, front = 0, right = level_w, bottom = level_h,
        back = level_d;
    if (box_address != 0) {
        // D3D12_BOX { left, top, front, right, bottom, back }.
        left = read_u32(box_address);
        top = read_u32(box_address + 4);
        front = read_u32(box_address + 8);
        right = read_u32(box_address + 12);
        bottom = read_u32(box_address + 16);
        back = read_u32(box_address + 20);
        if (right <= left || bottom <= top || back <= front) {
            m.set_return(S_OK);  // an empty box copies nothing
            return;
        }
        if (right > level_w || bottom > level_h || back > level_d ||
            (bc && ((left % 4) != 0 || (top % 4) != 0 ||
                    ((right % 4) != 0 && right != level_w) ||
                    ((bottom % 4) != 0 && bottom != level_h)))) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    const u32 width = right - left;
    const u32 height = bottom - top;
    const u32 depth = back - front;
    const u32 rows = bc ? (height + 3) / 4 : height;
    const usize row_bytes =
        static_cast<usize>(bc ? (width + 3) / 4 : width) * element;
    if (row_pitch < row_bytes ||
        (depth > 1 && depth_pitch < static_cast<u64>(row_pitch) * (rows - 1) +
                                        row_bytes)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Tightly packed host staging for one W slice at a time.
    std::vector<u8> staging(row_bytes * rows);
    if (write) {
        for (u32 z = 0; z < depth; ++z) {
            for (u32 y = 0; y < rows; ++y) {
                if (m_memory
                        .read_bytes(data + static_cast<u64>(z) * depth_pitch +
                                        static_cast<u64>(y) * row_pitch,
                                    staging.data() + y * row_bytes, row_bytes)
                        .is_err()) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
            }
            const bool ok =
                volume ? resource.texture3d != nullptr &&
                             resource.texture3d->update_region(
                                 mip, staging.data(),
                                 static_cast<unsigned>(row_bytes),
                                 static_cast<unsigned>(row_bytes * rows), left,
                                 top, front + z, width, height, 1)
                       : resource.texture != nullptr &&
                             resource.texture->update_region(
                                 mip, slice, staging.data(),
                                 static_cast<unsigned>(row_bytes), left, top,
                                 width, height);
            if (!ok) {
                unimplemented(api, "a texture the backend cannot write");
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        m.set_return(S_OK);
        return;
    }
    // Read: the whole level slice, then the box out of it.
    const u32 level_rows = bc ? (level_h + 3) / 4 : level_h;
    const usize level_row_bytes =
        static_cast<usize>(bc ? (level_w + 3) / 4 : level_w) * element;
    std::vector<u8> level(level_row_bytes * level_rows);
    const u32 first_row = bc ? top / 4 : top;
    const usize first_byte = static_cast<usize>(bc ? left / 4 : left) * element;
    for (u32 z = 0; z < depth; ++z) {
        const bool ok =
            volume ? resource.texture3d != nullptr &&
                         resource.texture3d->readback_w_slice(
                             mip, front + z, level.data(),
                             static_cast<unsigned>(level_row_bytes))
                   : resource.texture != nullptr &&
                         resource.texture->readback_slice(
                             mip, slice, level.data(),
                             static_cast<unsigned>(level_row_bytes));
        if (!ok) {
            unimplemented(api, "a texture the backend cannot read");
            m.set_return(E_INVALIDARG);
            return;
        }
        for (u32 y = 0; y < rows; ++y) {
            if (m_memory
                    .write_bytes(data + static_cast<u64>(z) * depth_pitch +
                                     static_cast<u64>(y) * row_pitch,
                                 level.data() +
                                     (first_row + y) * level_row_bytes +
                                     first_byte,
                                 row_bytes)
                    .is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
    }
    m.set_return(S_OK);
}

}  // namespace vitreo::winapi::d3d12
