// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_bind_hazards.cpp
/// @brief Binding-hazard tracking and the output-merger setter.
///
/// Which subresources a bound view covers, whether a read overlaps a bound
/// output, and the unbinding a new input or output binding forces;
/// set_output_merger, shared by OMSetRenderTargets and
/// OMSetRenderTargetsAndUnorderedAccessViews.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

namespace {

/// A volume's W slices are not subresources, so a 3D view covers every slice.
constexpr u32 kAllSlices = 0xFFFFFFFFu;

bool windows_overlap(u32 first_a, u32 count_a, u32 first_b, u32 count_b) {
    return static_cast<u64>(first_a) < static_cast<u64>(first_b) + count_b &&
           static_cast<u64>(first_b) < static_cast<u64>(first_a) + count_a;
}

// DXGI_FORMAT_X32_TYPELESS_G8X24_UINT and DXGI_FORMAT_X24_TYPELESS_G8_UINT: the
// SRV formats that read the stencil plane of a depth-stencil texture.
bool is_stencil_plane_format(u32 format) {
    return format == 22u || format == 47u;
}

} // namespace

bool D3D11Manager::subresources_overlap(const BoundSubresources& a,
                                        const BoundSubresources& b) {
    return a.resource != 0 && a.resource == b.resource &&
           (a.planes & b.planes) != 0 &&
           windows_overlap(a.first_mip, a.mip_count, b.first_mip,
                           b.mip_count) &&
           windows_overlap(a.first_slice, a.slice_count, b.first_slice,
                           b.slice_count);
}

D3D11Manager::BoundSubresources D3D11Manager::buffer_subresources(
    GuestAddr buffer) {
    // A buffer is one subresource, whatever element window a view selects
    // (measured: a UAV of elements 0-63 unbinds a VB at any offset).
    if (buffer == 0) return {};
    return {buffer, 0, 1, 0, 1, kPlaneDepthOrColour};
}

D3D11Manager::BoundSubresources D3D11Manager::srv_subresources(
    GuestAddr view) const {
    const auto it = m_shader_resource_views.find(view);
    if (it == m_shader_resource_views.end()) return {};
    const ShaderResourceView& srv = it->second;
    if (srv.buffer != 0) return buffer_subresources(srv.buffer);
    if (srv.texture == 0) return {};
    BoundSubresources set{srv.texture,       srv.most_detailed_mip,
                          srv.mip_levels,    srv.first_array_slice,
                          srv.array_size,    kPlaneDepthOrColour};
    if (srv.view_dimension == D3D11_SRV_DIMENSION_TEXTURE3D) {
        set.first_slice = 0;
        set.slice_count = kAllSlices;
    }
    if (is_stencil_plane_format(srv.format)) set.planes = kPlaneStencil;
    return set;
}

D3D11Manager::BoundSubresources D3D11Manager::rtv_subresources(
    GuestAddr view) const {
    const auto it = m_render_target_views.find(view);
    if (it == m_render_target_views.end()) return {};
    const RenderTargetView& rtv = it->second;
    if (rtv.buffer != 0) return buffer_subresources(rtv.buffer);
    if (rtv.texture == 0) return {};
    // A Texture3D RTV keeps FirstWSlice/WSize in the slice fields; those are
    // not subresources (measured: an RTV of W slice 2 unbinds the volume SRV).
    if (rtv.view_dimension == D3D11_RTV_DIMENSION_TEXTURE3D) {
        return {rtv.texture, rtv.mip_slice, 1, 0, kAllSlices,
                kPlaneDepthOrColour};
    }
    return {rtv.texture,           rtv.mip_slice,  1,
            rtv.first_array_slice, rtv.array_size, kPlaneDepthOrColour};
}

D3D11Manager::BoundSubresources D3D11Manager::uav_subresources(
    GuestAddr view) const {
    const auto it = m_unordered_access_views.find(view);
    if (it == m_unordered_access_views.end()) return {};
    const UnorderedAccessView& uav = it->second;
    if (uav.buffer != 0) return buffer_subresources(uav.buffer);
    if (uav.texture == 0) return {};
    if (uav.view_dimension == D3D11_UAV_DIMENSION_TEXTURE3D) {
        return {uav.texture, uav.mip_slice, 1, 0, kAllSlices,
                kPlaneDepthOrColour};
    }
    // Array views carry their slice window; a plain 1D/2D view is slice 0.
    return {uav.texture,           uav.mip_slice,  1,
            uav.first_array_slice, uav.array_size, kPlaneDepthOrColour};
}

D3D11Manager::BoundSubresources D3D11Manager::dsv_written_subresources(
    GuestAddr view) const {
    const auto it = m_depth_stencil_views.find(view);
    if (it == m_depth_stencil_views.end() || it->second.texture == 0) {
        return {};
    }
    const DepthStencilView& dsv = it->second;
    // Measured (hazard rows g/h): READ_ONLY_DEPTH leaves depth-plane SRVs
    // bound and bindable, READ_ONLY_STENCIL the stencil-plane ones.
    u32 planes = 0;
    if ((dsv.flags & d3d11::DSV_READ_ONLY_DEPTH) == 0) {
        planes |= kPlaneDepthOrColour;
    }
    if ((dsv.flags & d3d11::DSV_READ_ONLY_STENCIL) == 0) planes |= kPlaneStencil;
    return {dsv.texture,           dsv.mip_slice,  1,
            dsv.first_array_slice, dsv.array_size, planes};
}

bool D3D11Manager::bound_output_overlaps(const ContextState& state,
                                         const BoundSubresources& read) const {
    if (read.resource == 0) return false;
    for (const GuestAddr rtv : state.render_targets) {
        if (rtv != 0 && subresources_overlap(rtv_subresources(rtv), read)) {
            return true;
        }
    }
    if (state.depth_stencil_view != 0 &&
        subresources_overlap(dsv_written_subresources(state.depth_stencil_view),
                             read)) {
        return true;
    }
    for (const auto* uavs : {&state.output_merger_unordered_access_views,
                             &state.compute_unordered_access_views}) {
        for (const GuestAddr uav : *uavs) {
            if (uav != 0 && subresources_overlap(uav_subresources(uav), read)) {
                return true;
            }
        }
    }
    for (const GuestAddr target : state.stream_out_targets) {
        if (target != 0 &&
            subresources_overlap(buffer_subresources(target), read)) {
            return true;
        }
    }
    return false;
}

void D3D11Manager::unbind_inputs_overlapping(
    ContextState& state, std::span<const BoundSubresources> written) {
    if (written.empty()) return;
    const auto hit = [&](const BoundSubresources& read) {
        if (read.resource == 0) return false;
        for (const BoundSubresources& output : written) {
            if (subresources_overlap(output, read)) return true;
        }
        return false;
    };
    // Every stage, including ones whose shader is not bound: Windows clears
    // the slot itself (measured on VS/HS/DS/GS/PS/CS alike).
    for (auto* views :
         {&state.vertex_shader_resources, &state.hull_shader_resources,
          &state.domain_shader_resources, &state.geometry_shader_resources,
          &state.pixel_shader_resources, &state.compute_shader_resources}) {
        for (usize slot = 0; slot < views->size(); ++slot) {
            const GuestAddr view = (*views)[slot];
            if (view == 0 || !hit(srv_subresources(view))) continue;
            views->set(slot, 0);
            (void)release_shader_resource_view(view);
        }
    }
    // An unbound vertex or index buffer loses its stride/offset/format too
    // (measured: "m cs uavB bound: vb0: - strides 0 offsets 0", "ib: - fmt 0").
    for (usize slot = 0; slot < state.vertex_buffers.size(); ++slot) {
        const GuestAddr buffer = state.vertex_buffers[slot];
        if (buffer == 0 || !hit(buffer_subresources(buffer))) continue;
        state.vertex_buffers.set(slot, 0);
        state.vertex_buffer_strides[slot] = 0;
        state.vertex_buffer_offsets[slot] = 0;
        (void)release_buffer(buffer);
    }
    if (state.index_buffer != 0 &&
        hit(buffer_subresources(state.index_buffer))) {
        const GuestAddr buffer = state.index_buffer;
        state.index_buffer = 0;
        state.index_buffer_format = 0;
        state.index_buffer_offset = 0;
        (void)release_buffer(buffer);
    }
}

void D3D11Manager::unbind_outputs_overlapping(ContextState& state,
                                              const BoundSubresources& written,
                                              u32 classes) {
    if (written.resource == 0) return;
    if ((classes & kOutputRenderTargets) != 0) {
        for (usize slot = 0; slot < state.render_targets.size(); ++slot) {
            const GuestAddr rtv = state.render_targets[slot];
            if (rtv == 0 ||
                !subresources_overlap(rtv_subresources(rtv), written)) {
                continue;
            }
            state.render_targets.set(slot, 0);
            (void)release_render_target_view(rtv);
        }
    }
    const auto clear_uavs = [&](auto& uavs) {
        for (GuestAddr& uav : uavs) {
            if (uav == 0 ||
                !subresources_overlap(uav_subresources(uav), written)) {
                continue;
            }
            const GuestAddr previous = uav;
            uav = 0;
            (void)release_unordered_access_view(previous);
        }
    };
    if ((classes & kOutputMergerUavs) != 0) {
        clear_uavs(state.output_merger_unordered_access_views);
    }
    if ((classes & kOutputComputeUavs) != 0) {
        clear_uavs(state.compute_unordered_access_views);
    }
    if ((classes & kOutputStreamTargets) != 0) {
        for (GuestAddr& target : state.stream_out_targets) {
            if (target == 0 ||
                !subresources_overlap(buffer_subresources(target), written)) {
                continue;
            }
            const GuestAddr previous = target;
            target = 0;
            (void)release_buffer(previous);
        }
    }
}

void D3D11Manager::set_output_merger(GuestAddr context, u32 num_rtvs,
                                     GuestAddr rtvs, GuestAddr dsv,
                                     u32 uav_start, u32 num_uavs,
                                     GuestAddr uavs, GuestAddr initial_counts) {
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL and
    // D3D11_KEEP_UNORDERED_ACCESS_VIEWS (d3d11.h): 0xffffffff.
    constexpr u32 kKeep = 0xFFFFFFFFu;
    constexpr u32 kRenderTargetSlots = 8; // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
    const bool keep_rtvs = num_rtvs == kKeep;
    const bool keep_uavs = num_uavs == kKeep;
    if (keep_rtvs && keep_uavs) return;
    const auto owner_it = m_interfaces.find(context);
    const GuestAddr owner =
        owner_it != m_interfaces.end() ? owner_it->second.owner : 0;
    auto& state = m_context_states[context];
    const usize ptr_size = m_is_64bit ? 8u : 4u;

    std::array<GuestAddr, kRenderTargetSlots> new_rtvs{};
    GuestAddr new_dsv = 0;
    std::array<GuestAddr, d3d11::UAV_SLOTS> new_uavs{};
    // Index into ppUnorderedAccessViews (and pUAVInitialCounts) per u# slot.
    std::array<u32, d3d11::UAV_SLOTS> uav_index{};
    if (!keep_rtvs) {
        // More than eight is undefined (Windows crashed on nine); the first
        // eight are what any caller can mean.
        u32 rtv_count = std::min(num_rtvs, kRenderTargetSlots);
        // Measured: when UAVs are bound in the same call, render targets from
        // UAVStartSlot on are not bound -- those slots belong to the UAVs
        // (Learn: UAVStartSlot "should be equal to the number of render-target
        // views"). Such an RTV is discarded before the conflict check, and
        // with NumUAVs 0 every RTV binds.
        if (!keep_uavs && num_uavs > 0) rtv_count = std::min(rtv_count, uav_start);
        for (u32 i = 0; i < rtv_count && rtvs != 0; ++i) {
            GuestAddr address = 0;
            if (!read_pointer(rtvs + i * ptr_size, address) || address == 0) {
                continue;
            }
            const auto view = m_render_target_views.find(address);
            if (view == m_render_target_views.end() ||
                view->second.device_owner != owner) {
                continue;
            }
            new_rtvs[i] = address;
        }
        if (dsv != 0) {
            const auto view = m_depth_stencil_views.find(dsv);
            if (view != m_depth_stencil_views.end() &&
                view->second.device_owner == owner) {
                new_dsv = dsv;
            }
        }
    }
    if (!keep_uavs) {
        for (u32 i = 0; i < num_uavs && uavs != 0; ++i) {
            // u# slots past the feature level's eight are ignored, and the call
            // is not dropped for them (measured: start 7 with two UAVs binds
            // u7; start 8 binds nothing and still clears every other slot).
            const u64 target = static_cast<u64>(uav_start) + i;
            if (target >= new_uavs.size()) break;
            GuestAddr address = 0;
            if (!read_pointer(uavs + i * ptr_size, address) || address == 0) {
                continue;
            }
            const auto view = m_unordered_access_views.find(address);
            if (view == m_unordered_access_views.end() ||
                view->second.device_owner != owner) {
                continue;
            }
            new_uavs[target] = address;
            uav_index[target] = i;
        }
    }

    // Validation: Learn -- "Two RTVs conflict if they share a subresource",
    // two UAVs likewise, and "An RTV conflicts with a UAV if they share a
    // subresource". A call with a conflict is dropped whole: every binding,
    // including the half that was valid, stays as it was (measured, for
    // duplicate RTVs, duplicate UAVs, two views of one subresource, and an
    // RTV/UAV pair).
    std::array<BoundSubresources, kRenderTargetSlots + d3d11::UAV_SLOTS + 1>
        written{};
    usize written_count = 0;
    for (const GuestAddr rtv : new_rtvs) {
        if (rtv != 0) written[written_count++] = rtv_subresources(rtv);
    }
    const usize rtv_written = written_count;
    for (const GuestAddr uav : new_uavs) {
        if (uav != 0) written[written_count++] = uav_subresources(uav);
    }
    for (usize a = 0; a < written_count; ++a) {
        for (usize b = a + 1; b < written_count; ++b) {
            if (subresources_overlap(written[a], written[b])) {
                d3d11_diag("ID3D11DeviceContext::OMSetRenderTargets%s: two "
                           "outputs share a subresource -> call dropped",
                           num_uavs != 0 ? "AndUnorderedAccessViews" : "");
                return;
            }
        }
    }
    // Learn: all render targets and the depth-stencil view "must have the same
    // size in all dimensions" and the same sample count. Measured on the
    // VIEW's extent: a 32x32 texture's mip 1 matches a 16x16 DSV; array size
    // 1 matches a Texture2D; 2 slices against 1, 4x against 1x, and 16x16
    // against 32x32 all drop the call. UAVs are not size-checked.
    if (!keep_rtvs) {
        struct Extent {
            u32 width, height, array_size, samples;
            bool operator==(const Extent&) const = default;
        };
        const auto extent_of = [&](GuestAddr texture, u32 mip,
                                   u32 array_size) -> std::optional<Extent> {
            const auto t = m_textures.find(texture);
            if (t == m_textures.end()) return std::nullopt;
            const u32 width = t->second.volume ? t->second.volume_desc.width
                                               : t->second.desc.width;
            const u32 height = t->second.volume ? t->second.volume_desc.height
                                                : t->second.desc.height;
            return Extent{std::max(1u, width >> mip),
                          std::max(1u, height >> mip), array_size,
                          std::max(1u, t->second.desc.sample_count)};
        };
        std::optional<Extent> reference;
        bool mismatch = false;
        const auto check = [&](std::optional<Extent> extent) {
            if (!extent) return;
            if (!reference) reference = extent;
            else if (!(*reference == *extent)) mismatch = true;
        };
        for (const GuestAddr rtv : new_rtvs) {
            if (rtv == 0) continue;
            const auto& view = m_render_target_views.at(rtv);
            if (view.buffer != 0) {
                // A buffer view renders as NumElements x 1.
                check(Extent{view.num_elements, 1u, 1u, 1u});
                continue;
            }
            check(extent_of(view.texture, view.mip_slice, view.array_size));
        }
        if (new_dsv != 0) {
            const auto& view = m_depth_stencil_views.at(new_dsv);
            check(extent_of(view.texture, view.mip_slice, view.array_size));
        }
        if (mismatch) {
            d3d11_diag("ID3D11DeviceContext::OMSetRenderTargets%s: render "
                       "targets / depth-stencil differ in size, array size or "
                       "sample count -> call dropped",
                       num_uavs != 0 ? "AndUnorderedAccessViews" : "");
            return;
        }
    }

    auto final_rtvs = keep_rtvs ? state.render_targets.values() : new_rtvs;
    const GuestAddr final_dsv = keep_rtvs ? state.depth_stencil_view : new_dsv;
    auto final_uavs =
        keep_uavs ? state.output_merger_unordered_access_views : new_uavs;
    const std::span<const BoundSubresources> new_rtv_sets(written.data(),
                                                          rtv_written);
    const std::span<const BoundSubresources> new_uav_sets(
        written.data() + rtv_written, written_count - rtv_written);
    const auto overlaps_any = [](const BoundSubresources& set,
                                 std::span<const BoundSubresources> others) {
        for (const BoundSubresources& other : others) {
            if (subresources_overlap(set, other)) return true;
        }
        return false;
    };
    if (keep_rtvs) {
        // Learn case 2 (NumRTVs == KEEP): "All RTVs in slots >= UAVStartSlot"
        // and "All RTVs that conflict with any UAVs in ppUnorderedAccessView"
        // are unbound; pDepthStencilView is ignored and the DSV stays.
        for (u32 slot = 0; slot < final_rtvs.size(); ++slot) {
            if (final_rtvs[slot] == 0) continue;
            if ((num_uavs > 0 && slot >= uav_start) ||
                overlaps_any(rtv_subresources(final_rtvs[slot]),
                             new_uav_sets)) {
                final_rtvs[slot] = 0;
            }
        }
    }
    if (keep_uavs) {
        // Learn case 3 (NumUAVs == KEEP): "All UAVs in slots < NumRTVs" and
        // "All UAVs that conflict with any RTVs in ppRenderTargetViews" are
        // unbound; UAVStartSlot is ignored.
        for (u32 slot = 0; slot < final_uavs.size(); ++slot) {
            if (final_uavs[slot] == 0) continue;
            if (slot < num_rtvs ||
                overlaps_any(uav_subresources(final_uavs[slot]),
                             new_rtv_sets)) {
                final_uavs[slot] = 0;
            }
        }
    }

    // Reference the new set before releasing the old one: a view that stays
    // bound must not reach zero in between.
    for (const GuestAddr rtv : final_rtvs) {
        if (rtv != 0) ++m_render_target_views.at(rtv).ref_count;
    }
    if (final_dsv != 0) ++m_depth_stencil_views.at(final_dsv).ref_count;
    for (const GuestAddr uav : final_uavs) {
        if (uav != 0) ++m_unordered_access_views.at(uav).ref_count;
    }
    const auto previous_rtvs = state.render_targets;
    const GuestAddr previous_dsv = state.depth_stencil_view;
    const auto previous_uavs = state.output_merger_unordered_access_views;
    state.render_targets.assign(final_rtvs);
    state.depth_stencil_view.set(final_dsv);
    state.output_merger_unordered_access_views = final_uavs;
    // Learn case 1: the call "Unbinds all currently bound conflicting
    // resources (stream-output target resources (SOTargets), compute shader
    // (CS) UAVs, shader-resource views (SRVs))". Measured for CS UAVs in both
    // directions, and for RTVs as well as OM UAVs.
    for (usize i = 0; i < written_count; ++i) {
        unbind_outputs_overlapping(state, written[i],
                                   kOutputComputeUavs | kOutputStreamTargets);
    }
    for (const GuestAddr rtv : previous_rtvs) {
        if (rtv != 0) (void)release_render_target_view(rtv);
    }
    if (previous_dsv != 0) (void)release_depth_stencil_view(previous_dsv);
    for (const GuestAddr uav : previous_uavs) {
        if (uav != 0) (void)release_unordered_access_view(uav);
    }
    if (new_dsv != 0) {
        written[written_count++] = dsv_written_subresources(new_dsv);
    }
    unbind_inputs_overlapping(
        state, std::span<const BoundSubresources>(written.data(), written_count));
    // A hidden counter is the view's, not the context's: a deferred context
    // sets it when its list executes (the replayed call), not now.
    for (u32 slot = 0; slot < new_uavs.size() && !is_deferred_context(context);
         ++slot) {
        if (new_uavs[slot] != 0) {
            apply_initial_count(initial_counts, uav_index[slot], new_uavs[slot]);
        }
    }
}

} // namespace vitreo::winapi
