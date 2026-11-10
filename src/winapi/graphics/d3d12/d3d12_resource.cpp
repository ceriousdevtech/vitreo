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
#include "graphics/contracts/format_422.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi::d3d12 {
namespace {

// Opt-in cache diagnostic, aggregated per replay thread once per second.
// Measures rebuilding only; the lookup hot path gets no additional timer.
void report_buffer_span_rebuild(u64 start, usize objects) {
    if (start == 0) return;
    struct Stats { u64 since{0}, count{0}, total{0}, maximum{0}; };
    static thread_local Stats stats;
    const u64 now = graphics::frame_profile::now_ns();
    const u64 elapsed = now - start;
    if (stats.since == 0) stats.since = start;
    ++stats.count;
    stats.total += elapsed;
    stats.maximum = std::max(stats.maximum, elapsed);
    if (now - stats.since >= 1000000000ull) {
        std::fprintf(stderr,
            "[d3d12-span-index] interval=%.3fs rebuilds=%llu rebuild_ms=%.3f max_ms=%.3f objects=%zu\n",
            static_cast<double>(now - stats.since) / 1e9,
            static_cast<unsigned long long>(stats.count),
            static_cast<double>(stats.total) / 1e6,
            static_cast<double>(stats.maximum) / 1e6, objects);
        stats = {now, 0, 0, 0};
    }
}

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
    // R8G8_B8G8 / G8R8_G8B8: decoded RGBA8 storage (format_422.hpp). D3D12
    // alone transcodes their uploads, copies and readbacks, so the shared
    // D3D11 table keeps them storage-less.
    if (dxgi_format == DXGI_FORMAT_R8G8_B8G8_UNORM) {
        return graphics::PixelFormat::RGBG422Unorm;
    }
    if (dxgi_format == DXGI_FORMAT_G8R8_G8B8_UNORM) {
        return graphics::PixelFormat::GRGB422Unorm;
    }
    return d3d11::texture_storage_format(dxgi_format);
}

FormatBlock format_block_info(u32 dxgi_format) {
    FormatBlock block{};
    if (dxgi_format == DXGI_FORMAT_R8G8_B8G8_UNORM ||
        dxgi_format == DXGI_FORMAT_G8R8_G8B8_UNORM) {
        block.width = 2;  // Learn DXGI_FORMAT: a 32-bit block per pixel pair
        block.bytes = 4;
        return block;
    }
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
                                                  bool buffers_only,
                                                  bool write_watch) {
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
        if (write_watch) {
            // Learn (D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH): GetWriteWatch works
            // on the heap's CPU mapping as on VirtualAlloc(MEM_WRITE_WATCH)
            // memory (measured, a12 write_watch: 4 KiB granularity, the
            // written pages, reset). The mapping becomes such an allocation.
            if (m_vmm == nullptr ||
                !m_vmm->adopt_mapped_region(base, bytes, true)) {
                unimplemented("D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH",
                              "no virtual memory manager to register the "
                              "heap mapping with");
                return nullptr;
            }
            memory->write_watched = true;
        }
    }
    // VAs are 64 KiB aligned and never reused, with a 64 KiB gap between
    // allocations so a VA one past a buffer never names the next one.
    memory->va = m_next_va;
    m_next_va += desc.size + KB64;
    m_va_ranges.emplace(memory->va, memory.get());
    return memory;
}

std::shared_ptr<Memory> Frontend::alias_memory(
    const std::shared_ptr<Memory>& source) {
    if (!source) return nullptr;
    std::shared_ptr<Memory> memory(new Memory, [this](Memory* retired) {
        retire_memory(retired);
        delete retired;
    });
    memory->backend = source->backend;
    memory->source = source->source ? source->source : source;
    memory->size = source->size;
    memory->cpu_visible = source->cpu_visible;
    memory->guest = source->guest;
    memory->guest_size = source->guest_size;
    memory->va = m_next_va;
    m_next_va += memory->size + KB64;
    m_va_ranges.emplace(memory->va, memory.get());
    return memory;
}

void Frontend::retire_memory(Memory* memory) {
    if (memory == nullptr) return;
    m_va_ranges.erase(memory->va);
    // Acceleration structures whose blobs lived in this memory go with it.
    for (auto it = m_acceleration_structures.lower_bound(memory->va);
         it != m_acceleration_structures.end() && it->first < memory->va + memory->size;) {
        it = m_acceleration_structures.erase(it);
        ++m_rt_generation;
    }
    // An alias's guest mapping is its source's.
    if (memory->guest != 0 && !memory->source && !m_tearing_down &&
        !memory->external_guest) {
        if (memory->write_watched && m_vmm != nullptr) {
            m_vmm->release_adopted_region(memory->guest);
        }
        (void)m_memory.unmap_region(memory->guest, memory->guest_size);
        memory->guest = 0;
    }
}

const ReservedVa* Frontend::reserved_span(u64 va) const {
    auto range = m_reserved_vas.upper_bound(va);
    if (range == m_reserved_vas.begin()) return nullptr;
    --range;
    const ReservedVa* span = range->second;
    if (span == nullptr || va < span->va || va - span->va >= span->bytes) {
        return nullptr;
    }
    return span;
}

bool Frontend::resolve_va(u64 va, u64 bytes, graphics::BackendBuffer*& buffer,
                          u64& offset) {
    buffer = nullptr;
    offset = 0;
    auto range = m_va_ranges.upper_bound(va);
    if (range != m_va_ranges.begin()) {
        --range;
        const Memory* memory = range->second;
        if (memory != nullptr && va >= memory->va &&
            va - memory->va <= memory->size &&
            bytes <= memory->size - (va - memory->va)) {
            buffer = memory->backend->buffer();
            offset = va - memory->va;
            return buffer != nullptr;
        }
    }
    if (const ReservedVa* span = reserved_span(va);
        span != nullptr && span->buffer != nullptr &&
        bytes <= span->bytes - (va - span->va)) {
        buffer = span->buffer;
        offset = va - span->va;
        return true;
    }
    return false;
}

u32 Frontend::buffer_words_from_va(u64 va) const {
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        graphics::frame_profile::add(graphics::frame_profile::Counter::RootCbvSizeScans);
    }
    if (const ReservedVa* span = reserved_span(va)) {
        if (va - span->va >= span->resource_bytes) return 0;
        return static_cast<u32>(std::min<u64>(
            (span->va + span->resource_bytes - va) / 4u, 0xFFFFFFFFu));
    }
    auto range = m_va_ranges.upper_bound(va);
    if (range == m_va_ranges.begin()) return 0;
    --range;
    const Memory* memory = range->second;
    if (memory == nullptr || va < memory->va ||
        va - memory->va >= memory->size) {
        return 0;
    }
    // The smallest end of a buffer resource of this memory whose span holds
    // @p va, else the memory's own end (aliased placements may overlap).
    const u64 generation = m_next_serial + m_objects_released;
    if (m_buffer_spans_generation != generation) {
        static const bool profile = [] {
            const char* value = std::getenv("VITREO_D3D12_SPAN_PROFILE");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        }();
        const u64 rebuild_start = profile ? graphics::frame_profile::now_ns() : 0;
        m_buffer_spans.clear();
        for (const auto& [address, object] : m_objects) {
            (void)address;
            if (object->kind != Kind::Resource) continue;
            const auto& resource = static_cast<const Resource&>(*object);
            if (!resource.is_buffer() || !resource.memory) continue;
            const Memory* owner = resource.memory.get();
            const u64 start = owner->va + resource.offset;
            if (resource.desc.width == 0 || start > ~u64{0} - resource.desc.width) {
                continue;
            }
            m_buffer_spans[owner].emplace_back(start, start + resource.desc.width);
        }
        m_buffer_spans_generation = generation;
        report_buffer_span_rebuild(rebuild_start, m_objects.size());
    }
    u64 end = memory->va + memory->size;
    if (const auto spans = m_buffer_spans.find(memory); spans != m_buffer_spans.end()) {
        for (const auto& [start, resource_end] : spans->second) {
            if (va >= start && va < resource_end && resource_end < end) end = resource_end;
        }
    }
    return static_cast<u32>(std::min<u64>((end - va) / 4u, 0xFFFFFFFFu));
}

// =============================================================================
// ID3D12Heap
// =============================================================================

void Frontend::create_heap(Marshaler& m, Device& device, int variant) {
    // CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppvHeap);
    // CreateHeap1 inserts ID3D12ProtectedResourceSession *pProtectedSession;
    // ID3D12CompatibilityDevice::CreateSharedHeap inserts
    // D3D12_COMPATIBILITY_SHARED_FLAGS CompatibilityFlags there.
    const bool heap1 = variant == 1;
    const bool compat = variant == 2;
    const GuestAddr desc = m.arg_ptr(1);
    const usize riid_index = variant != 0 ? 3 : 2;
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
    const char* api = compat ? "ID3D12CompatibilityDevice::CreateSharedHeap"
                      : heap1 ? "ID3D12Device4::CreateHeap1"
                              : "ID3D12Device::CreateHeap";
    if (u32 error = heap_flags_error(heap->flags, heap->properties, true, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (compat) {
        heap->compatibility_flags = m.arg32(2);
        if (u32 error = compatibility_flags_error(heap->compatibility_flags,
                                                  heap->flags, api);
            error != S_OK) {
            m.set_return(error);
            return;
        }
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
    heap->memory = allocate_memory(
        heap->size, cpu, buffers_only,
        (heap->flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) != 0);
    if (!heap->memory) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    heap->memory->upload = heap->properties.type == D3D12_HEAP_TYPE_UPLOAD;
    m.set_return(return_new_object(std::move(heap), m.arg_ptr(riid_index),
                                   out));
}

void Frontend::open_existing_heap(Marshaler& m, Device& device, bool sized) {
    // OpenExistingHeapFromAddress(const void *pAddress, REFIID riid,
    //     void **ppvHeap); OpenExistingHeapFromAddress1(pAddress, SIZE_T size,
    //     riid, ppvHeap). Measured (a01 misc, a12 existing_heap, WARP and
    //     NVIDIA): the base of a committed read-write VirtualAlloc region is
    //     a heap of that region's size -- CUSTOM, WRITE_BACK, L0, flags
    //     SHARED | SHARED_CROSS_ADAPTER | 0x400 -- whose placed buffers are
    //     that memory (Map is the address; GPU writes reach it). Any other
    //     address (inside a region, image data, NULL, read-only, reserved)
    //     is E_INVALIDARG with *ppv NULL.
    const char* api = sized ? "ID3D12Device13::OpenExistingHeapFromAddress1"
                            : "ID3D12Device3::OpenExistingHeapFromAddress";
    const GuestAddr address = m.arg_ptr(1);
    const usize riid_index = sized ? 3 : 2;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    VirtualAllocationQuery region{};
    if (address == 0 || m_vmm == nullptr ||
        !m_vmm->virtual_query(address, region) ||
        region.allocation_base != address || region.base_address != address ||
        region.state != 0x1000 /* MEM_COMMIT */ ||
        (region.protect & 0xFF) != 0x04 /* PAGE_READWRITE */) {
        m.set_return(E_INVALIDARG);
        return;
    }
    u64 size = region.region_size;
    if (sized) {
        const u64 asked = m.arg(2);
        if (asked == 0 || asked > size) {
            m.set_return(E_INVALIDARG);
            return;
        }
        size = asked;
    }
    open_existing_heap_over(m, device, api, address, size, riid_index, out, {});
}

void Frontend::open_existing_heap_from_mapping(Marshaler& m, Device& device) {
    // OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid,
    //     void **ppvHeap). Learn: a heap in system memory, CPU-accessible,
    //     that "wraps the entire VirtualAlloc region" -- here a read-write
    //     view of the whole section, which the heap holds until the GPU is
    //     done with it. Measured (a12 existing_heap, WARP and NVIDIA): a 64
    //     KiB section is a 65536-byte heap with the FromAddress properties
    //     (flags 0x421); a handle that is not a section is E_INVALIDARG with
    //     *ppv NULL.
    const char* api = "ID3D12Device3::OpenExistingHeapFromFileMapping";
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (!m_sections.map_whole_view || !m_sections.unmap_view) {
        unimplemented(api, "no section services");
        m.set_return(E_INVALIDARG);
        return;
    }
    u64 size = 0;
    const GuestAddr view = m_sections.map_whole_view(m.arg(1), size);
    if (view == 0 || size == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto unmap = m_sections.unmap_view;
    open_existing_heap_over(m, device, api, view, size, 2, out,
                            [unmap, view] { unmap(view); });
}

void Frontend::open_existing_heap_over(Marshaler& m, Device& device,
                                       const char* api, GuestAddr address,
                                       u64 size, usize riid_index,
                                       GuestAddr out,
                                       std::function<void()> release) {
    // Metal wraps host memory in whole host pages (16 KiB on Apple
    // silicon). The guest's allocation base is 64 KiB aligned (Windows
    // allocation granularity), so the host pages that hold a smaller region
    // lie inside that region's own 64 KiB granule, which no other
    // allocation can share: the GPU buffer takes them in, the heap and its
    // placed resources stay the region's size.
    // The backend may run the release on another thread, and may or may
    // not have run it when it fails: run it once.
    std::function<void()> once;
    if (release) {
        auto flag = std::make_shared<std::atomic<bool>>(false);
        once = [flag, release = std::move(release)] {
            if (!flag->exchange(true)) release();
        };
    }
    const u64 host_page = static_cast<u64>(::getpagesize());
    const u64 wrapped = round_up(size, host_page);
    if (address % host_page != 0 || wrapped > round_up(size, KB64)) {
        unimplemented(api, "a region the host pages cannot wrap");
        if (once) once();
        m.set_return(E_INVALIDARG);
        return;
    }
    // The guest bytes resolve as one host run; the host pages holding them
    // are mapped whole (host protection is per host page).
    Byte* host = m_memory.host_ptr_mut(address, static_cast<usize>(size));
    if (host != nullptr && reinterpret_cast<uintptr_t>(host) % host_page != 0) {
        host = nullptr;
    }
    graphics::BackendHeapDesc desc{};
    desc.size = wrapped;
    desc.cpu_visible = true;
    desc.buffers_only = true;
    desc.external_host = host;
    desc.external_release = once;
    auto backend = host != nullptr ? m_backend.create_heap(desc) : nullptr;
    if (!backend || !backend->valid() || backend->buffer() == nullptr) {
        backend.reset();
        unimplemented(api, "host memory the GPU cannot wrap");
        if (once) once();
        m.set_return(E_INVALIDARG);
        return;
    }
    std::shared_ptr<Memory> memory(new Memory, [this](Memory* retired) {
        retire_memory(retired);
        delete retired;
    });
    memory->size = size;
    memory->cpu_visible = true;
    memory->backend = std::move(backend);
    memory->guest = address;
    memory->guest_size = static_cast<usize>(size);
    memory->external_guest = true;
    memory->va = m_next_va;
    m_next_va += round_up(size, KB64) + KB64;
    m_va_ranges.emplace(memory->va, memory.get());
    auto heap = std::make_unique<Heap>();
    heap->device = device.address;
    heap->size = size;
    heap->alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap->properties.type = D3D12_HEAP_TYPE_CUSTOM;
    heap->properties.cpu_page_property = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap->properties.memory_pool_preference = D3D12_MEMORY_POOL_L0;
    heap->properties.creation_node_mask = 1;
    heap->properties.visible_node_mask = 1;
    heap->flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
                  D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS;
    heap->memory = std::move(memory);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(return_new_object(std::move(heap), m.arg_ptr(riid_index), out));
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
    // Measured (a04s resource, WARP and an RTX 4080): a SHARED heap or
    // committed resource cannot have CPU pages -- UPLOAD, READBACK and a
    // CUSTOM WRITE_BACK heap are all E_INVALIDARG.
    if ((flags & D3D12_HEAP_FLAG_SHARED) != 0 && cpu_visible(props)) {
        return E_INVALIDARG;
    }
    if (heap_object && (flags & kDenyHeapFlags) == kDenyHeapFlags) {
        return E_INVALIDARG;
    }
    if (!heap_object &&
        (flags & (kDenyHeapFlags | D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS)) != 0) {
        return E_INVALIDARG;
    }
    if ((flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) != 0 &&
        !cpu_visible(props)) {
        // Measured (a12 write_watch): DEFAULT and CUSTOM NOT_AVAILABLE are
        // E_INVALIDARG; UPLOAD, READBACK, CUSTOM WRITE_BACK / WRITE_COMBINE
        // are taken (allocate_memory registers the mapping).
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
        d.acceleration_structure =
            (d.flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE) != 0;
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
            // Windows accepts it: up to D3D12_REQ_MIP_LEVELS (15), the levels
            // past the chain are 1x1 (d3d12_a41_format_sweep_probe [create]
            // mips, WARP and RTX 4080: 4x4 with 4..15 levels S_OK, GetDesc
            // keeps the count). A Metal texture's levels stop at the full
            // chain, so those extra levels have no storage here.
            unimplemented(api, "MipLevels past the full mip chain (Windows "
                               "makes the extra levels 1x1; Metal has no "
                               "storage for them)");
            return E_INVALIDARG;
        }
        // A feedback map's storage is a buffer (create_feedback_map).
        if (!is_feedback_format(d.format) &&
            texture_format_for(d.format, d.flags) ==
            graphics::PixelFormat::Unknown) {
            // Windows makes these (d3d12_a41_format_sweep_probe [create]:
            // R32G32B32_* 5-8, R8G8_B8G8 / G8R8_G8B8 68-69, A4B4G4R4 191
            // and others); the backend has no texture storage for them.
            const std::string what =
                "a texture of DXGI format " + std::to_string(d.format) +
                ", which has no backend storage";
            unimplemented(api, what.c_str());
            return E_INVALIDARG;
        }
    }
    return S_OK;
}

u32 Frontend::layout_creation_error(const ResourceDescription& desc,
                                    u32 initial_layout, u32 count,
                                    GuestAddr formats, const char* api) {
    if (const char* why = initial_layout_violation(desc, initial_layout)) {
        d3d12_diag("%s: %s", api, why);
        return E_INVALIDARG;
    }
    if (count == 0 || formats == 0) return S_OK;
    CastableFormats castable;
    castable.formats.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        castable.formats.push_back(read_u32(formats + static_cast<u64>(i) * 4));
    }
    // A typeless resource can already be viewed as any member of its family,
    // so a list of such members asks for nothing new (Learn, "Format
    // casting"). Anything else is Relaxed Format Casting: measured on WARP
    // (a07 enhanced_create), same-size formats of other families pass and
    // another element size or a buffer fails -- the
    // GetResourceAllocationInfo3 rule. A device without
    // RelaxedFormatCastingSupported cannot view the resource that way, so
    // the description is not created.
    bool family_only = desc.dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                       texture_is_typeless(desc.format);
    for (const u32 f : castable.formats) {
        if (f == DXGI_FORMAT_UNKNOWN || !same_cast_family(f, desc.format)) {
            family_only = false;
        }
    }
    if (family_only) return S_OK;
    if (desc.dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !layout().relaxed_format_casting) {
        unimplemented(api, "castable formats outside the resource's family "
                           "(RelaxedFormatCastingSupported FALSE)");
        return E_INVALIDARG;
    }
    if (!castable_formats_ok(to_layout_desc(desc), castable, layout())) {
        d3d12_diag("%s: a castable format the resource cannot be cast to", api);
        return E_INVALIDARG;
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

u32 Frontend::create_feedback_map(Resource& resource,
                                  std::shared_ptr<Memory> memory, u64 offset) {
    namespace fb = graphics::sampler_feedback;
    const ResourceDescription& d = resource.desc;
    resource.feedback_map = true;
    resource.feedback = fb::geometry(static_cast<u32>(d.width), d.height,
                                     d.mip_levels, d.depth_or_array_size,
                                     d.mip_region[0], d.mip_region[1]);
    const u64 bytes = resource.feedback.total_words() * 4u;
    if (!memory) {
        memory = allocate_memory(bytes, false, false);
        if (!memory) return E_OUTOFMEMORY;
        offset = 0;
    } else if (offset + bytes > memory->size) {
        return E_INVALIDARG;
    }
    resource.memory = std::move(memory);
    resource.offset = offset;
    std::vector<u32> words(static_cast<usize>(resource.feedback.total_words()), 0u);
    fb::write_header(resource.feedback, d.format, words.data());
    graphics::BackendBuffer* buffer = resource.buffer();
    if (buffer == nullptr ||
        !buffer->update(static_cast<usize>(offset), words.data(), words.size() * 4u)) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
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
    // variant 4: ID3D12CompatibilityDevice::CreateSharedResource(props, flags,
    //     DESC*, state, clear, const D3D11_RESOURCE_FLAGS *pFlags11,
    //     CompatibilityFlags, pLifetimeTracker, pOwningSwapchain, riid, ppv)
    const GuestAddr props_address = m.arg_ptr(1);
    const u32 heap_flags = m.arg32(2);
    const GuestAddr desc_address = m.arg_ptr(3);
    const u32 initial_state = m.arg32(4);
    const usize session_index = 6;
    const usize riid_index = variant == 0   ? 6
                             : variant == 3 ? 9
                             : variant == 4 ? 10
                                            : 7;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    const char* api = variant == 0   ? "ID3D12Device::CreateCommittedResource"
                      : variant == 1 ? "ID3D12Device4::CreateCommittedResource1"
                      : variant == 2 ? "ID3D12Device8::CreateCommittedResource2"
                      : variant == 3 ? "ID3D12Device10::CreateCommittedResource3"
                                     : "ID3D12CompatibilityDevice::CreateSharedResource";
    if (variant != 0 && variant != 4 && m.arg_ptr(session_index) != 0) {
        unimplemented(api, "protected resource sessions");
        m.set_return(E_INVALIDARG);
        return;
    }
    if (variant == 4) {
        if (u32 error = compatibility_flags_error(m.arg32(7), heap_flags, api);
            error != S_OK) {
            m.set_return(error);
            return;
        }
        if (m.arg_ptr(6) != 0) {
            // D3D11_RESOURCE_FLAGS make the resource a D3D11 one as well
            // (bind, misc and CPU-access rules of D3D11): no D3D11 view of
            // a D3D12 resource exists here. Windows refuses the measured
            // case too (a01d compat: tex2d + BIND_SHADER_RESOURCE).
            unimplemented(api, "pFlags11 (D3D11 resource flags)");
            m.set_return(E_INVALIDARG);
            return;
        }
        if (m.arg_ptr(8) != 0 || m.arg_ptr(9) != 0) {
            unimplemented(api, "lifetime tracker / owning swap chain");
            m.set_return(E_NOTIMPL);
            return;
        }
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
    // Variant 3 takes an InitialLayout where the others take a state: the
    // state rules give way to the layout rules (layout_creation_error).
    if (u32 error = validate_resource_desc(desc, props, initial_state,
                                           m.arg_ptr(5), variant != 3, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (variant == 3) {
        if (u32 error = layout_creation_error(desc, initial_state, m.arg32(7),
                                              m.arg_ptr(8), api);
            error != S_OK) {
            m.set_return(error);
            return;
        }
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
    if (desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        device.next_allocation_va != 0) {
        // Measured (a01d tools): with ID3D12DeviceTools::
        // SetNextAllocationAddress pending, committed buffers are refused on
        // WARP and NVIDIA alike, whatever the address.
        d3d12_diag("%s: next allocation address 0x%llx pending -> "
                   "E_INVALIDARG",
                   api, static_cast<unsigned long long>(device.next_allocation_va));
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    auto resource = std::make_unique<Resource>();
    resource->device = device.address;
    resource->desc = desc;
    resource->committed = true;
    if (variant == 4) resource->compatibility_flags = m.arg32(7);
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
    // (Variant 3's argument is a layout, not a state.)
    resource->initial_state = variant == 3 ? D3D12_RESOURCE_STATE_COMMON
                                           : initial_state;
    const bool cpu = cpu_visible(props);
    if (desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        resource->memory = allocate_memory(
            desc.width, cpu, true,
            (heap_flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) != 0);
        if (!resource->memory) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        resource->memory->upload = props.type == D3D12_HEAP_TYPE_UPLOAD;
        resource->offset = 0;
    } else if (is_feedback_format(desc.format)) {
        if (u32 error = create_feedback_map(*resource, nullptr, 0); error != S_OK) {
            m.set_return(error);
            return;
        }
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
    Heap* heap = find<Heap>(heap_address, Kind::Heap);
    ResourceDescription desc{};
    if (heap == nullptr || !read_resource_desc(desc_address, variant >= 1, desc)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const char* api = variant == 0   ? "ID3D12Device::CreatePlacedResource"
                      : variant == 1 ? "ID3D12Device8::CreatePlacedResource1"
                                     : "ID3D12Device10::CreatePlacedResource2";
    // An existing heap (OpenExistingHeapFromAddress) is SHARED_CROSS_ADAPTER:
    // measured (a12 existing_heap), its buffers must be
    // ALLOW_CROSS_ADAPTER -- a plain buffer is E_INVALIDARG -- and such a
    // buffer is then an ordinary buffer over that memory.
    const bool existing = heap->memory && heap->memory->external_guest;
    const bool cross = (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
    if (existing && (desc.dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !cross)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (existing) desc.flags &= ~D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    const u32 validated = validate_resource_desc(desc, heap->properties,
                                                 initial_state,
                                                 m.arg_ptr(next + 2), false, api);
    if (existing) desc.flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (u32 error = validated; error != S_OK) {
        m.set_return(error);
        return;
    }
    if (variant == 2) {
        if (u32 error = layout_creation_error(desc, initial_state,
                                              m.arg32(next + 3),
                                              m.arg_ptr(next + 4), api);
            error != S_OK) {
            m.set_return(error);
            return;
        }
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
    u64 backend_offset = heap_offset;
    const bool feedback = is_feedback_format(desc.format);
    if (feedback) {
        // Opaque storage in the heap's memory (create_feedback_map), in
        // the texture category the heap must allow.
        if (heap->memory->cpu_visible ||
            (heap->flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
                D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto g = graphics::sampler_feedback::geometry(
            static_cast<u32>(desc.width), desc.height, desc.mip_levels,
            desc.depth_or_array_size, desc.mip_region[0], desc.mip_region[1]);
        size = round_up(g.total_words() * 4u, KB64);
    } else if (!is_buffer) {
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
        if (backend_alignment > alignment &&
            alignment <= D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
            // GetResourceAllocationInfo answered "not small" for this
            // description (texture_allocation); it cannot be placed at 4 KiB.
            m.set_return(E_INVALIDARG);
            return;
        }
        // A Metal alignment above D3D12's is met inside the same slack
        // range GetResourceAllocationInfo sized (texture_placement_size):
        // the texture goes at the next backend-aligned offset in it.
        size = texture_placement_size(backend_size, backend_alignment, alignment);
        backend_offset = placed_backend_offset(heap_offset, backend_alignment);
        if (size == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
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
    resource->initial_state = variant == 2 ? D3D12_RESOURCE_STATE_COMMON
                                           : initial_state;
    resource->memory = heap->memory;
    resource->offset = heap_offset;
    if (feedback) {
        if (u32 error = create_feedback_map(*resource, heap->memory, heap_offset);
            error != S_OK) {
            m.set_return(error);
            return;
        }
    } else if (!is_buffer) {
        if (u32 error = create_resource_backend(
                *resource, heap->memory->backend.get(), backend_offset, false);
            error != S_OK) {
            m.set_return(error);
            return;
        }
    }
    m.set_return(return_new_object(std::move(resource), m.arg_ptr(riid_index),
                                   out));
}

bool Frontend::build_reserved_tiles(Resource& resource, const char* api) {
    auto tiles = std::make_unique<ReservedTiles>();
    const ResourceDescription& desc = resource.desc;
    if (resource.is_buffer()) {
        tiles->buffer = true;
        tiles->tile_width = static_cast<u32>(KB64);
        tiles->tile_height = 1;
        tiles->mip_levels = 1;
        tiles->array_size = 1;
        tiles->standard_mips = 1;
        const u64 count = (desc.width + KB64 - 1) / KB64;
        if (count == 0 || count > 0xFFFFFFFEull) return false;
        tiles->tiles_per_slice = static_cast<u32>(count);
        tiles->total_tiles = tiles->tiles_per_slice;
        tiles->mip_first = {0};
        tiles->mip_tiles_x = {tiles->total_tiles};
        tiles->mip_tiles_y = {1};
        resource.tiles = std::move(tiles);
        return true;
    }
    graphics::BackendTiledLayout layout{};
    if (resource.texture == nullptr ||
        !m_backend.tiled_texture_layout(resource.texture.get(), layout) ||
        layout.tile_width == 0 || layout.tile_height == 0) {
        unimplemented(api, "backend tiled layout");
        return false;
    }
    tiles->tile_width = layout.tile_width;
    tiles->tile_height = layout.tile_height;
    tiles->mip_levels = std::max(1u, desc.mip_levels);
    tiles->array_size = std::max(1u, desc.depth_or_array_size);
    tiles->standard_mips = std::min(layout.first_packed_level, tiles->mip_levels);
    tiles->packed_tiles = tiles->standard_mips < tiles->mip_levels
                              ? layout.packed_tiles
                              : 0u;
    // Learn D3D12_TILED_RESOURCES_TIER_2: a mip at least one standard tile in
    // every dimension is never packed (D3D11.3 5.9.2.8.5), which also gives
    // tier 1's whole-multiple rule. The tile shape is the standard one (the
    // format's TILED support compares them). A backend that packs such a mip
    // cannot honour the reported tier.
    for (u32 mip = tiles->standard_mips; mip < tiles->mip_levels; ++mip) {
        const u64 w = std::max<u64>(1, desc.width >> mip);
        const u32 h = std::max(1u, desc.height >> mip);
        if (w >= tiles->tile_width && h >= tiles->tile_height) {
            unimplemented(api, "backend packs a mip of at least one standard tile");
            return false;
        }
    }
    // Tiers 1-3: an array's mips are whole tiles, never a packed tail (the
    // create rule above refuses the rest; tier 4 lifts it, D3D12 Tiled
    // Resource Tier 4 spec).
    if (tiles->array_size > 1 && tiles->packed_tiles != 0) {
        unimplemented(api, "tiled array whose backend packs mips");
        return false;
    }
    u64 first = 0;
    for (u32 mip = 0; mip < tiles->standard_mips; ++mip) {
        const u32 w = static_cast<u32>(std::max<u64>(1, desc.width >> mip));
        const u32 h = std::max(1u, desc.height >> mip);
        const u32 tx = (w + tiles->tile_width - 1) / tiles->tile_width;
        const u32 ty = (h + tiles->tile_height - 1) / tiles->tile_height;
        tiles->mip_first.push_back(static_cast<u32>(first));
        tiles->mip_tiles_x.push_back(tx);
        tiles->mip_tiles_y.push_back(ty);
        first += static_cast<u64>(tx) * ty;
    }
    first += tiles->packed_tiles;
    const u64 total = first * tiles->array_size;
    if (first == 0 || total > 0xFFFFFFFEull) return false;
    tiles->tiles_per_slice = static_cast<u32>(first);
    tiles->total_tiles = static_cast<u32>(total);
    resource.tiles = std::move(tiles);
    return true;
}

void Frontend::assign_reserved_va(Resource& resource) {
    // The VA is 64 KiB-aligned (m_next_va is) and covers every tile, with
    // the same gap allocate_memory leaves so a VA one past this buffer does
    // not name the next allocation. Measured (a02 reserved
    // GetGPUVirtualAddress): nonzero and 64 KiB-aligned.
    const u64 bytes = round_up(std::max<u64>(resource.desc.width, 1), KB64);
    auto span = std::shared_ptr<ReservedVa>(new ReservedVa, [this](ReservedVa* retired) {
        if (retired->va != 0) m_reserved_vas.erase(retired->va);
        delete retired;
    });
    span->va = m_next_va;
    span->bytes = bytes;
    span->resource_bytes = resource.desc.width;
    span->buffer = resource.tiled_buffer.get();
    if (resource.tiles) span->resident = resource.tiles->resident;
    m_next_va += bytes + KB64;
    m_reserved_vas.emplace(span->va, span.get());
    resource.reserved_va = std::move(span);
}

std::shared_ptr<graphics::BackendTilePool> Frontend::heap_tile_pool(Heap& heap) {
    if (heap.tile_pool) return heap.tile_pool;
    auto created = m_backend.create_tile_pool();
    if (!created) return nullptr;
    heap.tile_count = heap.size / KB64;
    created->set_size(heap.tile_count);
    heap.tile_pool = std::move(created);
    return heap.tile_pool;
}

void Frontend::create_reserved_resource(Marshaler& m, Device& device,
                                        int variant) {
    // CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid,
    //     ppvResource). CreateReservedResource1 adds pProtectedSession.
    // CreateReservedResource2 takes a barrier layout and castable formats.
    // Learn: virtual address space only, every tile NULL until
    // UpdateTileMappings. ppvResource null is S_FALSE when the description
    // is valid (capability test) and creates nothing.
    const char* api = variant == 0 ? "ID3D12Device::CreateReservedResource"
                      : variant == 1 ? "ID3D12Device4::CreateReservedResource1"
                                     : "ID3D12Device10::CreateReservedResource2";
    const usize out_index = variant == 0 ? 5 : variant == 1 ? 6 : 8;
    const GuestAddr out = m.arg_ptr(out_index);
    if (out != 0) (void)write_pointer(out, 0);
    if (!m_backend.supports_tiled_resources()) {
        // D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED: CreateReservedResource
        // cannot be used, not even for buffers.
        unimplemented(api, "tiled resources (tier NOT_SUPPORTED)");
        m.set_return(E_INVALIDARG);
        return;
    }
    // CreateReservedResource2(pDesc, InitialLayout, pOptimizedClearValue,
    //     pProtectedSession, NumCastableFormats, pCastableFormats, riid, ppv).
    if (variant != 0 && m.arg_ptr(4) != 0) {
        unimplemented(api, "protected resource sessions");
        m.set_return(E_INVALIDARG);
        return;
    }
    ResourceDescription desc{};
    if (!read_resource_desc(m.arg_ptr(1), false, desc)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 initial_state = m.arg32(2);
    const GuestAddr clear = m.arg_ptr(3);
    // Reserved resources live in GPU memory. The committed state and clear
    // rules apply; there is no upload or readback heap to check. Variant 2
    // takes a layout instead of a state.
    HeapProperties props{};
    props.type = D3D12_HEAP_TYPE_DEFAULT;
    if (u32 error = validate_resource_desc(desc, props, initial_state, clear,
                                           variant != 2, api);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    if (variant == 2) {
        if (u32 error = layout_creation_error(desc, initial_state, m.arg32(5),
                                              m.arg_ptr(6), api);
            error != S_OK) {
            m.set_return(error);
            return;
        }
    }
    const bool is_buffer = desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if (!is_buffer) {
        // Learn D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE: this is the
        // layout CreateReservedResource requires. Measured (a02 reserved):
        // UNKNOWN, ROW_MAJOR and 64KB_STANDARD_SWIZZLE are E_INVALIDARG;
        // Texture1D is E_INVALIDARG.
        if (desc.layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ||
            desc.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            if (desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                desc.layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE) {
                // Tier 3. a02's 3D reserved create succeeds on WARP (tier 3).
                unimplemented(api, "volume tiled resources (tiled resources tier 3)");
            }
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto& formats = caps().formats;
        const u32 samples = std::max(1u, desc.sample_count);
        const bool shape_ok =
            samples == 1
                ? desc.format < formats.tiled_format.size() &&
                      formats.tiled_format[desc.format]
                : samples == 4 && desc.format < formats.tiled_format_4x.size() &&
                      formats.tiled_format_4x[desc.format];
        if (!shape_ok) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (desc.mip_levels == 0) {
            desc.mip_levels = full_mip_count(desc.width, desc.height, 1u);
        }
        // Tier 1: an array only when every mip is a whole number of standard
        // tiles (D3D11.3 "Tiled resources with any mipmaps not a multiple of
        // standard tile size ... are not allowed to have an array size
        // larger than 1"; measured d3d11_tiled_probe [create]).
        if (desc.depth_or_array_size > 1) {
            u32 tile_w = 0;
            u32 tile_h = 0;
            if (!d3d11::standard_tile_shape(desc.format, samples, tile_w,
                                            tile_h) ||
                tile_w == 0 || tile_h == 0) {
                m.set_return(E_INVALIDARG);
                return;
            }
            for (u32 mip = 0; mip < desc.mip_levels; ++mip) {
                const u32 w = static_cast<u32>(std::max<u64>(1, desc.width >> mip));
                const u32 h = std::max(1u, desc.height >> mip);
                if (w % tile_w != 0 || h % tile_h != 0) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
            }
        }
    }
    // Measured (a02 reserved, including the 4x RT): alignment 0 is reported
    // back as 64 KiB, not the 4 MiB MSAA placement alignment.
    if (desc.alignment == 0) {
        desc.alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    auto resource = std::make_unique<Resource>();
    resource->device = device.address;
    resource->desc = desc;
    resource->reserved = true;
    resource->initial_state = variant == 2 ? D3D12_RESOURCE_STATE_COMMON : initial_state;
    if (is_buffer) {
        if (desc.width > static_cast<u64>(static_cast<usize>(-1))) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        graphics::BackendBufferDesc backend_desc{};
        backend_desc.byte_width = static_cast<usize>(desc.width);
        backend_desc.tiled = true;
        backend_desc.shader_resource =
            (desc.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
        backend_desc.unordered_access =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        resource->tiled_buffer = m_backend.create_buffer(backend_desc, nullptr);
        if (!resource->tiled_buffer || !resource->tiled_buffer->valid()) {
            // Measured (a02 "reserved buffer 64 GiB"): E_OUTOFMEMORY, no
            // object. A sparse buffer the backend cannot address fails the
            // same way.
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        if (!build_reserved_tiles(*resource, api)) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        assign_reserved_va(*resource);
    } else {
        resource->pixel_format = texture_format_for(desc.format, desc.flags);
        graphics::BackendTexture2DDesc backend_desc{};
        backend_desc.width = static_cast<unsigned>(desc.width);
        backend_desc.height = desc.height;
        backend_desc.mip_levels = desc.mip_levels;
        backend_desc.array_size = desc.depth_or_array_size;
        backend_desc.pixel_format = resource->pixel_format;
        backend_desc.pixel_format_view = texture_is_typeless(desc.format);
        backend_desc.shader_resource =
            (desc.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
        backend_desc.render_target =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        backend_desc.depth_stencil =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        backend_desc.unordered_access =
            (desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        backend_desc.sample_count = std::max(1u, desc.sample_count);
        backend_desc.tiled = true;
        // Tier 2: shaders read the tiles' residency (the status forms), so
        // the backend keeps the texture's residency companion.
        const bool tier2 = caps().tiled_resources_tier >= 2u;
        backend_desc.residency_companion = tier2;
        resource->texture = m_backend.create_texture_2d(backend_desc, nullptr, 0);
        if (!resource->texture || !resource->texture->valid()) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        if (tier2 && !resource->texture->has_residency_companion()) {
            // The reported tier promises the status of every reserved
            // texture the device creates.
            unimplemented(api, "a residency companion for this format's tiles");
            m.set_return(E_INVALIDARG);
            return;
        }
        if (!build_reserved_tiles(*resource, api)) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    m.set_return(return_new_object(std::move(resource), m.arg_ptr(out_index - 1),
                                   out));
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
        // Measured (a02 reserved Map): E_INVALIDARG and *ppData NULL.
        // A DEFAULT buffer's Map leaves *ppData untouched (a02 default_map).
        if (resource.reserved) {
            if (data_out != 0) (void)write_pointer(data_out, 0);
            m.set_return(E_INVALIDARG);
            return;
        }
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

/// Depth-stencil families CopyTextureRegion splits into planes (DXGI
/// R32G8X24 is 19-22, R24G8 is 44-47).
[[nodiscard]] bool depth_stencil_planes(u32 format) {
    return (format >= DXGI_FORMAT_R32G8X24_TYPELESS &&
            format <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) ||
           (format >= DXGI_FORMAT_R24G8_TYPELESS &&
            format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT);
}

[[nodiscard]] bool d24_depth_plane(u32 format) {
    return format >= DXGI_FORMAT_R24G8_TYPELESS &&
           format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT;
}

/// DXGI depth plane -> the backend's float32 depth. D24 is UNORM24 in the
/// low 24 bits (measured, a05 clear "D24S8": 0.5 -> 0x800000, nearest
/// even). D32 is the float bits unchanged. Same rule as CopyTextureRegion.
void depth_to_backend(u32 format, const u8* dxgi, u8* backend) {
    u32 stored = 0;
    std::memcpy(&stored, dxgi, 4);
    float depth = 0.0f;
    if (d24_depth_plane(format)) {
        depth = static_cast<float>(stored & 0xFFFFFFu) / 16777215.0f;
    } else {
        std::memcpy(&depth, &stored, 4);
    }
    std::memcpy(backend, &depth, 4);
}

void depth_from_backend(u32 format, const u8* backend, u8* dxgi) {
    float depth = 0.0f;
    std::memcpy(&depth, backend, 4);
    u32 stored = 0;
    if (d24_depth_plane(format)) {
        const float scaled = std::clamp(depth, 0.0f, 1.0f) * 16777215.0f;
        stored = static_cast<u32>(std::nearbyint(scaled));
    } else {
        std::memcpy(&stored, &depth, 4);
    }
    std::memcpy(dxgi, &stored, 4);
}

struct PlaneWindow {
    u32 left{0};
    u32 top{0};
    u32 width{0};
    u32 height{0};
};

enum class WindowStatus { Ok, Empty, Invalid };

/// A 2D plane's box. An empty box copies nothing (Learn). A box that does
/// not fit the plane is invalid. One slice deep: the array index is the
/// subresource, not D3D12_BOX::front.
[[nodiscard]] WindowStatus resolve_plane_window(GuestMemory& memory,
                                                GuestAddr box, u32 level_w,
                                                u32 level_h, PlaneWindow& out) {
    out = {};
    out.width = level_w;
    out.height = level_h;
    if (box == 0) return WindowStatus::Ok;
    const auto ru = [&](u32 offset) -> u32 {
        const auto value = memory.read_u32(box + offset);
        return value.is_ok() ? value.value() : 0u;
    };
    const u32 left = ru(0);
    const u32 top = ru(4);
    const u32 front = ru(8);
    const u32 right = ru(12);
    const u32 bottom = ru(16);
    const u32 back = ru(20);
    if (right <= left || bottom <= top || back <= front) return WindowStatus::Empty;
    if (front != 0 || back != 1 || right > level_w || bottom > level_h) {
        return WindowStatus::Invalid;
    }
    out.left = left;
    out.top = top;
    out.width = right - left;
    out.height = bottom - top;
    return WindowStatus::Ok;
}

void refuse_plane(Marshaler& m, const char* api, const char* what) {
    unimplemented(api, what);
    m.set_return(E_INVALIDARG);
}

void finish_recorded_gpu(graphics::GraphicsBackendDevice& backend) {
    graphics::frame_profile::WaitScope profile_wait("resource:finish-recorded-gpu");
    if (auto completion = backend.flush_completion()) {
        (void)completion->wait();
    } else {
        (void)backend.wait_idle();
    }
}

/// Depth or stencil of a combined depth-stencil texture. The backend
/// replaces one plane (update_depth_stencil_plane); a combined upload
/// would clobber the other.
void copy_depth_stencil_cpu(graphics::GraphicsBackendDevice& backend,
                            GuestMemory& memory, Marshaler& m,
                            Resource& resource, bool write, const char* api,
                            GuestAddr data, GuestAddr box, u32 row_pitch,
                            u32 mip, u32 slice, u32 plane) {
    if (resource.texture == nullptr || plane > 1 ||
        resource.desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        refuse_plane(m, api, "a depth-stencil plane the backend cannot address");
        return;
    }
    const bool stencil = plane == 1;
    const u32 texel = stencil ? 1u : 4u;
    const ResourceDescription& d = resource.desc;
    const u32 level_w = static_cast<u32>(std::max<u64>(1, d.width >> mip));
    const u32 level_h =
        d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
            ? 1u
            : std::max(1u, d.height >> mip);
    PlaneWindow window;
    switch (resolve_plane_window(memory, box, level_w, level_h, window)) {
    case WindowStatus::Empty:
        m.set_return(S_OK);
        return;
    case WindowStatus::Invalid:
        m.set_return(E_INVALIDARG);
        return;
    case WindowStatus::Ok:
        break;
    }
    const usize row_bytes = static_cast<usize>(window.width) * texel;
    if (row_pitch < row_bytes) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const usize level_row = static_cast<usize>(level_w) * texel;
    std::vector<u8> level(level_row * level_h);
    const bool covers = window.left == 0 && window.top == 0 &&
                        window.width == level_w && window.height == level_h;
    // The plane read/write is whole (readback_ / update_depth_stencil_plane).
    // A box copies through a CPU read-modify-write of this plane only.
    finish_recorded_gpu(backend);
    if (!write || !covers) {
        if (!resource.texture->readback_depth_stencil_plane(
                mip, slice, stencil, level.data(),
                static_cast<unsigned>(level_row))) {
            refuse_plane(m, api, stencil ? "a stencil plane the backend cannot read"
                                         : "a depth plane the backend cannot read");
            return;
        }
    }
    if (!write) {
        for (u32 y = 0; y < window.height; ++y) {
            std::vector<u8> row(row_bytes);
            for (u32 x = 0; x < window.width; ++x) {
                const u8* from = level.data() +
                                 ((static_cast<usize>(window.top + y) * level_w) +
                                  window.left + x) *
                                     texel;
                u8* to = row.data() + static_cast<usize>(x) * texel;
                if (stencil) {
                    *to = *from;
                } else {
                    depth_from_backend(d.format, from, to);
                }
            }
            if (memory
                    .write_bytes(data + static_cast<u64>(y) * row_pitch, row.data(),
                                 row_bytes)
                    .is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        m.set_return(S_OK);
        return;
    }
    std::vector<u8> tight(row_bytes * window.height);
    for (u32 y = 0; y < window.height; ++y) {
        if (memory
                .read_bytes(data + static_cast<u64>(y) * row_pitch,
                            tight.data() + static_cast<usize>(y) * row_bytes,
                            row_bytes)
                .is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    for (u32 y = 0; y < window.height; ++y) {
        for (u32 x = 0; x < window.width; ++x) {
            const u8* from = tight.data() + (static_cast<usize>(y) * window.width + x) * texel;
            u8* to = level.data() +
                     ((static_cast<usize>(window.top + y) * level_w) + window.left + x) *
                         texel;
            if (stencil) {
                *to = *from;
            } else {
                depth_to_backend(d.format, from, to);
            }
        }
    }
    if (!resource.texture->update_depth_stencil_plane(
            mip, slice, stencil, level.data(), static_cast<unsigned>(level_row))) {
        refuse_plane(m, api, stencil ? "a stencil plane the backend cannot write"
                                     : "a depth plane the backend cannot write");
        return;
    }
    m.set_return(S_OK);
}

/// One plane of NV12 / P010 / P016. Plane 0 is the full-size luma image
/// (Learn DXGI_FORMAT: Y as R8 or R16). Plane 1 is the half-size chroma
/// texture view_plane returns (R8G8 / R16G16). The planar texture's own
/// upload writes both planes, so luma goes through luma_plane(), which
/// addresses the Y texture alone. A format that cannot be split stays
/// refused.
void copy_planar_cpu(GuestMemory& memory, Marshaler& m, Resource& resource,
                     bool write, const char* api, GuestAddr data, GuestAddr box,
                     u32 row_pitch, u32 mip, u32 slice, u32 plane) {
    const char* missing = plane == 0 ? "a planar luma plane"
                                     : "a chroma plane the backend cannot address";
    if (resource.texture == nullptr || plane > 1 ||
        !graphics::is_planar_format(resource.texture->desc().pixel_format)) {
        refuse_plane(m, api, missing);
        return;
    }
    graphics::BackendTexture2D* image = nullptr;
    if (plane == 0) {
        image = resource.texture->luma_plane();
    } else {
        image = resource.texture->view_plane(graphics::planar_chroma_format(
            resource.texture->desc().pixel_format));
    }
    const u32 element =
        image == nullptr ? 0u
                         : graphics::format_table(image->desc().pixel_format)
                               .bytes_per_pixel;
    // view_plane returns the parent for a format that is not chroma, and
    // that parent's upload writes both planes. luma_plane() is null when
    // the backend cannot split them. Either one stays refused.
    if (image == nullptr || image == resource.texture.get() || element == 0) {
        refuse_plane(m, api, missing);
        return;
    }
    const u32 level_w = std::max(1u, image->desc().width >> mip);
    const u32 level_h = std::max(1u, image->desc().height >> mip);
    PlaneWindow window;
    switch (resolve_plane_window(memory, box, level_w, level_h, window)) {
    case WindowStatus::Empty:
        m.set_return(S_OK);
        return;
    case WindowStatus::Invalid:
        m.set_return(E_INVALIDARG);
        return;
    case WindowStatus::Ok:
        break;
    }
    const usize row_bytes = static_cast<usize>(window.width) * element;
    if (row_pitch < row_bytes) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (write) {
        std::vector<u8> tight(row_bytes * window.height);
        for (u32 y = 0; y < window.height; ++y) {
            if (memory
                    .read_bytes(data + static_cast<u64>(y) * row_pitch,
                                tight.data() + static_cast<usize>(y) * row_bytes,
                                row_bytes)
                    .is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        if (!image->update_region(mip, slice, tight.data(),
                                  static_cast<unsigned>(row_bytes), window.left,
                                  window.top, window.width, window.height)) {
            refuse_plane(m, api,
                         plane == 0 ? "a planar luma plane the backend cannot write"
                                    : "a chroma plane the backend cannot write");
            return;
        }
        m.set_return(S_OK);
        return;
    }
    const usize level_row = static_cast<usize>(level_w) * element;
    std::vector<u8> level(level_row * level_h);
    if (!image->readback_slice(mip, slice, level.data(),
                               static_cast<unsigned>(level_row))) {
        refuse_plane(m, api,
                     plane == 0 ? "a planar luma plane the backend cannot read"
                                : "a chroma plane the backend cannot read");
        return;
    }
    const usize first = static_cast<usize>(window.left) * element;
    for (u32 y = 0; y < window.height; ++y) {
        if (memory
                .write_bytes(data + static_cast<u64>(y) * row_pitch,
                             level.data() +
                                 (static_cast<usize>(window.top + y) * level_row) +
                                 first,
                             row_bytes)
                .is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    m.set_return(S_OK);
}

/// True when this call was answered (success or a loud refusal).
[[nodiscard]] bool copy_addressable_plane(graphics::GraphicsBackendDevice& backend,
                                          GuestMemory& memory, Marshaler& m,
                                          Resource& resource, bool write,
                                          const char* api, GuestAddr data,
                                          GuestAddr box, u32 row_pitch, u32 mip,
                                          u32 slice, u32 plane) {
    if (depth_stencil_planes(resource.desc.format)) {
        copy_depth_stencil_cpu(backend, memory, m, resource, write, api, data,
                               box, row_pitch, mip, slice, plane);
        return true;
    }
    const graphics::PixelFormat storage =
        resource.texture != nullptr ? resource.texture->desc().pixel_format
                                    : resource.pixel_format;
    if (graphics::is_planar_format(storage) ||
        graphics::is_planar_format(resource.pixel_format)) {
        copy_planar_cpu(memory, m, resource, write, api, data, box, row_pitch,
                        mip, slice, plane);
        return true;
    }
    return false;
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
    const u32 slices = std::max(1u, resource.array_size());
    // Learn (Subresources): MipSlice + ArraySlice * MipLevels +
    // PlaneSlice * MipLevels * ArraySize. The same split CopyTextureRegion uses.
    const u32 mip = subresource % mips;
    const u32 slice = (subresource / mips) % slices;
    const u32 plane = subresource / (mips * slices);
    if (plane_count(d.format) != 1) {
        // Depth, stencil, and each plane of NV12/P010/P016 go through the
        // plane read/write. Anything the backend cannot split stays refused.
        if (!copy_addressable_plane(m_backend, m_memory, m, resource, write, api,
                                    data, box_address, row_pitch, mip, slice,
                                    plane)) {
            unimplemented(api, "multi-plane (depth-stencil / video) formats");
            m.set_return(E_INVALIDARG);
        }
        return;
    }
    const u32 element = format_element_bytes(d.format);
    const bool bc = block_compressed(d.format);
    // R8G8_B8G8 / G8R8_G8B8: 2x1 blocks of 4 bytes, held as decoded RGBA8
    // texels (graphics::format_422.hpp); the guest side is blocks.
    const graphics::PixelFormat pair_storage =
        resource.texture != nullptr ? resource.texture->desc().pixel_format
        : resource.texture3d != nullptr
            ? resource.texture3d->desc().pixel_format
            : graphics::PixelFormat::Unknown;
    const bool pair = graphics::is_422_storage(pair_storage);
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
                    ((bottom % 4) != 0 && bottom != level_h))) ||
            (pair && ((left % 2) != 0 ||
                      ((right % 2) != 0 && right != level_w)))) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    const u32 width = right - left;
    const u32 height = bottom - top;
    const u32 depth = back - front;
    const u32 rows = bc ? (height + 3) / 4 : height;
    const usize row_bytes =
        static_cast<usize>(bc     ? (width + 3) / 4
                           : pair ? (width + 1) / 2
                                  : width) *
        element;
    // The storage rows of a decoded 4:2:2 box: one RGBA8 texel per pixel.
    const usize texel_row_bytes = static_cast<usize>(width) * 4u;
    std::vector<u8> texels(pair ? texel_row_bytes * height : 0u);
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
            const u8* upload = staging.data();
            usize upload_row = row_bytes;
            if (pair) {
                graphics::decode_422_rows(pair_storage, staging.data(),
                                          row_bytes, texels.data(),
                                          texel_row_bytes, width, rows);
                upload = texels.data();
                upload_row = texel_row_bytes;
            }
            const bool ok =
                volume ? resource.texture3d != nullptr &&
                             resource.texture3d->update_region(
                                 mip, upload,
                                 static_cast<unsigned>(upload_row),
                                 static_cast<unsigned>(upload_row * rows), left,
                                 top, front + z, width, height, 1)
                       : resource.texture != nullptr &&
                             resource.texture->update_region(
                                 mip, slice, upload,
                                 static_cast<unsigned>(upload_row), left, top,
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
        pair ? static_cast<usize>(level_w) * 4u
             : static_cast<usize>(bc ? (level_w + 3) / 4 : level_w) * element;
    std::vector<u8> level(level_row_bytes * level_rows);
    const u32 first_row = bc ? top / 4 : top;
    const usize first_byte =
        pair ? static_cast<usize>(left) * 4u
             : static_cast<usize>(bc ? left / 4 : left) * element;
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
            const u8* row = level.data() + (first_row + y) * level_row_bytes +
                            first_byte;
            if (pair) {
                graphics::encode_422_rows(pair_storage, row, level_row_bytes,
                                          staging.data(), row_bytes, width, 1);
                row = staging.data();
            }
            if (m_memory
                    .write_bytes(data + static_cast<u64>(z) * depth_pitch +
                                     static_cast<u64>(y) * row_pitch,
                                 row, row_bytes)
                    .is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
    }
    m.set_return(S_OK);
}

}  // namespace vitreo::winapi::d3d12
