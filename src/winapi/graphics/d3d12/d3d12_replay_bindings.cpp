// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_bindings.cpp
/// @brief Root-signature and descriptor resolution for a draw or
///        dispatch, and the root-constant upload ring.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include <vitreo/ab_cycle.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <tuple>

namespace vitreo::winapi::d3d12 {

namespace {

[[nodiscard]] u32 set_of_range(u32 range_type) {
    switch (range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return SET_SRV;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return SET_UAV;
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return SET_CBV;
    default: return SET_SAMPLER;
    }
}

[[nodiscard]] bool visible_to(u32 visibility, u32 stage) {
    if (stage == STAGE_COMPUTE) return true;  // compute sees every parameter
    if (visibility == D3D12_SHADER_VISIBILITY_ALL) return true;
    switch (stage) {
    case STAGE_VERTEX: return visibility == D3D12_SHADER_VISIBILITY_VERTEX;
    case STAGE_AMPLIFICATION: return visibility == D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    case STAGE_MESH: return visibility == D3D12_SHADER_VISIBILITY_MESH;
    default: return visibility == D3D12_SHADER_VISIBILITY_PIXEL;
    }
}

[[nodiscard]] D3DSamplerFields sampler_fields(const SamplerState& s) {
    D3DSamplerFields fields{};
    fields.filter = s.filter;
    fields.address_u = s.address_u;
    fields.address_v = s.address_v;
    fields.address_w = s.address_w;
    fields.mip_lod_bias = s.mip_lod_bias;
    fields.max_anisotropy = s.max_anisotropy;
    fields.comparison_func = s.comparison_func;
    for (usize c = 0; c < 4; ++c) {
        std::memcpy(&fields.border[c], &s.border[c], 4);
    }
    fields.min_lod = s.min_lod;
    fields.max_lod = s.max_lod;
    fields.flags = s.flags;
    return fields;
}

}  // namespace

graphics::BackendDescriptorTable* Replayer::table(
    u32 stage, u32 set, u32 slots, graphics::BackendDescriptorClass klass) {
    auto& entry = rr.tables[stage][set];
    if (!entry || entry->slot_count() < slots) {
        graphics::BackendDescriptorTableDesc desc{};
        desc.slot_count = std::max(slots, entry ? entry->slot_count() : 0u);
        desc.slot_class = klass;
        entry = f.m_backend.create_descriptor_table(desc);
    }
    return entry.get();
}

bool Replayer::upload(const void* data, usize bytes,
                      graphics::BackendBuffer*& buffer, u64& offset) {
    u8* host = nullptr;
    if (!reserve(bytes, buffer, offset, host)) return false;
    std::memcpy(host, data, bytes);
    return true;
}

bool Replayer::reserve(usize bytes, graphics::BackendBuffer*& buffer,
                       u64& offset, u8*& out) {
    const u64 size =
        (bytes + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) /
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT *
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    if (size > CONSTANT_CHUNK_BYTES) return false;
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        graphics::frame_profile::add(graphics::frame_profile::Counter::UploadBytes, size);
    }
    auto& current = rr.current;
    if (!current.heap || current.used + size > CONSTANT_CHUNK_BYTES) {
        if (current.heap) {
            current.retire_generation = f.m_backend.recording_generation();
            rr.retired.push_back(std::move(current));
            current = {};
        }
        for (auto it = rr.retired.begin(); it != rr.retired.end(); ++it) {
            if (f.m_backend.generation_complete(it->retire_generation)) {
                current = std::move(*it);
                current.used = 0;
                rr.retired.erase(it);
                break;
            }
        }
        if (!current.heap) {
            graphics::BackendHeapDesc desc{};
            desc.size = CONSTANT_CHUNK_BYTES;
            desc.cpu_visible = true;
            desc.buffers_only = true;
            current.heap = f.m_backend.create_heap(desc);
            current.used = 0;
            if (graphics::frame_profile::enabled()) [[unlikely]] {
                graphics::frame_profile::add(
                    graphics::frame_profile::Counter::UploadChunks);
            }
            if (!current.heap) return false;
        }
    }
    auto* host = static_cast<u8*>(current.heap->host_pointer());
    if (host == nullptr) return false;
    if (size > bytes) std::memset(host + current.used + bytes, 0, size - bytes);
    out = host + current.used;
    buffer = current.heap->buffer();
    offset = current.used;
    current.used += size;
    return true;
}

namespace {
/// An SRV's Shader4ComponentMapping for the backend: 0 for the identity
/// (D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING), else the D3D12 encoding.
[[nodiscard]] u16 component_mapping(const Descriptor& d) {
    const u32 mapping = d.component_mapping & 0x1FFFu;
    return mapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING || mapping == 0
               ? u16{0}
               : static_cast<u16>(mapping | 0x1000u);
}
}  // namespace

u32 Replayer::bind_view(graphics::BackendDescriptorTable& t, unsigned slot,
                        const Descriptor& d, bool uav, bool& bound,
                        bool uint_view) {
    bound = false;
    if (!uav && d.dimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
        bound = bind_acceleration_structure(t, slot, d.first_element);
        return 0;
    }
    // Diagnose the SRV clamp independently of the sampler's MinLOD. This
    // observes every replayed texture view; it does not select a workload or
    // change sampling. Bound output keeps the opt-in trace usable in motion.
    static const bool trace_srv_lod = [] {
        const char* value = std::getenv("VITREO_D3D12_TRACE_SRV_LOD");
        return value != nullptr && value[0] == '1';
    }();
    if (trace_srv_lod && !uav && d.dimension != D3D12_SRV_DIMENSION_BUFFER) {
        static std::atomic<bool> announced{false};
        static std::atomic<u32> nonzero{0};
        if (!announced.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[d3d12-srv-lod] texture view trace active; "
                                 "reporting nonzero ResourceMinLODClamp requests\n");
        }
        if (d.min_lod != 0.0f &&
            nonzero.fetch_add(1, std::memory_order_relaxed) < 16u) {
            std::fprintf(stderr, "[d3d12-srv-lod] slot=%u dimension=%u "
                                 "min_lod=%.9g first_mip=%u mip_levels=%u\n",
                         slot, d.dimension, static_cast<double>(d.min_lod),
                         d.most_detailed_mip, d.mip_levels);
        }
    }
    // The format the view is made as: an R32_FLOAT UAV the shader runs
    // atomics on is read and written through an R32_UINT view of the same
    // bits (DxbcTranslationOptions::typed_uav_uint_views).
    const u32 view_dxgi = uav && uint_view && d.format == DXGI_FORMAT_R32_FLOAT
                              ? DXGI_FORMAT_R32_UINT
                              : d.format;
    Resource* r = resource(d.resource, d.resource_serial);
    if (r == nullptr) {
        // A null descriptor, or one whose resource is gone: reads 0.
        t.clear_slot(slot);
        return 0;
    }
    // A sampler feedback map's UAV is a raw view of its opaque storage
    // (CreateSamplerFeedbackUnorderedAccessView).
    if (r->is_buffer() || r->feedback_map) {
        graphics::BackendBuffer* buffer = r->buffer();
        const bool is_buffer_view =
            uav ? d.dimension == D3D12_UAV_DIMENSION_BUFFER
                : d.dimension == D3D12_SRV_DIMENSION_BUFFER;
        if (buffer == nullptr || !is_buffer_view) {
            t.clear_slot(slot);
            return 0;
        }
        const bool raw = (d.flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
        static_assert(D3D12_BUFFER_SRV_FLAG_RAW == D3D12_BUFFER_UAV_FLAG_RAW);
        if (!raw && d.structure_stride == 0) {
            // A typed buffer view: a texture buffer over the element window,
            // decoded by the view's format.
            // Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): FirstElement is a
            // UINT64 element index. The byte offset is the resource's
            // placement plus FirstElement times the element size. A product
            // that does not fit in 64 bits must not wrap onto a low offset.
            // A format with no texture storage here (A4B4G4R4) is read as
            // raw memory only.
            const u32 texel = dxgi_element_bytes(d.format);
            const u32 element =
                texel != 0 ? texel : typed_raw_element_bytes(d.format);
            if (element == 0 || r->offset % element != 0) {
                unimplemented("ExecuteCommandLists",
                              "a typed buffer view of this format");
                t.clear_slot(slot);
                return 0;
            }
            if (d.first_element > (~u64{0}) / element ||
                d.first_element * static_cast<u64>(element) >
                    ~u64{0} - r->offset) {
                unimplemented("ExecuteCommandLists",
                              "a typed buffer view whose first element does "
                              "not fit the backend's element index");
                t.clear_slot(slot);
                return 0;
            }
            const u64 first = r->offset / element + d.first_element;
            const u16 mapping = uav ? u16{0} : component_mapping(d);
            // The view read as raw memory (graphics::kTypedBufferRaw): the
            // shader reads the element bytes through the buffer's device
            // address and converts the format itself. Used when the
            // backend cannot alias the storage as a texture: a reserved
            // buffer (Metal makes no texture of a placement sparse buffer),
            // or a view whose texture the backend refused.
            // The shader applies an SRV's Shader4ComponentMapping and runs
            // typed UAV atomics on the element words itself
            // (dxbc_to_spirv_typed_raw.cpp).
            const auto bind_raw = [&]() -> u32 {
                const u64 byte = first * static_cast<u64>(element);
                const u64 base = buffer->device_address();
                if (base == 0 || byte > ~u64{0} - base ||
                    !typed_raw_layout(
                        d.format, base + byte, d.num_elements,
                        mapping != 0 ? u32{mapping}
                                     : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                        m_typed_raw_words)) {
                    unimplemented("ExecuteCommandLists",
                                  "a typed buffer view of this format read "
                                  "as raw memory");
                    t.clear_slot(slot);
                    return 0;
                }
                // The slot holds the buffer: that is what makes it resident
                // and orders it against copies. The shader does not read
                // the slot itself for a raw view.
                bound = t.set_buffer(slot, buffer, byte);
                if (!bound) {
                    t.clear_slot(slot);
                    return 0;
                }
                return graphics::kTypedBufferRaw |
                       (d3d11::dxgi_format_has_alpha(d.format)
                            ? 0u
                            : graphics::kTypedBufferAlphaMissing);
            };
            if (r->reserved || texel == 0) return bind_raw();
            const auto key = std::make_tuple(view_dxgi, first, d.num_elements,
                                             uav, mapping);
            auto found = r->typed_views.find(key);
            if (found == r->typed_views.end()) {
                auto view =
                    mapping != 0
                        ? f.m_backend.create_mapped_texture_buffer(
                              *buffer, graphics::pixel_format_from_dxgi(d.format),
                              first, d.num_elements, mapping)
                        : f.m_backend.create_texture_buffer(
                              *buffer, graphics::pixel_format_from_dxgi(view_dxgi),
                              first, d.num_elements, uav);
                if (!view) {
                    // The backend made no texture of this view (a format
                    // it has no texture buffer for, an offset its texture
                    // cannot name, or a mapping it cannot apply): read it
                    // as raw memory, which applies the mapping itself.
                    return bind_raw();
                }
                found = r->typed_views.emplace(key, std::move(view)).first;
            }
            // The texture starts element_bias() elements below FirstElement
            // when that element is not on the backend's alignment. The
            // shader adds the bias (push block for a non-table shader model
            // 5.0 translation, typed_buffer_bias_buffer for a descriptor
            // table). A bias that does not fit the packed entry cannot be
            // reported, so the slot stays empty.
            const u32 bias = found->second->element_bias();
            if (bias > graphics::kTypedBufferBiasMask) {
                unimplemented("ExecuteCommandLists",
                              "a typed buffer view whose element bias does "
                              "not fit the size-table entry");
                t.clear_slot(slot);
                return 0;
            }
            bound = t.set_texture_buffer(slot, found->second.get());
            if (!bound) {
                t.clear_slot(slot);
                return 0;
            }
            // The size-table entry: the bias, and kTypedBufferAlphaMissing
            // when the format has no alpha so a load past the view returns
            // alpha 1 (D3D11.3 19.1.3.3). The caller stores it in the push
            // block (shader model 5.0) and in the per-slot bias buffer.
            return (bias & graphics::kTypedBufferBiasMask) |
                   (d3d11::dxgi_format_has_alpha(d.format)
                        ? 0u
                        : graphics::kTypedBufferAlphaMissing);
        }
        const u64 stride = raw ? 4u : d.structure_stride;
        // FirstElement is a UINT64 element index (Learn, D3D12_BUFFER_SRV /
        // D3D12_BUFFER_UAV). The byte offset is the resource's placement
        // plus FirstElement times the element stride: 4 for a raw view,
        // StructureByteStride for a structured view. A product that does
        // not fit in 64 bits must not wrap onto a low offset.
        if (stride == 0 ||
            d.first_element > (~u64{0} - r->offset) / stride) {
            unimplemented("ExecuteCommandLists",
                          "a buffer view whose byte offset does not fit in "
                          "64 bits");
            t.clear_slot(slot);
            return 0;
        }
        const u64 offset = r->offset + d.first_element * stride;
        bound = t.set_buffer(slot, buffer, offset);
        if (!bound) {
            // Pointer-width Metal offsets hold this on arm64. A backend
            // whose offset is narrower cannot name the byte; the slot
            // stays empty.
            if (offset > 0xFFFFFFFFull) {
                unimplemented("ExecuteCommandLists",
                              "a buffer view more than 4 GiB into its heap");
            }
            t.clear_slot(slot);
            return 0;
        }
        const u64 bytes = static_cast<u64>(d.num_elements) * stride;
        return static_cast<u32>(std::min<u64>(bytes / 4u, 0xFFFFFFFFull));
    }
    const u16 mapping = uav ? u16{0} : component_mapping(d);
    const graphics::PixelFormat view_format =
        graphics::pixel_format_from_dxgi(view_dxgi);
    if (r->texture3d) {
        const graphics::PixelFormat stored =
            view_dxgi == r->desc.format ? graphics::PixelFormat::Unknown
                                        : view_format;
        const unsigned mips = uav ? 1u : d.mip_levels;
        u16 mapping3d = mapping;
        if (!uav &&
            d3d11::view_swizzle_for(d.format, false) ==
                d3d11::ViewSwizzle::OpaqueAlpha) {
            // B8G8R8X8 is stored as B8G8R8A8: its alpha reads 1 (DXGI_FORMAT
            // "X8 ... unused"). Composed into the view's own mapping, every
            // component that selects alpha takes FORCE_VALUE_1 instead.
            u32 m = mapping3d == 0 ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
                                   : mapping3d;
            for (u32 c = 0; c < 4; ++c) {
                if (((m >> (c * 3u)) & 7u) == 3u) {
                    m = (m & ~(7u << (c * 3u))) | (5u << (c * 3u));
                }
            }
            mapping3d = static_cast<u16>((m & 0xFFFu) | 0x1000u);
        } else if (!uav && d3d11::view_swizzle_for(d.format, false) ==
                               d3d11::ViewSwizzle::Bgra4) {
            // B4G4R4A4 is stored as Metal ABGR4: DXGI R, G, B, A are the
            // storage's G, B, A, R (graphics::BackendViewSwizzle::GBAR).
            // Composed into the view's mapping, each component that selects
            // a DXGI component selects its storage component instead.
            constexpr u32 kStorageOf[4] = {1u, 2u, 3u, 0u};
            u32 m = mapping3d == 0 ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
                                   : mapping3d;
            for (u32 c = 0; c < 4; ++c) {
                const u32 sel = (m >> (c * 3u)) & 7u;
                if (sel < 4u) {
                    m = (m & ~(7u << (c * 3u))) | (kStorageOf[sel] << (c * 3u));
                }
            }
            mapping3d = static_cast<u16>((m & 0xFFFu) | 0x1000u);
        }
        // UAV descriptions have no Shader4ComponentMapping; `mapping` is 0.
        // An identity SRV uses view() unchanged. A non-identity SRV asks the
        // backend to swizzle, and stays unbound when it cannot.
        auto* volume =
            mapping3d == 0
                ? r->texture3d->view(stored, d.most_detailed_mip, mips)
                : r->texture3d->view(stored, d.most_detailed_mip, mips,
                                     mapping3d);
        if (volume == nullptr) {
            if (mapping3d != 0) {
                unimplemented("ExecuteCommandLists",
                              "a Shader4ComponentMapping the backend cannot "
                              "apply to a 3D texture view");
            }
            t.clear_slot(slot);
            return 0;
        }
        bound = t.set_texture_3d(slot, volume);
        if (!bound) t.clear_slot(slot);
        return 0;
    }
    if (!r->texture) {
        t.clear_slot(slot);
        return 0;
    }
    graphics::BackendTextureViewDesc view{};
    using Dim = graphics::BackendTextureViewDimension;
    switch (d.dimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        view.dimension = Dim::Texture2D;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        view.dimension = Dim::Texture2DArray;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        view.dimension = Dim::Texture2DMS;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        view.dimension = Dim::Texture2DMSArray;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        view.dimension = uav ? Dim::Texture2DArray : Dim::TextureCube;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        view.dimension = uav ? Dim::Texture2DArray : Dim::TextureCubeArray;
        break;
    default:
        view.dimension = Dim::Automatic;
        break;
    }
    const bool depth = r->texture->desc().depth_stencil;
    const bool stencil_plane = d.format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ||
                               d.format == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    if (d.format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) {
        view.pixel_format = graphics::PixelFormat::X32_Stencil8;
    } else if (d.format == DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
        view.pixel_format = graphics::PixelFormat::X24_Stencil8;
    } else if (!depth && view_format != graphics::PixelFormat::Unknown &&
               view_format != r->texture->desc().pixel_format) {
        // The view reads the resource AS its format (an sRGB view of a
        // TYPELESS texture decodes sRGB), as the D3D11 frontend measured.
        view.pixel_format = view_format;
    }
    switch (d3d11::view_swizzle_for(d.format, !stencil_plane && depth)) {
    case d3d11::ViewSwizzle::OpaqueAlpha:
        view.swizzle = graphics::BackendViewSwizzle::RGB1;
        break;
    case d3d11::ViewSwizzle::Bgra4:
        view.swizzle = graphics::BackendViewSwizzle::GBAR;
        break;
    case d3d11::ViewSwizzle::DepthRed:
        view.swizzle = graphics::BackendViewSwizzle::R001;
        break;
    case d3d11::ViewSwizzle::Identity:
        break;
    }
    view.component_mapping = mapping;
    view.most_detailed_mip = d.most_detailed_mip;
    view.mip_levels = uav ? 1u : d.mip_levels;
    view.first_array_slice = d.first_slice;
    view.array_size = d.array_size;
    bound = t.set_texture(slot, r->texture.get(), view);
    if (!bound) {
        if (mapping != 0) {
            unimplemented("ExecuteCommandLists",
                          "a Shader4ComponentMapping the backend cannot "
                          "apply to a texture view");
        }
        t.clear_slot(slot);
    }
    return 0;
}

void Replayer::resolve_heap_window_slot(HeapWindowMirror& window, u32 slot,
                                        const Descriptor* d) {
    u32 entry = 0;
    bool bound = false;
    // Every CBV, SRV and UAV of the heap, each as its own view type: an
    // SRV and a UAV as bind_view binds it in a table, a CBV as its buffer
    // at the view's address with its vector count (SizeInBytes is a
    // multiple of 256, Learn D3D12_CONSTANT_BUFFER_VIEW_DESC; one vector
    // is 16 bytes). An empty record -- a slot never written, which tier 3
    // allows wherever the shader does not read it (Learn, "Hardware
    // Tiers") -- and a null view read as null.
    if (d != nullptr && d->type == DescriptorType::Srv &&
        d->dimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE &&
        std::find(window.rt_slots.begin(), window.rt_slots.end(), slot) == window.rt_slots.end()) {
        window.rt_slots.push_back(slot);
    }
    if (d != nullptr &&
        (d->type == DescriptorType::Srv || d->type == DescriptorType::Uav)) {
        // The slot is every shader's; a typed view read as raw memory runs
        // typed UAV atomics on its element words (dxbc_to_spirv_typed_raw).
        entry = bind_view(*window.table, slot, *d, d->type == DescriptorType::Uav,
                          bound);
    } else if (d != nullptr && d->type == DescriptorType::Cbv) {
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (f.resolve_va(d->first_element, d->num_elements, buffer, offset) &&
            window.table->set_buffer(slot, buffer, offset)) {
            bound = true;
            entry = d->num_elements / 16u;
        } else {
            if (offset > 0xFFFFFFFFull) {
                unimplemented("ExecuteCommandLists",
                              "a buffer view more than 4 GiB into its heap");
            }
            window.table->clear_slot(slot);
        }
    } else {
        window.table->clear_slot(slot);
    }
    u32* side = window.side_words +
                static_cast<usize>(slot) * graphics::kDescriptorHeapWindowSideWords;
    side[graphics::kDescriptorHeapWindowEntryWord] = bound ? entry : 0u;
    // A typed view bind_view bound as raw memory: its address, size and
    // format, at the words a table slot's record holds them
    // (graphics::kTypedBufferEntryWords; typed_raw_word reads both alike).
    const bool raw = bound && d != nullptr && d->type != DescriptorType::Cbv &&
                     (entry & graphics::kTypedBufferRaw) != 0;
    for (u32 w = 1; w < graphics::kDescriptorHeapWindowNullWord; ++w) {
        side[w] = raw ? m_typed_raw_words[w - 1] : 0u;
    }
    side[graphics::kDescriptorHeapWindowNullWord] = bound ? 0u : 1u;

    // Tiled status of a buffer view: a residency record id, kept while the
    // slot holds a view of a tiled buffer.
    const bool view = d != nullptr && (d->type == DescriptorType::Srv ||
                                       d->type == DescriptorType::Uav);
    Resource* r = view ? resource(d->resource, d->resource_serial) : nullptr;
    const bool tiled_buffer = r != nullptr && r->is_buffer() && r->reserved &&
                              r->tiles != nullptr;
    if (slot < window.residency_id.size()) {
        u32& id = window.residency_id[slot];
        if (tiled_buffer && id == 0) {
            const auto free = std::find(window.residency_slot.begin(),
                                        window.residency_slot.end(), ~0u);
            if (free != window.residency_slot.end()) {
                *free = slot;
                id = static_cast<u32>(free - window.residency_slot.begin()) + 1u;
            } else {
                window.residency_slot.push_back(slot);
                id = static_cast<u32>(window.residency_slot.size());
            }
        } else if (!tiled_buffer && id != 0) {
            window.residency_slot[id - 1u] = ~0u;
            id = 0;
        }
        side[graphics::kDescriptorHeapWindowResidencyWord] = id;
    }
    if (!window.aux || slot >= window.count) return;
    // The auxiliary region (graphics::kDescriptorHeapWindowAuxBinding).
    const u32 aux = window.count + 1u + slot;
    graphics::BackendDescriptorTable& t = *window.table;
    if (!view) {
        t.clear_slot(aux);
        return;
    }
    const bool uav = d->type == DescriptorType::Uav;
    if (d->dimension == D3D12_SRV_DIMENSION_BUFFER) {
        if (!uav) {
            t.clear_slot(aux);
            return;
        }
        // The UAV's counter (Learn, CreateUnorderedAccessView:
        // pCounterResource at CounterOffsetInBytes, which a structured
        // UAV's IncrementCounter / DecrementCounter moves). A view without
        // one gets the device's scratch word, so a shader that moves it
        // anyway reads and writes memory that exists.
        graphics::BackendBuffer* buffer = nullptr;
        u64 byte = 0;
        if (d->counter != 0) {
            Resource* counter = f.find_serial<Resource>(
                d->counter, d->counter_serial, Kind::Resource);
            buffer = counter != nullptr ? counter->buffer() : nullptr;
            if (buffer != nullptr &&
                counter->offset > ~u64{0} - d->counter_offset) {
                buffer = nullptr;
            }
            if (buffer != nullptr) byte = counter->offset + d->counter_offset;
        }
        if (buffer == nullptr) {
            if (!f.m_window_counter_scratch) {
                graphics::BackendBufferDesc desc{};
                desc.byte_width = 16;
                desc.shader_resource = true;
                desc.unordered_access = true;
                const std::array<u32, 4> zero{};
                f.m_window_counter_scratch =
                    f.m_backend.create_buffer(desc, zero.data());
            }
            buffer = f.m_window_counter_scratch.get();
            byte = 0;
        }
        if (buffer == nullptr || !t.set_buffer(aux, buffer, byte)) {
            unimplemented("ExecuteCommandLists",
                          "a heap window UAV counter the backend cannot bind");
            t.clear_slot(aux);
        }
        return;
    }
    // A texture view's residency companion in the view's own dimension
    // (the shader declares the one the view has), 1.0 everywhere for a
    // resource that is not tiled or a null view.
    using Dim = graphics::BackendTextureViewDimension;
    bool companion = false;
    if (d->dimension == D3D12_SRV_DIMENSION_TEXTURE3D) {
        companion = t.set_residency_texture_3d(aux);  // volumes are never tiled
    } else {
        graphics::BackendTextureViewDesc v{};
        switch (d->dimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            v.dimension = Dim::Texture2DArray;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            v.dimension = Dim::Texture2DMS;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            v.dimension = Dim::Texture2DMSArray;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            v.dimension = uav ? Dim::Texture2DArray : Dim::TextureCube;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            v.dimension = uav ? Dim::Texture2DArray : Dim::TextureCubeArray;
            break;
        default:
            v.dimension = Dim::Texture2D;
            break;
        }
        const bool ms = v.dimension == Dim::Texture2DMS ||
                        v.dimension == Dim::Texture2DMSArray;
        const graphics::BackendTexture2D* texture =
            r != nullptr && r->reserved && r->texture ? r->texture.get() : nullptr;
        unsigned samples = ms ? 4u : 1u;
        if (texture != nullptr) {
            v.most_detailed_mip = d->most_detailed_mip;
            v.mip_levels = uav ? 1u : d->mip_levels;
            v.first_array_slice = d->first_slice;
            v.array_size = d->array_size;
            samples = std::max(1u, texture->desc().sample_count);
        }
        companion = t.set_residency_texture(aux, texture, v, samples);
    }
    if (!companion) {
        unimplemented("ExecuteCommandLists",
                      "a heap window residency companion the backend cannot "
                      "bind");
        t.clear_slot(aux);
    }
}

u32 Replayer::append_buffer_residency(const Descriptor& d,
                                      std::vector<u32>& words) {
    Resource* r = resource(d.resource, d.resource_serial);
    if (r == nullptr || !r->is_buffer() || !r->reserved || r->tiles == nullptr) {
        return 0;
    }
    const bool raw = (d.flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    const bool typed = !raw && d.structure_stride == 0;
    const u64 stride = raw ? 4u : typed ? dxgi_element_bytes(d.format)
                                        : d.structure_stride;
    if (stride == 0) return 0;
    const u64 tiles = r->tiles->total_tiles;
    const std::vector<u32>* bits = r->tiles->resident.get();
    const u64 start = d.first_element * stride;
    const u64 units = typed ? d.num_elements : d.num_elements * stride;
    if (units > 0xFFFFFFFFull || tiles > 0xFFFFFFFFull) {
        unimplemented("ExecuteCommandLists",
                      "tiled status of a buffer view past 4 GiB");
        return 0;
    }
    const u32 offset = static_cast<u32>(words.size());
    words.push_back(static_cast<u32>(tiles));
    words.push_back(static_cast<u32>(start / graphics::BACKEND_TILE_BYTES));
    words.push_back(static_cast<u32>(start % graphics::BACKEND_TILE_BYTES));
    words.push_back(static_cast<u32>(units));
    words.push_back(typed ? static_cast<u32>(stride) : 1u);
    const usize count = (static_cast<usize>(tiles) + 31u) / 32u;
    for (usize w = 0; w < count; ++w) {
        words.push_back(bits != nullptr && w < bits->size() ? (*bits)[w] : 0u);
    }
    return offset;
}

void Replayer::window_residency_words(DescriptorHeap* heap,
                                      HeapWindowMirror& window,
                                      std::vector<u32>& words) {
    // Header word r: record r's offset (word 0, id 0, is never a record).
    words.assign(std::max<usize>(window.residency_slot.size() + 1u,
                                 graphics::kTiledResidencyMinWords),
                 0u);
    if (heap == nullptr) return;
    std::lock_guard<std::mutex> lock(heap->window_mutex);
    for (usize k = 0; k < window.residency_slot.size(); ++k) {
        const u32 slot = window.residency_slot[k];
        if (slot >= heap->records.size()) continue;
        const u32 offset = append_buffer_residency(heap->records[slot], words);
        words[k + 1u] = offset;
    }
}

HeapWindowMirror* Replayer::sync_heap_window(DescriptorHeap* heap, bool aux) {
    const auto make_table = [&](u32 count, bool with_aux) {
        // count descriptors and the sentinel null slot after them, twice
        // with the auxiliary region.
        graphics::BackendDescriptorTableDesc desc{};
        desc.slot_count = (count + 1u) * (with_aux ? 2u : 1u);
        desc.slot_class = graphics::BackendDescriptorClass::Texture;
        desc.persistent = true;
        return f.m_backend.create_descriptor_table(desc);
    };
    const auto make = [&](HeapWindowMirror& window, u32 count) -> bool {
        window.count = count;
        window.aux = aux;
        window.table = make_table(count, aux);
        graphics::BackendHeapDesc side{};
        side.size = static_cast<u64>(count + 1u) *
                    graphics::kDescriptorHeapWindowSideWords * sizeof(u32);
        side.cpu_visible = true;
        side.buffers_only = true;
        window.side = f.m_backend.create_heap(side);
        window.side_words =
            window.side ? static_cast<u32*>(window.side->host_pointer())
                        : nullptr;
        if (!window.table || !window.side || window.side_words == nullptr ||
            window.side->buffer() == nullptr) {
            window.table.reset();
            window.side.reset();
            window.side_words = nullptr;
            return false;
        }
        // Every slot starts null (the heap is zero-filled).
        for (u32 slot = 0; slot <= count; ++slot) {
            window.side_words[slot * graphics::kDescriptorHeapWindowSideWords +
                              graphics::kDescriptorHeapWindowNullWord] = 1u;
        }
        window.dirty.assign(count, 0u);
        window.dirty_list.clear();
        window.residency_id.assign(count, 0u);
        window.residency_slot.clear();
        return true;
    };
    if (heap == nullptr) {
        HeapWindowMirror& window = rr.null_window;
        if (window.table && aux && !window.aux) {
            window.retired.push_back(std::move(window.table));
        }
        if (!window.table && !make(window, 0u)) {
            unimplemented("ExecuteCommandLists",
                          "a descriptor-heap window the backend cannot make");
            return nullptr;
        }
        return &window;
    }
    std::lock_guard<std::mutex> lock(heap->window_mutex);
    if (heap->window == nullptr) {
        auto window = std::make_unique<HeapWindowMirror>();
        const u32 count = static_cast<u32>(heap->records.size());
        if (!make(*window, count)) {
            unimplemented("ExecuteCommandLists",
                          "a descriptor-heap window the backend cannot make");
            return nullptr;
        }
        // The records written before the first window draw.
        for (u32 slot = 0; slot < count; ++slot) {
            if (heap->records[slot].type != DescriptorType::None) {
                window->dirty[slot] = 1u;
                window->dirty_list.push_back(slot);
            }
        }
        heap->window = std::move(window);
    }
    HeapWindowMirror& window = *heap->window;
    if (window.rt_generation != f.m_rt_generation) {
        // A structure was rebuilt into a new backend object, or freed.
        window.rt_generation = f.m_rt_generation;
        for (const u32 slot : window.rt_slots) {
            if (slot < window.dirty.size() && window.dirty[slot] == 0u) {
                window.dirty[slot] = 1u;
                window.dirty_list.push_back(slot);
            }
        }
    }
    if (aux && !window.aux) {
        // The first draw that reads the auxiliary region: a table with it,
        // every written slot resolved again into it.
        auto table = make_table(window.count, true);
        if (!table) {
            unimplemented("ExecuteCommandLists",
                          "a descriptor-heap window auxiliary region the "
                          "backend cannot make");
            return nullptr;
        }
        window.retired.push_back(std::move(window.table));
        window.table = std::move(table);
        window.aux = true;
        for (u32 slot = 0; slot < window.count; ++slot) {
            if (heap->records[slot].type != DescriptorType::None &&
                window.dirty[slot] == 0u) {
                window.dirty[slot] = 1u;
                window.dirty_list.push_back(slot);
            }
        }
    }
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        graphics::frame_profile::add(graphics::frame_profile::Counter::WindowSyncs);
        graphics::frame_profile::add(graphics::frame_profile::Counter::WindowResolves,
                                     window.dirty_list.size());
    }
    for (const u32 slot : window.dirty_list) {
        window.dirty[slot] = 0u;
        resolve_heap_window_slot(window, slot, &heap->records[slot]);
    }
    window.dirty_list.clear();
    return &window;
}

bool Replayer::bind_stage(
    ReplayState& st, u32 stage, const CompiledShader& shader,
    std::array<const graphics::BackendDescriptorTable*,
                               graphics::BACKEND_MAX_DESCRIPTOR_SETS>& out,
    std::vector<u32>& push) {
    out.fill(nullptr);
    push.assign(graphics::kPushBlockBytes / 4, 0u);
    if (!shader.present) return true;
    const u32 bind_point = stage == STAGE_COMPUTE ? 1u : 0u;
    const RootLayout* root = st.roots[bind_point].get();
    if (root == nullptr) return false;
    const std::array<u32, 4> slots{shader.registers.cbv_slots,
                                   shader.registers.srv_slots,
                                   shader.registers.sampler_slots,
                                   shader.registers.uav_slots};
    const std::array<graphics::BackendDescriptorClass, 4> classes{
        graphics::BackendDescriptorClass::Buffer,
        graphics::BackendDescriptorClass::Texture,
        graphics::BackendDescriptorClass::Sampler,
        graphics::BackendDescriptorClass::Texture};
    // Only storage is reused: every source is gathered again from the current
    // root arguments and descriptors, including writes after Close/replay.
    using Source = BindingSource;
    auto& sources = m_binding_sources;
    for (auto& source_set : sources) source_set.clear();
    const auto& args = st.args[bind_point];
    // What the sampler sources are a function of: the shader, the root
    // signature (its ranges and static samplers), each sampler table's GPU
    // handle and the sampler heap with its write generation. When the
    // stage's memo was filled from the same, its sources are not gathered
    // again (thousands of slots for an unbounded or heap-indexed table).
    u64 sampler_fast_key = 0;
    bool sampler_fast_hit = false;
    if (slots[SET_SAMPLER] != 0 && !shader.tiled_status && stage < rr.sampler_memo.size()) {
        u64 h = 1469598103934665603ull;
        const auto mix = [&](u64 v) { h = (h ^ v) * 1099511628211ull; };
        mix(reinterpret_cast<uintptr_t>(&shader));
        mix(reinterpret_cast<uintptr_t>(root));
        mix(reinterpret_cast<uintptr_t>(st.heaps[1]));
        mix(st.heaps[1] != nullptr ? st.heaps[1]->generation.load(std::memory_order_relaxed) : 0u);
        for (u32 index = 0; index < root->parameters.size(); ++index) {
            const BindingParameter& p = root->parameters[index];
            if (p.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !visible_to(p.visibility, stage)) continue;
            bool samplers = false;
            for (const BindingRange& range : p.ranges) samplers |= set_of_range(range.type) == SET_SAMPLER;
            if (!samplers) continue;
            const auto* arg = index < args.size() ? &args[index] : nullptr;
            mix(index);
            mix(arg != nullptr && arg->set ? arg->table + 1u : 0u);
        }
        sampler_fast_key = h;
        const auto& memo = rr.sampler_memo[stage];
        const auto& kept = rr.tables[stage][SET_SAMPLER];
        sampler_fast_hit = memo.valid && memo.fast_key == h && memo.record_buffer != nullptr &&
                           memo.shader == &shader && kept && memo.table == kept.get() &&
                           kept->slot_count() >= slots[SET_SAMPLER];
    }
    for (u32 set = 0; set < 4; ++set) {
        if (set != SET_SAMPLER || !sampler_fast_hit) sources[set].resize(slots[set]);
    }
    for (u32 index = 0; index < root->parameters.size(); ++index) {
        const BindingParameter& p = root->parameters[index];
        if (!visible_to(p.visibility, stage)) continue;
        const auto* arg = index < args.size() ? &args[index] : nullptr;
        switch (p.type) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            if (arg == nullptr || !arg->set) break;
            u32 base = 0;
            DescriptorHeap* heap = f.gpu_descriptor(arg->table, base);
            if (heap == nullptr) break;
            for (const BindingRange& range : p.ranges) {
                // Each register of the range the shader declares lands on
                // its slot: the register itself for a 5.0 shader (space 0),
                // the remapped run's slot for a 5.1 one.
                const u32 set = set_of_range(range.type);
                if (sampler_fast_hit && set == SET_SAMPLER) continue;
                const u64 available =
                    heap->records.size() > static_cast<u64>(base) + range.table_offset
                        ? heap->records.size() - base - range.table_offset
                        : 0u;
                const u64 count = range.count == 0xFFFFFFFFu
                                      ? available
                                      : std::min<u64>(range.count, available);
                // Visit only what the shader can declare: its runs in this
                // space (5.1), or the table's registers (5.0).
                const auto bind_register = [&](u32 reg) {
                    if (reg < range.base_register ||
                        reg - range.base_register >= count) {
                        return;
                    }
                    const u32 slot = shader.slot_of(set, range.space, reg);
                    if (slot >= slots[set]) return;
                    const u64 at = static_cast<u64>(base) + range.table_offset +
                                   (reg - range.base_register);
                    sources[set][slot] = {Source::Kind::Record,
                                          &heap->records[at], 0, index, nullptr};
                };
                if (shader.table_mode) {
                    for (const auto& run : shader.runs[set]) {
                        if (run.space != range.space) continue;
                        for (u32 k = 0; k < run.count; ++k) {
                            bind_register(run.first_register + k);
                        }
                    }
                } else if (range.space == 0) {
                    for (u32 reg = range.base_register;
                         reg < slots[set] && reg - range.base_register < count;
                         ++reg) {
                        bind_register(reg);
                    }
                }
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            const u32 slot =
                shader.slot_of(SET_CBV, p.space, p.shader_register);
            if (slot < slots[SET_CBV]) {
                sources[SET_CBV][slot] = {Source::Kind::Constants, nullptr, 0,
                                          index, nullptr};
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: {
            const u32 set = p.type == D3D12_ROOT_PARAMETER_TYPE_CBV ? SET_CBV
                            : p.type == D3D12_ROOT_PARAMETER_TYPE_SRV ? SET_SRV
                                                                       : SET_UAV;
            const u32 slot = shader.slot_of(set, p.space, p.shader_register);
            if (slot < slots[set] && arg != nullptr && arg->set) {
                sources[set][slot] = {Source::Kind::View, nullptr, arg->va,
                                      index, nullptr};
            }
            break;
        }
        default:
            break;
        }
    }
    // SM 6.6 SamplerDescriptorHeap[] (graphics::kDxbcSamplerHeapSpace):
    // element e is slot e of the bound sampler heap, the whole of it
    // (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE slots, compile_shader).
    if (shader.table_mode && st.heaps[1] != nullptr && !sampler_fast_hit) {
        DescriptorHeap* samplers = st.heaps[1];
        for (const auto& run : shader.runs[SET_SAMPLER]) {
            if (run.space != graphics::kDxbcSamplerHeapSpace) continue;
            for (u32 k = 0; k < run.count && k < samplers->records.size(); ++k) {
                const u32 slot = run.first_slot + k;
                if (slot >= slots[SET_SAMPLER]) break;
                sources[SET_SAMPLER][slot] = {Source::Kind::Record,
                                              &samplers->records[k], 0, 0,
                                              nullptr};
            }
        }
    }
    {
        // The reserved bindings (CompiledShader::rt_reserved): a top level's
        // blob view always; a DispatchRays megakernel's shader tables and
        // dispatch constants.
        // Local root descriptors first (what 6): their acceleration
        // structures have companions too.
        for (const auto& r : shader.rt_reserved) {
            if (r.what != 6 || rt_dispatch_ == nullptr || r.as_slot >= rt_dispatch_->local_views.size()) continue;
            const auto& vas = rt_dispatch_->local_views[r.as_slot];
            for (u32 e = 1; e < vas.size() && e < r.count; ++e) {
                if (r.slot + e < slots[r.set] && vas[e] != 0) {
                    sources[r.set][r.slot + e] = {Source::Kind::View, nullptr, vas[e], 0, nullptr};
                }
            }
        }
        for (const auto& r : shader.rt_reserved) {
            if (r.slot >= slots[r.set] || r.what == 6) continue;
            // A sampler-memo hit gathers no sampler sources: a local static
            // sampler (what 8) is the shader's own, so the memo already
            // holds its record.
            if (r.set == SET_SAMPLER && sampler_fast_hit) continue;
            if (r.what != 5 && r.what != 8 && rt_dispatch_ == nullptr) continue;
            if (r.what < 4) {
                if (rt_dispatch_->tables[r.what] != 0) {
                    sources[r.set][r.slot] = {Source::Kind::View, nullptr, rt_dispatch_->tables[r.what], 0, nullptr};
                }
            } else if (r.what == 4) {
                sources[r.set][r.slot] = {Source::Kind::Direct, nullptr, 0, 0, nullptr};
            } else if (r.what == 7) {
                sources[r.set][r.slot] = {Source::Kind::Translation, nullptr, 0, 0, nullptr};
            } else if (r.what == 8) {
                if (r.as_slot < shader.rt_samplers.size()) {
                    sources[r.set][r.slot] = {Source::Kind::Static, nullptr, 0, 0, &shader.rt_samplers[r.as_slot]};
                }
            } else {
                // Each element's blob view: the VA its element is bound from.
                for (u32 e = 0; e < r.count; ++e) {
                    if (r.as_slot + e >= slots[SET_SRV] || r.slot + e >= slots[r.set]) break;
                    const Source& as = sources[SET_SRV][r.as_slot + e];
                    const u64 va = as.kind == Source::Kind::View ? as.va
                                   : as.kind == Source::Kind::Record && as.record != nullptr
                                       ? as.record->first_element
                                       : 0u;
                    if (va != 0) sources[r.set][r.slot + e] = {Source::Kind::View, nullptr, va, 0, nullptr};
                }
            }
        }
    }
    for (const BindingSampler& s : root->static_samplers) {
        const u32 slot =
            shader.slot_of(SET_SAMPLER, s.space, s.shader_register);
        if (!sampler_fast_hit && slot < slots[SET_SAMPLER] && visible_to(s.visibility, stage)) {
            sources[SET_SAMPLER][slot] = {Source::Kind::Static, nullptr, 0, 0,
                                          &s.state};
        }
    }
    // A 5.1 shader reads its samplers' MipLODBias / POINT-mip state from
    // records in the CBV table (DxbcTranslationOptions::sampler_state_buffer),
    // not from the push block.
    // Twins occupy the upper half of a descriptor-table sampler argument
    // buffer (compile_shader doubles sampler_slots). Records are one per
    // real slot, indexed by that slot's binding.
    const bool dynamic_samplers = shader.table_mode &&
                                  shader.sampler_state_slot != ~0u &&
                                  slots[SET_SAMPLER] > 0u;
    // A tiled-status shader's table also holds a status twin per real
    // sampler after the border twins (CompiledShader::status_sampler_base).
    const u32 real_samplers =
        shader.tiled_status ? shader.status_sampler_count
        : dynamic_samplers  ? slots[SET_SAMPLER] / 2u
                            : slots[SET_SAMPLER];
    std::vector<u32> sampler_records;
    if (shader.table_mode && shader.sampler_state_slot < slots[SET_CBV] && !sampler_fast_hit) {
        sampler_records.assign(
            std::max(1u, real_samplers) * graphics::kSamplerStateRecordWords,
            0u);
    }
    // Learn (ByteAddressBuffer / StructuredBuffer): one word-count per table
    // slot. 0 is a null or empty view, so a load returns 0. A root descriptor
    // has no NumElements; its size is the resource from the GPU VA.
    std::vector<u32> srv_view_sizes;
    std::vector<u32> uav_view_sizes;
    if (shader.table_mode && shader.srv_view_size_slot != ~0u) {
        srv_view_sizes.assign(std::max(1u, slots[SET_SRV]), 0u);
    }
    if (shader.table_mode && shader.uav_view_size_slot != ~0u) {
        uav_view_sizes.assign(std::max(1u, slots[SET_UAV]), 0u);
    }
    // D3D11.3 7.5: vector count of the constant buffer bound at each slot.
    // 0 is a null or empty view, so every element reads 0. A root CBV has
    // no SizeInBytes; its count is the resource from the GPU VA. Root
    // constants are the dwords the signature supplied.
    std::vector<u32> cbv_limits;
    if (shader.table_mode && shader.cbv_limit_slot != ~0u) {
        cbv_limits.assign(std::max(1u, slots[SET_CBV]), 0u);
    }
    // D3D11.3 22.4.14 / 22.4.1: 1 marks a null descriptor, so GetDimensions
    // returns 0 without querying the image. Slots start null.
    std::vector<u32> srv_null;
    std::vector<u32> uav_null;
    if (shader.srv_null_slot != ~0u) {
        srv_null.assign(std::max(1u, slots[SET_SRV]), 1u);
    }
    if (shader.uav_null_slot != ~0u) {
        uav_null.assign(std::max(1u, slots[SET_UAV]), 1u);
    }
    // Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): one packed element-bias
    // entry per table slot. 0 is a null slot or a view with no bias whose
    // format has alpha. bind_view's return is that entry for a typed view
    // and a word count for a raw or structured one; a typed shader reads
    // only its own slots.
    std::vector<u32> srv_typed_bias;
    std::vector<u32> uav_typed_bias;
    if (shader.srv_typed_bias_slot != ~0u) {
        srv_typed_bias.assign(
            std::max(1u, slots[SET_SRV]) * graphics::kTypedBufferEntryWords, 0u);
    }
    if (shader.uav_typed_bias_slot != ~0u) {
        uav_typed_bias.assign(
            std::max(1u, slots[SET_UAV]) * graphics::kTypedBufferEntryWords, 0u);
    }
    const auto note_null = [&](u32 set, u32 slot, bool is_null) {
        std::vector<u32>& flags = set == SET_UAV ? uav_null : srv_null;
        if (slot < flags.size()) flags[slot] = is_null ? 1u : 0u;
    };
    const auto note_cbv_limit = [&](u32 slot, u32 vectors) {
        if (shader.table_mode) {
            if (slot < cbv_limits.size()) cbv_limits[slot] = vectors;
        } else if (slot < graphics::kConstantBufferLimitSlots) {
            push[graphics::kConstantBufferLimitOffset / 4 + slot] =
                vectors | graphics::kConstantBufferLimitKnown;
        }
    };
    const auto note_view_size = [&](u32 set, u32 slot, u32 words) {
        std::vector<u32>& sizes =
            set == SET_UAV ? uav_view_sizes : srv_view_sizes;
        if (slot < sizes.size()) sizes[slot] = words;
    };
    const auto note_typed_bias = [&](u32 set, u32 slot, u32 entry) {
        std::vector<u32>& biases =
            set == SET_UAV ? uav_typed_bias : srv_typed_bias;
        // graphics::kTypedBufferEntryWords per slot: the entry, then a raw
        // view's address, size and format (kTypedBufferRaw).
        const usize at = static_cast<usize>(slot) * graphics::kTypedBufferEntryWords;
        if (at + graphics::kTypedBufferEntryWords > biases.size()) return;
        biases[at] = entry;
        const bool raw = (entry & graphics::kTypedBufferRaw) != 0;
        for (u32 w = 0; w < 7; ++w) {
            biases[at + 1 + w] = raw ? m_typed_raw_words[w] : 0u;
        }
    };
    u32 point_mip_mask = 0;
    u32 nudge_mask = 0;
    // A memo hit whose records already sit in their own buffer; a fill that
    // refreshes a memo (its records go to a new buffer).
    ReplayResources::SamplerMemo* memo_records = nullptr;
    ReplayResources::SamplerMemo* memo_filled = nullptr;
    for (u32 set = 0; set < 4; ++set) {
        if (slots[set] == 0) continue;
        graphics::BackendDescriptorTable* t =
            table(stage, set, slots[set], classes[set]);
        if (t == nullptr) return false;
        // The sampler table: unchanged sources keep the table and records
        // the last fill made (ReplayResources::SamplerMemo).
        ReplayResources::SamplerMemo* memo = nullptr;
        std::vector<std::pair<u8, const void*>> sampler_key;
        if (set == SET_SAMPLER && stage < rr.sampler_memo.size()) {
            memo = &rr.sampler_memo[stage];
            if (sampler_fast_hit && memo->table == t) {
                memo_records = memo;
                for (u32 k = 0; k < 16 && k < push.size(); ++k) {
                    if ((memo->bias_mask & (1u << k)) != 0u) push[k] = memo->bias[k];
                }
                point_mip_mask |= memo->point_mip_mask;
                nudge_mask |= memo->nudge_mask;
                out[set] = t;
                continue;
            }
            sampler_key.reserve(slots[set]);
            for (const Source& src : sources[SET_SAMPLER]) {
                sampler_key.emplace_back(
                    static_cast<u8>(src.kind),
                    src.kind == Source::Kind::Static
                        ? static_cast<const void*>(src.sampler)
                        : static_cast<const void*>(src.record));
            }
            const DescriptorHeap* heap = st.heaps[1];
            const u64 generation =
                heap != nullptr ? heap->generation.load(std::memory_order_relaxed) : 0u;
            if (memo->valid && memo->shader == &shader && memo->table == t &&
                memo->heap == heap && memo->generation == generation &&
                memo->sources == sampler_key &&
                memo->records.size() == sampler_records.size()) {
                if (memo->record_buffer) {
                    memo_records = memo;
                } else {
                    sampler_records = memo->records;
                }
                for (u32 k = 0; k < 16 && k < push.size(); ++k) {
                    if ((memo->bias_mask & (1u << k)) != 0u) push[k] = memo->bias[k];
                }
                point_mip_mask |= memo->point_mip_mask;
                nudge_mask |= memo->nudge_mask;
                out[set] = t;
                continue;
            }
            memo->valid = false;
            memo->heap = heap;
            memo->generation = generation;
        }
        const std::array<u32, 16> push_before = [&] {
            std::array<u32, 16> a{};
            for (u32 k = 0; k < 16 && k < push.size(); ++k) a[k] = push[k];
            return a;
        }();
        const u32 masks_before[2] = {point_mip_mask, nudge_mask};
        for (u32 slot = 0; slot < slots[set]; ++slot) {
            const Source& src = sources[set][slot];
            if (set == SET_SAMPLER && dynamic_samplers && slot >= real_samplers) {
                // The border twin of real slot `slot - real_samplers`: no
                // root-signature source names the upper half itself, so it
                // follows its primary whatever that primary's source is.
                const Source& from = sources[SET_SAMPLER][slot - real_samplers];
                const SamplerState* primary =
                    from.kind == Source::Kind::Static ? from.sampler
                    : from.kind == Source::Kind::Record && from.record != nullptr &&
                            from.record->type == DescriptorType::Sampler
                        ? &from.record->sampler
                        : nullptr;
                if (primary == nullptr ||
                    !d3d_sampler_needs_border_twin(sampler_fields(*primary))) {
                    t->clear_slot(slot);
                    continue;
                }
                graphics::BackendSamplerDesc twin{};
                d3d_backend_sampler(sampler_fields(*primary), twin, nullptr);
                // Opaque white: the blend weight is hi - lo.
                twin.border_color = 0xffffffffu;
                (void)t->set_sampler(slot, twin);
                continue;
            }
            switch (src.kind) {
            case Source::Kind::None:
                t->clear_slot(slot);
                if (set == SET_CBV) note_cbv_limit(slot, 0);
                else if (set == SET_SRV || set == SET_UAV) note_null(set, slot, true);
                break;
            case Source::Kind::Direct:
                // The DispatchRays constants (Replayer::rt_dispatch_).
                if (rt_dispatch_ == nullptr ||
                    !t->set_buffer(slot, rt_dispatch_->constants, rt_dispatch_->constants_offset)) {
                    t->clear_slot(slot);
                    if (set == SET_CBV) note_cbv_limit(slot, 0);
                    break;
                }
                if (set == SET_CBV) note_cbv_limit(slot, graphics::kRtConstWords / 4u);
                break;
            case Source::Kind::Translation:
                // The local root descriptor translation (Replayer::rt_dispatch_).
                if (rt_dispatch_ == nullptr || rt_dispatch_->translation == nullptr ||
                    !t->set_buffer(slot, rt_dispatch_->translation, rt_dispatch_->translation_offset)) {
                    t->clear_slot(slot);
                    note_null(set, slot, true);
                    break;
                }
                note_null(set, slot, false);
                note_view_size(set, slot, rt_dispatch_->translation_words);
                break;
            case Source::Kind::Constants: {
                const auto& values = args[src.parameter].constants;
                graphics::BackendBuffer* buffer = nullptr;
                u64 offset = 0;
                const u32 zero = 0;
                if (!upload(values.empty() ? static_cast<const void*>(&zero)
                                           : values.data(),
                            values.empty() ? 4u : values.size() * 4u, buffer,
                            offset)) {
                    t->clear_slot(slot);
                    if (set == SET_CBV) note_cbv_limit(slot, 0);
                    break;
                }
                (void)t->set_buffer(slot, buffer, static_cast<unsigned>(offset));
                if (set == SET_CBV) {
                    note_cbv_limit(
                        slot, values.empty()
                                  ? 0u
                                  : static_cast<u32>((values.size() + 3u) / 4u));
                }
                break;
            }
            case Source::Kind::View: {
                if (set == SET_SRV && slot < shader.srv_slot_acceleration.size() &&
                    shader.srv_slot_acceleration[slot] != 0) {
                    // A root SRV naming a top-level acceleration structure.
                    (void)bind_acceleration_structure(*t, slot, src.va);
                    note_null(set, slot, false);
                    break;
                }
                graphics::BackendBuffer* buffer = nullptr;
                u64 offset = 0;
                if (!f.resolve_va(src.va, 0, buffer, offset)) {
                    t->clear_slot(slot);
                    if (set == SET_CBV) note_cbv_limit(slot, 0);
                    else if (set == SET_SRV || set == SET_UAV) {
                        note_null(set, slot, true);
                    }
                    break;
                }
                if (!t->set_buffer(slot, buffer, offset)) {
                    if (offset > 0xFFFFFFFFull) {
                        unimplemented("ExecuteCommandLists",
                                      "a buffer view more than 4 GiB into "
                                      "its heap");
                        t->clear_slot(slot);
                        if (set == SET_CBV) note_cbv_limit(slot, 0);
                        else if (set == SET_SRV || set == SET_UAV) {
                            note_null(set, slot, true);
                        }
                        break;
                    }
                }
                if (set == SET_CBV) {
                    note_cbv_limit(slot, f.buffer_words_from_va(src.va) / 4u);
                } else if (set == SET_SRV || set == SET_UAV) {
                    note_view_size(set, slot, f.buffer_words_from_va(src.va));
                    note_null(set, slot, false);
                }
                break;
            }
            case Source::Kind::Static:
            case Source::Kind::Record: {
                if (set == SET_SAMPLER) {
                    const auto state_of = [](const Source& from) -> const SamplerState* {
                        if (from.kind == Source::Kind::Static) return from.sampler;
                        if (from.kind == Source::Kind::Record &&
                            from.record != nullptr &&
                            from.record->type == DescriptorType::Sampler) {
                            return &from.record->sampler;
                        }
                        return nullptr;
                    };
                    const SamplerState* state = state_of(src);
                    if (state == nullptr) {
                        t->clear_slot(slot);
                        break;
                    }
                    const D3DSamplerFields fields = sampler_fields(*state);
                    graphics::BackendSamplerDesc sampler{};
                    float bias = 0.0f;
                    d3d_backend_sampler(fields, sampler, &bias);
                    (void)t->set_sampler(slot, sampler);
                    if (!sampler_records.empty() && slot < real_samplers) {
                        d3d_write_sampler_state_record(
                            fields, bias,
                            sampler_records.data() +
                                slot * graphics::kSamplerStateRecordWords);
                    } else if (slot < 16) {
                        std::memcpy(push.data() + slot, &bias, 4);
                        if (d3d_point_mip_filter(state->filter)) {
                            point_mip_mask |= 1u << slot;
                        }
                        if (d3d_point_mip_tie_nudged(state->filter)) {
                            nudge_mask |= 1u << slot;
                        }
                    }
                    break;
                }
                const Descriptor& d = *src.record;
                if (set == SET_CBV) {
                    graphics::BackendBuffer* buffer = nullptr;
                    u64 offset = 0;
                    if (d.type != DescriptorType::Cbv ||
                        !f.resolve_va(d.first_element, d.num_elements, buffer,
                                      offset)) {
                        t->clear_slot(slot);
                        note_cbv_limit(slot, 0);
                        break;
                    }
                    if (!t->set_buffer(slot, buffer, offset)) {
                        if (offset > 0xFFFFFFFFull) {
                            unimplemented("ExecuteCommandLists",
                                          "a buffer view more than 4 GiB "
                                          "into its heap");
                            t->clear_slot(slot);
                            note_cbv_limit(slot, 0);
                            break;
                        }
                    }
                    // SizeInBytes is a multiple of 256 (Learn,
                    // D3D12_CONSTANT_BUFFER_VIEW_DESC). One vector is 16 bytes.
                    note_cbv_limit(slot, d.num_elements / 16u);
                    break;
                }
                const bool uav = set == SET_UAV;
                if (d.type != (uav ? DescriptorType::Uav : DescriptorType::Srv)) {
                    t->clear_slot(slot);
                    break;
                }
                bool bound = false;
                const u32 words = bind_view(*t, slot, d, uav, bound,
                                            uav && shader.uint_view_uav(set, slot));
                note_view_size(set, slot, words);
                note_typed_bias(set, slot, words);
                note_null(set, slot, !bound);
                // The size table: t0..t127, then u0..u7 (dxbc_to_spirv.hpp
                // kGuardSizeTableOffset); a slot past it is unguarded.
                // Shader model 5.0 reads this push block. 5.1 reads
                // srv_view_sizes / uav_view_sizes instead.
                const u32 entry = uav ? graphics::kGuardSizeSrvSlots + slot : slot;
                if ((uav ? slot < graphics::kGuardSizeUavSlots
                         : slot < graphics::kGuardSizeSrvSlots)) {
                    push[graphics::kGuardSizeTableOffset / 4 + entry] = words;
                }
                break;
            }
            }
        }
        if (memo != nullptr) {
            memo->valid = true;
            memo->shader = &shader;
            memo->table = t;
            memo->sources = std::move(sampler_key);
            memo->records = sampler_records;
            memo->bias_mask = 0;
            for (u32 k = 0; k < 16 && k < push.size(); ++k) {
                memo->bias[k] = push[k];
                if (push[k] != push_before[k]) memo->bias_mask |= 1u << k;
            }
            memo->point_mip_mask = point_mip_mask & ~masks_before[0];
            memo->nudge_mask = nudge_mask & ~masks_before[1];
            memo->fast_key = sampler_fast_key;
            memo_filled = memo;
        }
        out[set] = t;
    }
    if (memo_records != nullptr) {
        auto* cbv = rr.tables[stage][SET_CBV].get();
        if (cbv == nullptr ||
            !cbv->set_buffer(shader.sampler_state_slot, memo_records->record_buffer.get(), 0))
            return false;
    } else if (!sampler_records.empty()) {
        // After the sampler table is resolved: the records are complete.
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        auto* cbv = rr.tables[stage][SET_CBV].get();
        if (memo_filled != nullptr && cbv != nullptr) {
            // A buffer of the memo's own, which later matching draws bind
            // again; the one it replaces lives until the GPU is past it.
            const u64 now = f.m_backend.recording_generation();
            auto& retired = rr.retired_sampler_records;
            // Appended in generation order and completed in that order: the
            // retired entries are a prefix (no walk of the whole list).
            retired.erase(retired.begin(),
                          std::find_if(retired.begin(), retired.end(),
                                       [&](const auto& r) {
                                           return !f.m_backend.generation_complete(r.second);
                                       }));
            if (memo_filled->record_buffer) {
                retired.emplace_back(std::move(memo_filled->record_buffer), now);
            }
            graphics::BackendBufferDesc desc{};
            desc.byte_width = sampler_records.size() * 4u;
            desc.shader_resource = true;
            desc.constant_buffer = true;
            static const bool cache_enabled = [] {
                const char* value = std::getenv("VITREO_D3D12_SAMPLER_RECORD_CACHE");
                return value == nullptr || value[0] != '0';
            }();
            const bool reuse = cache_enabled &&
                !diagnostics::ab_cycle_off("d3d12-sampler-record-cache");
            auto created = reuse ? rr.sampler_record_buffers.find(sampler_records)
                                 : SamplerRecordBufferCache::Buffer{};
            graphics::compile_profile::note(created ? graphics::compile_profile::SamplerBufferHit
                                                     : graphics::compile_profile::SamplerBufferMiss);
            if (!created) {
                created = f.m_backend.create_buffer(desc, sampler_records.data());
                if (reuse && created && created->valid())
                    (void)rr.sampler_record_buffers.remember(sampler_records, created);
            }
            if (created && created->valid()) {
                memo_filled->record_buffer = std::move(created);
                if (!cbv->set_buffer(shader.sampler_state_slot,
                                      memo_filled->record_buffer.get(), 0)) return false;
                sampler_records.clear();
            }
        }
        if (!sampler_records.empty()) {
            if (cbv == nullptr ||
                !upload(sampler_records.data(), sampler_records.size() * 4u, buffer, offset) ||
                !cbv->set_buffer(shader.sampler_state_slot, buffer, offset)) return false;
        }
    }
    const auto bind_view_sizes = [&](const std::vector<u32>& words,
                                     u32 size_slot) -> bool {
        if (words.empty() || size_slot == ~0u) return true;
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        auto* cbv = rr.tables[stage][SET_CBV].get();
        const usize bytes = words.size() * sizeof(u32);
        if (cbv != nullptr && bytes > CONSTANT_CHUNK_BYTES) {
            // Past one upload chunk (a table of thousands of slots: a
            // DispatchRays megakernel's local view arrays at 8 typed-bias
            // words a slot): a buffer of its own, kept until the GPU is
            // past this recording.
            graphics::BackendBufferDesc desc{};
            desc.byte_width = bytes;
            desc.shader_resource = true;
            desc.constant_buffer = true;
            std::shared_ptr<graphics::BackendBuffer> own =
                f.m_backend.create_buffer(desc, words.data());
            if (!own || !own->valid()) return false;
            (void)cbv->set_buffer(size_slot, own.get(), 0);
            auto& retired = rr.retired_sampler_records;
            // Appended in generation order and completed in that order: the
            // retired entries are a prefix (no walk of the whole list).
            retired.erase(retired.begin(),
                          std::find_if(retired.begin(), retired.end(),
                                       [&](const auto& r) {
                                           return !f.m_backend.generation_complete(r.second);
                                       }));
            retired.emplace_back(std::move(own), f.m_backend.recording_generation());
            return true;
        }
        if (cbv == nullptr ||
            !upload(words.data(), words.size() * sizeof(u32), buffer, offset) ||
            offset > 0xFFFFFFFFull) {
            return false;
        }
        (void)cbv->set_buffer(size_slot, buffer, static_cast<unsigned>(offset));
        return true;
    };
    if (!bind_view_sizes(srv_view_sizes, shader.srv_view_size_slot) ||
        !bind_view_sizes(uav_view_sizes, shader.uav_view_size_slot) ||
        !bind_view_sizes(cbv_limits, shader.cbv_limit_slot) ||
        !bind_view_sizes(srv_null, shader.srv_null_slot) ||
        !bind_view_sizes(uav_null, shader.uav_null_slot) ||
        !bind_view_sizes(srv_typed_bias, shader.srv_typed_bias_slot) ||
        !bind_view_sizes(uav_typed_bias, shader.uav_typed_bias_slot)) {
        return false;
    }
    if (shader.heap_window.present) {
        // The heap window (CompiledShader::heap_window): the bound
        // CBV/SRV/UAV heap, and for each segment the slot its table range
        // starts at. Nothing is copied; only the heap's slots written since
        // the last draw are resolved. A segment this stage cannot see, or
        // whose table is unset or in another heap, gets base = limit and
        // drops out (an element no segment covers reads the sentinel).
        DescriptorHeap* heap = st.heaps[0];
        std::vector<u64> bases(shader.heap_window.segments.size(), ~u64{0});
        for (usize k = 0; k < shader.heap_window.segments.size(); ++k) {
            const auto& segment = shader.heap_window.segments[k];
            if (segment.parameter == CompiledShader::HeapWindow::kHeapStart) {
                bases[k] = 0;
                continue;
            }
            if (segment.parameter >= root->parameters.size()) continue;
            const BindingParameter& p = root->parameters[segment.parameter];
            const auto* arg = segment.parameter < args.size()
                                  ? &args[segment.parameter]
                                  : nullptr;
            if (p.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                !visible_to(p.visibility, stage) || arg == nullptr ||
                !arg->set) {
                continue;
            }
            u32 base = 0;
            DescriptorHeap* found = f.gpu_descriptor(arg->table, base);
            if (found == nullptr ||
                found->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                continue;
            }
            if (heap == nullptr) heap = found;
            if (found != heap) continue;
            bases[k] = static_cast<u64>(base) + segment.table_offset;
        }
        HeapWindowMirror* window = sync_heap_window(heap, shader.heap_window.aux);
        auto* cbv = rr.tables[stage][SET_CBV].get();
        if (window == nullptr || cbv == nullptr) return false;
        if (shader.heap_window.residency_slot != ~0u) {
            // Tiled status of the window's buffer elements: every tiled
            // buffer view of the heap, as its residency stands now.
            std::vector<u32> records;
            window_residency_words(heap, *window, records);
            graphics::BackendBuffer* buffer = nullptr;
            u64 offset = 0;
            if (!upload(records.data(), records.size() * sizeof(u32), buffer,
                        offset) ||
                !cbv->set_buffer(shader.heap_window.residency_slot, buffer,
                                 offset)) {
                return false;
            }
        }
        if (shader.heap_window.writes) window->table->declare_shader_writes();
        const u32 limit =
            heap != nullptr ? static_cast<u32>(heap->records.size()) : 0u;
        u32 words = 1;
        for (const auto& segment : shader.heap_window.segments) {
            words = std::max(words, segment.base_word + 1u);
        }
        std::vector<u32> params(std::max(words, 4u), 0u);
        params[0] = limit;
        for (usize k = 0; k < shader.heap_window.segments.size(); ++k) {
            params[shader.heap_window.segments[k].base_word] =
                static_cast<u32>(std::min<u64>(bases[k], limit));
        }
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (!upload(params.data(), params.size() * sizeof(u32), buffer, offset) ||
            !cbv->set_buffer(shader.heap_window.params_slot, buffer, offset) ||
            !cbv->set_buffer(shader.heap_window.side_slot,
                             window->side->buffer(), 0)) {
            return false;
        }
        out[graphics::BACKEND_DESCRIPTOR_SET_HEAP_WINDOW] = window->table.get();
    }
    if (shader.tiled_status) {
        // Tiled resources tier 2: what the real slots are bound to, for
        // their companions, status twins and residency records.
        TiledStatusSources tiled;
        const auto take = [&](u32 set, u32 count, std::vector<const Descriptor*>& records,
                              std::vector<u64>* vas) {
            records.assign(count, nullptr);
            if (vas != nullptr) vas->assign(count, 0u);
            for (u32 slot = 0; slot < count && slot < sources[set].size(); ++slot) {
                const Source& src = sources[set][slot];
                if (src.kind == Source::Kind::Record) records[slot] = src.record;
                if (src.kind == Source::Kind::View && vas != nullptr) {
                    (*vas)[slot] = src.va;
                }
            }
        };
        take(SET_SRV, shader.srv_companion_base, tiled.srv, &tiled.srv_va);
        take(SET_UAV, slots[SET_UAV], tiled.uav, &tiled.uav_va);
        tiled.samplers.assign(shader.status_sampler_count, nullptr);
        for (u32 slot = 0; slot < shader.status_sampler_count &&
                           slot < sources[SET_SAMPLER].size();
             ++slot) {
            const Source& src = sources[SET_SAMPLER][slot];
            if (src.kind == Source::Kind::Static) {
                tiled.samplers[slot] = src.sampler;
            } else if (src.kind == Source::Kind::Record && src.record != nullptr &&
                       src.record->type == DescriptorType::Sampler) {
                tiled.samplers[slot] = &src.record->sampler;
            }
        }
        if (!bind_tiled_status(stage, shader, tiled)) return false;
    }
    push[graphics::kPointMipMaskOffset / 4] = point_mip_mask;
    push[graphics::kPointMipNudgeMaskOffset / 4] = nudge_mask;
    return true;
}

bool Replayer::bind_tiled_status(u32 stage, const CompiledShader& shader,
                                 const TiledStatusSources& in) {
    using Dim = graphics::BackendTextureViewDimension;
    graphics::BackendDescriptorTable* srv_table = rr.tables[stage][SET_SRV].get();
    graphics::BackendDescriptorTable* sampler_table =
        rr.tables[stage][SET_SAMPLER].get();
    graphics::BackendDescriptorTable* cbv = rr.tables[stage][SET_CBV].get();
    // Residency companions, in the declared shape of each texture slot
    // (D3D10_SB_RESOURCE_DIMENSION: 2/3 1D/2D, 4 2DMS, 5 3D, 6 cube,
    // 7/8 1D/2D array, 9 2DMS array, 10 cube array; a Texture1D is the 2D
    // row the translator reads).
    const auto bind_companion = [&](u32 slot, const Descriptor* d, bool uav,
                                    u8 declared) {
        if (srv_table == nullptr || slot >= srv_table->slot_count()) return;
        if (declared == 0u || declared == 1u || declared > 10u) {
            srv_table->clear_slot(slot);
            return;
        }
        bool bound = false;
        if (declared == 5u) {
            // Volumes are never tiled (tiled resources tier 3).
            bound = srv_table->set_residency_texture_3d(slot);
        } else {
            const Dim dimension =
                declared == 4u ? Dim::Texture2DMS
                : declared == 9u ? Dim::Texture2DMSArray
                : declared == 6u ? (uav ? Dim::Texture2DArray : Dim::TextureCube)
                : declared == 10u ? (uav ? Dim::Texture2DArray : Dim::TextureCubeArray)
                : declared == 7u || declared == 8u ? Dim::Texture2DArray
                                                   : Dim::Texture2D;
            const bool ms = dimension == Dim::Texture2DMS ||
                            dimension == Dim::Texture2DMSArray;
            Resource* r = d != nullptr ? resource(d->resource, d->resource_serial)
                                       : nullptr;
            const graphics::BackendTexture2D* texture =
                r != nullptr && r->reserved && r->texture ? r->texture.get() : nullptr;
            graphics::BackendTextureViewDesc view{};
            view.dimension = dimension;
            unsigned samples = ms ? 4u : 1u;
            if (texture != nullptr) {
                view.most_detailed_mip = d->most_detailed_mip;
                view.mip_levels = uav ? 1u : d->mip_levels;
                view.first_array_slice = d->first_slice;
                view.array_size = d->array_size;
                samples = std::max(1u, texture->desc().sample_count);
            }
            bound = srv_table->set_residency_texture(slot, texture, view, samples);
        }
        if (!bound) {
            unimplemented("ExecuteCommandLists",
                          "a residency companion the backend cannot bind");
            srv_table->clear_slot(slot);
        }
    };
    for (u32 k = 0; k < in.srv.size(); ++k) {
        bind_companion(shader.srv_companion_base + k, in.srv[k], false,
                       k < shader.srv_slot_dimension.size()
                           ? shader.srv_slot_dimension[k]
                           : 0u);
    }
    for (u32 k = 0; k < in.uav.size(); ++k) {
        bind_companion(shader.uav_companion_base + k, in.uav[k], true,
                       k < shader.uav_slot_dimension.size()
                           ? shader.uav_slot_dimension[k]
                           : 0u);
    }
    // Status twins: s#'s filter and addressing with no comparison and no
    // min/max reduction (the plain filter weighs the same texels), and an
    // opaque white border, which reads as mapped.
    for (u32 k = 0; k < in.samplers.size(); ++k) {
        const u32 slot = shader.status_sampler_base + k;
        if (sampler_table == nullptr || slot >= sampler_table->slot_count()) break;
        if (in.samplers[k] == nullptr) {
            sampler_table->clear_slot(slot);
            continue;
        }
        D3DSamplerFields fields = sampler_fields(*in.samplers[k]);
        fields.filter &= ~(D3D12_FILTER_REDUCTION_TYPE_MASK
                           << D3D12_FILTER_REDUCTION_TYPE_SHIFT);
        graphics::BackendSamplerDesc twin{};
        d3d_backend_sampler(fields, twin, nullptr);
        twin.comparison_enabled = false;
        twin.border_color = 0xffffffffu;
        (void)sampler_table->set_sampler(slot, twin);
    }
    // Buffer residency records (graphics::kTiledResidency*): header word k
    // is slot k's record offset, 0 for a view of a resource that is not
    // tiled.
    const auto records = [&](const std::vector<const Descriptor*>& views,
                             const std::vector<u64>& vas) {
        std::vector<u32> words(std::max<usize>(views.size(),
                                                graphics::kTiledResidencyMinWords),
                               0u);
        for (u32 k = 0; k < views.size(); ++k) {
            const std::vector<u32>* bits = nullptr;
            u64 tiles = 0;
            u64 start = 0;
            u64 units = 0;
            u32 unit_bytes = 1;
            if (const Descriptor* d = views[k]; d != nullptr) {
                const u32 offset = append_buffer_residency(*d, words);
                words[k] = offset;
                continue;
            } else if (k < vas.size() && vas[k] != 0) {
                // A root SRV / UAV: raw or structured bytes from the VA to
                // the end of the resource.
                const ReservedVa* span = f.reserved_span(vas[k]);
                if (span == nullptr || !span->resident) continue;
                start = vas[k] - span->va;
                if (start >= span->resource_bytes) continue;
                units = span->resource_bytes - start;
                tiles = span->bytes / graphics::BACKEND_TILE_BYTES;
                bits = span->resident.get();
            } else {
                continue;
            }
            if (units > 0xFFFFFFFFull || tiles > 0xFFFFFFFFull) {
                unimplemented("ExecuteCommandLists",
                              "tiled status of a buffer view past 4 GiB");
                continue;
            }
            words[k] = static_cast<u32>(words.size());
            words.push_back(static_cast<u32>(tiles));
            words.push_back(static_cast<u32>(start / graphics::BACKEND_TILE_BYTES));
            words.push_back(static_cast<u32>(start % graphics::BACKEND_TILE_BYTES));
            words.push_back(static_cast<u32>(units));
            words.push_back(unit_bytes);
            const usize count = (static_cast<usize>(tiles) + 31u) / 32u;
            for (usize w = 0; w < count; ++w) {
                words.push_back(bits != nullptr && w < bits->size() ? (*bits)[w] : 0u);
            }
        }
        return words;
    };
    const auto bind_records = [&](const std::vector<u32>& words, u32 slot) {
        if (slot == ~0u || cbv == nullptr || slot >= cbv->slot_count()) return true;
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (!upload(words.data(), words.size() * sizeof(u32), buffer, offset)) {
            return false;
        }
        return cbv->set_buffer(slot, buffer, offset);
    };
    return bind_records(records(in.srv, in.srv_va), shader.srv_residency_slot) &&
           bind_records(records(in.uav, in.uav_va), shader.uav_residency_slot);
}

}  // namespace vitreo::winapi::d3d12
