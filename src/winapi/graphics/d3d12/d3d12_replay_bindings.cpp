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

#include <algorithm>
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
    return stage == STAGE_VERTEX ? visibility == D3D12_SHADER_VISIBILITY_VERTEX
                                 : visibility == D3D12_SHADER_VISIBILITY_PIXEL;
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
                        const Descriptor& d, bool uav) {
    Resource* r = resource(d.resource, d.resource_serial);
    if (r == nullptr) {
        // A null descriptor, or one whose resource is gone: reads 0.
        t.clear_slot(slot);
        return 0;
    }
    if (r->is_buffer()) {
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
            const u32 element = dxgi_element_bytes(d.format);
            if (element == 0 || r->offset % element != 0) {
                unimplemented("ExecuteCommandLists",
                              "a typed buffer view of this format");
                t.clear_slot(slot);
                return 0;
            }
            const u64 first = r->offset / element + d.first_element;
            const u16 mapping = uav ? u16{0} : component_mapping(d);
            const auto key = std::make_tuple(d.format, first, d.num_elements,
                                             uav, mapping);
            auto found = r->typed_views.find(key);
            if (found == r->typed_views.end()) {
                auto view =
                    mapping != 0
                        ? f.m_backend.create_mapped_texture_buffer(
                              *buffer, graphics::pixel_format_from_dxgi(d.format),
                              static_cast<unsigned>(first), d.num_elements,
                              mapping)
                        : f.m_backend.create_texture_buffer(
                              *buffer, graphics::pixel_format_from_dxgi(d.format),
                              static_cast<unsigned>(first), d.num_elements, uav);
                if (!view) {
                    if (mapping != 0) {
                        unimplemented("ExecuteCommandLists",
                                      "a Shader4ComponentMapping the backend "
                                      "cannot apply to a typed buffer");
                    }
                    t.clear_slot(slot);
                    return 0;
                }
                found = r->typed_views.emplace(key, std::move(view)).first;
            }
            if (found->second->element_bias() != 0) {
                // The texture starts below the view's first element
                // (BackendTextureBuffer::element_bias), and a
                // descriptor-table translation applies no bias
                // (DxbcTranslationOptions::typed_buffer_bias is D3D11's).
                unimplemented("ExecuteCommandLists",
                              "a typed buffer view whose first element is not "
                              "on the backend's texture alignment");
                t.clear_slot(slot);
                return 0;
            }
            (void)t.set_texture_buffer(slot, found->second.get());
            return 0;
        }
        const u64 stride = raw ? 4u : d.structure_stride;
        const u64 offset = r->offset + d.first_element * stride;
        if (offset > 0xFFFFFFFFull) {
            unimplemented("ExecuteCommandLists",
                          "a buffer view more than 4 GiB into its heap");
            t.clear_slot(slot);
            return 0;
        }
        (void)t.set_buffer(slot, buffer, static_cast<unsigned>(offset));
        const u64 bytes = static_cast<u64>(d.num_elements) * stride;
        return static_cast<u32>(std::min<u64>(bytes / 4u, 0xFFFFFFFFull));
    }
    const u16 mapping = uav ? u16{0} : component_mapping(d);
    const graphics::PixelFormat view_format =
        graphics::pixel_format_from_dxgi(d.format);
    if (r->texture3d) {
        if (mapping != 0) {
            unimplemented("ExecuteCommandLists",
                          "a Shader4ComponentMapping on a 3D texture view");
            t.clear_slot(slot);
            return 0;
        }
        auto* volume = r->texture3d->view(
            d.format == r->desc.format ? graphics::PixelFormat::Unknown
                                       : view_format,
            d.most_detailed_mip, uav ? 1u : d.mip_levels);
        if (volume == nullptr) {
            t.clear_slot(slot);
            return 0;
        }
        (void)t.set_texture_3d(slot, volume);
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
    (void)t.set_texture(slot, r->texture.get(), view);
    return 0;
}

bool Replayer::bind_stage(
    ReplayState& st, u32 stage, const CompiledShader& shader,
    std::array<const graphics::BackendDescriptorTable*, 4>& out,
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
    // What each register of each class is bound to by the root signature.
    struct Source {
        enum class Kind : u8 { None, Record, View, Constants, Static } kind{};
        const Descriptor* record{nullptr};
        u64 va{0};
        u32 parameter{0};
        const SamplerState* sampler{nullptr};
    };
    std::array<std::vector<Source>, 4> sources;
    for (u32 set = 0; set < 4; ++set) sources[set].resize(slots[set]);
    const auto& args = st.args[bind_point];
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
    for (const BindingSampler& s : root->static_samplers) {
        const u32 slot =
            shader.slot_of(SET_SAMPLER, s.space, s.shader_register);
        if (slot < slots[SET_SAMPLER] && visible_to(s.visibility, stage)) {
            sources[SET_SAMPLER][slot] = {Source::Kind::Static, nullptr, 0, 0,
                                          &s.state};
        }
    }
    // A 5.1 shader reads its samplers' MipLODBias / POINT-mip state from
    // records in the CBV table (DxbcTranslationOptions::sampler_state_buffer),
    // not from the push block.
    std::vector<u32> sampler_records;
    if (shader.table_mode && shader.sampler_state_slot < slots[SET_CBV]) {
        sampler_records.assign(
            std::max(1u, slots[SET_SAMPLER]) * graphics::kSamplerStateRecordWords,
            0u);
    }
    u32 point_mip_mask = 0;
    u32 nudge_mask = 0;
    for (u32 set = 0; set < 4; ++set) {
        if (slots[set] == 0) continue;
        graphics::BackendDescriptorTable* t =
            table(stage, set, slots[set], classes[set]);
        if (t == nullptr) return false;
        for (u32 slot = 0; slot < slots[set]; ++slot) {
            const Source& src = sources[set][slot];
            switch (src.kind) {
            case Source::Kind::None:
                t->clear_slot(slot);
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
                    break;
                }
                (void)t->set_buffer(slot, buffer, static_cast<unsigned>(offset));
                break;
            }
            case Source::Kind::View: {
                graphics::BackendBuffer* buffer = nullptr;
                u64 offset = 0;
                if (!f.resolve_va(src.va, 0, buffer, offset) ||
                    offset > 0xFFFFFFFFull) {
                    t->clear_slot(slot);
                    break;
                }
                (void)t->set_buffer(slot, buffer, static_cast<unsigned>(offset));
                break;
            }
            case Source::Kind::Static:
            case Source::Kind::Record: {
                if (set == SET_SAMPLER) {
                    const SamplerState* state = src.sampler;
                    if (src.kind == Source::Kind::Record) {
                        state = src.record->type == DescriptorType::Sampler
                                    ? &src.record->sampler
                                    : nullptr;
                    }
                    if (state == nullptr) {
                        t->clear_slot(slot);
                        break;
                    }
                    graphics::BackendSamplerDesc sampler{};
                    float bias = 0.0f;
                    d3d_backend_sampler(sampler_fields(*state), sampler, &bias);
                    (void)t->set_sampler(slot, sampler);
                    if (!sampler_records.empty()) {
                        u32* record = sampler_records.data() +
                                      slot * graphics::kSamplerStateRecordWords;
                        std::memcpy(record + graphics::kSamplerStateBiasWord,
                                    &bias, 4);
                        record[graphics::kSamplerStateFlagsWord] =
                            (d3d_point_mip_filter(state->filter)
                                 ? graphics::kSamplerStatePointMip
                                 : 0u) |
                            (d3d_point_mip_tie_nudged(state->filter)
                                 ? graphics::kSamplerStateBiasNudged
                                 : 0u);
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
                                      offset) ||
                        offset > 0xFFFFFFFFull) {
                        t->clear_slot(slot);
                        break;
                    }
                    (void)t->set_buffer(slot, buffer,
                                        static_cast<unsigned>(offset));
                    break;
                }
                const bool uav = set == SET_UAV;
                if (d.type != (uav ? DescriptorType::Uav : DescriptorType::Srv)) {
                    t->clear_slot(slot);
                    break;
                }
                const u32 words = bind_view(*t, slot, d, uav);
                // The size table: t0..t127, then u0..u7 (dxbc_to_spirv.hpp
                // kGuardSizeTableOffset); a slot past it is unguarded.
                const u32 entry = uav ? graphics::kGuardSizeSrvSlots + slot : slot;
                if ((uav ? slot < graphics::kGuardSizeUavSlots
                         : slot < graphics::kGuardSizeSrvSlots)) {
                    push[graphics::kGuardSizeTableOffset / 4 + entry] = words;
                }
                break;
            }
            }
        }
        out[set] = t;
    }
    if (!sampler_records.empty()) {
        // After the sampler table is resolved: the records are complete.
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        auto* cbv = rr.tables[stage][SET_CBV].get();
        if (cbv != nullptr &&
            upload(sampler_records.data(), sampler_records.size() * 4u, buffer,
                   offset)) {
            (void)cbv->set_buffer(shader.sampler_state_slot, buffer,
                                  static_cast<unsigned>(offset));
        }
    }
    push[graphics::kPointMipMaskOffset / 4] = point_mip_mask;
    push[graphics::kPointMipNudgeMaskOffset / 4] = nudge_mask;
    return true;
}

}  // namespace vitreo::winapi::d3d12
