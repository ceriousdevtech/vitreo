// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_device_ext.cpp
/// @brief The ID3D11Device1-5 / ID3D11DeviceContext1-4 surface beyond the
///        core methods: the `*1` creators, counters, tiling queries,
///        annotations, Flush1, hardware protection, device-removed events.
///
/// Every behaviour here is either measured on Windows
/// (tools/oracle_probe/d3d11_interface_surface_probe.c) or follows from what
/// this device reports through CheckFeatureSupport: no logic ops, tiled
/// resources at Tier 1 (d3d11_tiled.cpp), no map-on-default textures, no
/// standard swizzle, no counters. A
/// method whose valid use depends on one of those answers the way a Windows
/// driver reporting the same caps does, which for most is E_INVALIDARG.
///
/// Where a `*1` creator only extends its base descriptor -- the new fields come
/// after the old ones and the argument positions match -- it validates the new
/// fields and hands the call to the base implementation unchanged.
///
/// Split out of this file, each by the interface it serves:
/// d3d11_format_caps_context.cpp (the cached backend format caps),
/// d3d11_dxgi_surface.cpp and d3d11_dxgi_surface_facets.cpp (the
/// QueryInterface graph and IDXGISurface2), d3d11_device_context_state.cpp
/// (ID3DDeviceContextState), d3d11_context_clear_view.cpp (ClearView),
/// d3d11_context_annotations.cpp (debug markers), d3d11_multithread.cpp
/// (ID3D11Multithread), d3d11_class_linkage.cpp and
/// d3d11_class_instance_binding.cpp (class linkage and class instances).

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 DXGI_ERROR_INVALID_CALL_HR = 0x887A0001u;
// D3D11_CONTEXT_TYPE_VIDEO, the largest valid context type.
constexpr u32 CONTEXT_TYPE_MAX = 4;

}  // namespace

bool D3D11Manager::device_ext_method(Marshaler& m, usize slot,
                                     GuestAddr self) {
    switch (slot) {
    case 26: {
        // CreateCounter(pCounterDesc, ppCounter). This device exposes no
        // counters (CheckCounterInfo: LastDeviceDependentCounter 0), and
        // Windows answers such a request with E_INVALIDARG and leaves the
        // out-parameter untouched -- measured.
        m.set_return(d3d11::E_INVALIDARG);
        return true;
    }
    case 31: {
        // CheckCounterInfo(pCounterInfo): {LastDeviceDependentCounter,
        // NumSimultaneousCounters, UINT8 NumDetectableParallelUnits}. Windows
        // reports {0, 0, 1} on a device with no counters -- measured.
        const GuestAddr info = m.arg_ptr(1);
        if (info != 0) {
            (void)m_memory.write_u32(info, 0);
            (void)m_memory.write_u32(info + 4, 0);
            (void)m_memory.write_u8(info + 8, 1);
        }
        m.set_return(0);
        return true;
    }
    case 32:
        // CheckCounter: E_INVALIDARG with every output untouched -- measured.
        m.set_return(d3d11::E_INVALIDARG);
        return true;

    case 44:   // CreateDeferredContext1
    case 51:   // CreateDeferredContext2
    case 62:   // CreateDeferredContext3
        // (ContextFlags, ppDeferredContext): the base method's argument
        // layout exactly, and the one object serves every context version.
        impl_DeviceMethod(m, 27);
        return true;

    case 45:
        create_blend_state1(m, self);
        return true;
    case 47:
        create_device_context_state(m, self);
        return true;

    case 46: {
        // CreateRasterizerState1(pDesc1, ppState). DESC1 is the 40-byte DESC
        // plus ForcedSampleCount; the DESC2 rules (rasterizer_desc_valid)
        // with ConservativeRaster OFF. A forced sample count is honoured
        // where it has an effect this device allows: a draw with no render
        // target and no depth-stencil view rasterizes at that count (the
        // attachment-less pass, d3d11_pipeline.cpp), and a forced single
        // sample over single-sample targets rasterizes as it would unforced.
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr state_out = m.arg_ptr(2);
        if (state_out != 0) (void)m.write_ptr(state_out, 0);
        RasterizerStateDesc desc{};
        const Device* device = find_device(self);
        if (desc_address == 0 ||
            m_memory.read_bytes(desc_address, reinterpret_cast<Byte*>(&desc),
                                44).is_err() ||
            !rasterizer_desc_valid(
                desc, device != nullptr ? device->feature_level
                                        : d3d11::FEATURE_LEVEL_11_0)) {
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        if (state_out == 0) {
            m.set_return(1u);
            return true;
        }
        GuestAddr state = 0;
        const u32 hr = acquire_rasterizer_state(
            m_interfaces.find(self)->second.owner, desc, state);
        if (hr != d3d11::S_OK) {
            m.set_return(hr);
            return true;
        }
        if (m.write_ptr(state_out, state).is_err()) {
            (void)release_object(state);
            m.set_return(d3d11::E_POINTER);
            return true;
        }
        m.set_return(d3d11::S_OK);
        return true;
    }

    case 49:
        // OpenSharedResourceByName(lpName, dwDesiredAccess, riid,
        // ppResource): a name IDXGIResource1::CreateSharedHandle gave an NT
        // handle, in any process (d3d11_sharing.cpp).
        m.set_return(open_shared_resource(m, self, SharedHandleKind::Name));
        return true;

    case 52:
        // GetResourceTiling (d3d11_tiled.cpp).
        get_resource_tiling(m);
        return true;

    case 53: {
        // CheckMultisampleQualityLevels1(Format, SampleCount, Flags, pLevels).
        // Unknown flag bits are ignored (measured). With TILED_RESOURCE the
        // levels are those of a tiled texture: single sample and 4x on a
        // FORMAT_SUPPORT2_TILED format (measured WARP: x1 and x4 only, none
        // for D24S8 / D32S8X24, x1 for BC1), none without tiled resources.
        const CapsQueryLog caps_log(
            m, CapsQueryLog::Kind::CheckMultisampleQualityLevels1);
        const u32 format = static_cast<u32>(m.arg(1));
        const u32 samples = static_cast<u32>(m.arg(2));
        const u32 flags = static_cast<u32>(m.arg(3));
        const GuestAddr levels_out = m.arg_ptr(4);
        if (levels_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        const Device* device = find_device(self);
        u32 levels = 0;
        const u32 hr = d3d11::check_multisample_quality_levels(
            format, samples,
            device != nullptr
                ? format_caps_context(*device)
                : format_caps_context(d3d11::FEATURE_LEVEL_11_0),
            levels);
        if (hr == d3d11::E_INVALIDARG) {
            m.set_return(hr);
            return true;
        }
        if ((flags &
             D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE) !=
            0) {
            const auto context = device != nullptr
                                     ? format_caps_context(*device)
                                     : format_caps_context(
                                           d3d11::FEATURE_LEVEL_11_0);
            const bool tiled =
                context.tiled_resources &&
                context.feature_level >= d3d11::FEATURE_LEVEL_11_0 &&
                format < context.tiled_format.size() &&
                (samples == 1 ? context.tiled_format[format]
                 : samples == 4 ? context.tiled_format_4x[format]
                                : false);
            if (!tiled) levels = 0;
        }
        (void)m_memory.write_u32(levels_out, levels);
        m.set_return(hr);
        return true;
    }

    case 54:
    case 55: {
        // CreateTexture2D1 / CreateTexture3D1(pDesc1, pInitialData, ppTexture).
        // DESC1 appends TextureLayout to the base descriptor. The base
        // creator's body validates it with the rest of the descriptor
        // (texture_desc_violation: a layout other than UNDEFINED needs CPU
        // access on a DEFAULT texture, ROW_MAJOR is one plain mip; STAGING,
        // DYNAMIC and IMMUTABLE textures take any value) and the texture
        // keeps it for GetDesc1 and Map -- measured, d3d11_texmap_probe
        // [create] / [other]: every value WARP is given is stored verbatim.
        const GuestAddr desc_address = m.arg_ptr(1);
        const u32 layout_offset = slot == 54 ? 44u : 36u;
        u32 layout = 0;
        if (desc_address != 0) {
            const auto read = m_memory.read_u32(desc_address + layout_offset);
            if (read.is_err()) {
                const GuestAddr out = m.arg_ptr(3);
                if (out != 0) (void)m.write_ptr(out, 0);
                m.set_return(d3d11::E_INVALIDARG);
                return true;
            }
            layout = read.value();
        }
        m_pending_texture_layout = layout;
        impl_DeviceMethod(m, slot == 54 ? 5 : 6);
        m_pending_texture_layout = 0;
        return true;
    }

    case 57:
    case 58:
    case 59: {
        // CreateShaderResourceView1 / UnorderedAccessView1 / RenderTargetView1
        // (pResource, pDesc1, ppView). DESC1 adds a PlaneSlice to the
        // Texture2D and Texture2DArray members, after their existing fields,
        // so the rest of the layout is the base descriptor's. A single-plane
        // resource has plane 0 only (Windows refuses plane 1 with
        // E_INVALIDARG, measured for all three); on a planar video resource
        // the plane must be the one the view format names (measured,
        // d3d11_nv12_probe [views] "srv1/rtv1/uav1 ... planeN").
        const GuestAddr desc_address = m.arg_ptr(2);
        u32 plane = 0;
        u32 expected_plane = 0;
        if (desc_address != 0) {
            const auto resource = m_textures.find(m.arg_ptr(1));
            const auto view_format = m_memory.read_u32(desc_address);
            if (resource != m_textures.end() && view_format.is_ok() &&
                dxgi_format_is_planar(resource->second.desc.format)) {
                const int named = dxgi_video_view_plane(
                    resource->second.desc.format, view_format.value(),
                    slot == 58);
                expected_plane = named < 0 ? 0u : static_cast<u32>(named);
            }
            const auto dimension = m_memory.read_u32(desc_address + 4);
            constexpr u32 TEXTURE2D = 4;
            constexpr u32 TEXTURE2DARRAY = 5;
            u32 plane_offset = 0;
            if (dimension.is_ok() && dimension.value() == TEXTURE2D) {
                plane_offset = slot == 57 ? 16u : 12u;
            } else if (dimension.is_ok() &&
                       dimension.value() == TEXTURE2DARRAY) {
                plane_offset = slot == 57 ? 24u : 20u;
            }
            if (plane_offset != 0) {
                const auto read = m_memory.read_u32(desc_address + plane_offset);
                plane = read.is_ok() ? read.value() : 1u;
            }
        }
        if (plane != expected_plane) {
            const GuestAddr out = m.arg_ptr(3);
            if (out != 0) (void)m.write_ptr(out, 0);
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        impl_DeviceMethod(m, slot - 50);  // 7, 8, 9
        return true;
    }

    case 60: {
        // CreateQuery1(pQueryDesc1, ppQuery). DESC1 = DESC + ContextType.
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        u32 context_type = 0;
        u32 query_type = 0;
        if (desc_address != 0) {
            const auto read = m_memory.read_u32(desc_address + 8);
            context_type = read.is_ok() ? read.value() : 0xFFFFFFFFu;
            const auto type = m_memory.read_u32(desc_address);
            query_type = type.is_ok() ? type.value() : 0;
        }
        // Measured (d3d11_a10_query_rules_probe [create1]): ContextType past
        // VIDEO is E_INVALIDARG, and PIPELINE_STATISTICS and every later type
        // (the predicates and stream-output queries) take only ALL and 3D.
        if (context_type > CONTEXT_TYPE_MAX ||
            (context_type > 1 && query_type_needs_3d_context(query_type))) {
            if (out != 0) (void)m.write_ptr(out, 0);
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        impl_DeviceMethod(m, 24);
        const u32 result =
            m.cpu() != nullptr ? static_cast<u32>(m.cpu()->gpr.rax) : 1u;
        if (result == d3d11::S_OK && out != 0) {
            GuestAddr query = 0;
            if (read_pointer(out, query)) {
                auto it = m_queries.find(query);
                if (it != m_queries.end()) it->second.context_type = context_type;
            }
        }
        return true;
    }

    case 63:   // WriteToSubresource
    case 64:   // ReadFromSubresource
        // (void) Map on default: d3d11_mapped_image.cpp,
        // transfer_mapped_subresource.
        transfer_mapped_subresource(m, slot == 63);
        m.set_return(0);
        return true;

    case 65: {
        // RegisterDeviceRemovedEvent(hEvent, pdwCookie). This device is never
        // removed, so the event is never set; a NULL event is
        // DXGI_ERROR_INVALID_CALL -- measured.
        const GuestAddr event = m.arg_ptr(1);
        const GuestAddr cookie_out = m.arg_ptr(2);
        if (event == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL_HR);
            return true;
        }
        if (cookie_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        (void)m_memory.write_u32(cookie_out, ++m_device_removed_cookie);
        m.set_return(d3d11::S_OK);
        return true;
    }
    case 66:
        // UnregisterDeviceRemoved(dwCookie) (void).
        m.set_return(0);
        return true;

    case 67: {
        // OpenSharedFence(hFence, riid, ppFence). CreateFence refuses the
        // SHARED flag here, so no shared fence handle can exist.
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)m.write_ptr(out, 0);
        m.set_return(d3d11::E_INVALIDARG);
        return true;
    }
    default:
        return false;
    }
}

bool D3D11Manager::context_ext_method(Marshaler& m, usize slot,
                                      GuestAddr self) {
    switch (slot) {
    case 131:
        swap_device_context_state(m, self);
        return true;
    case 132:
        clear_view(m, self);
        return true;
    case 117:  // DiscardResource
    case 118:  // DiscardView
    case 133:  // DiscardView1
        // A discard makes the contents undefined. Keeping them is one valid
        // undefined value, so a no-op is a correct implementation.
        m.set_return(0);
        return true;

    // Tiled resources (d3d11_tiled.cpp). A deferred context answers these
    // itself (d3d11_deferred.cpp); here they run on the immediate context or
    // replay from a command list.
    case 134:
        m.set_return(update_tile_mappings(m));
        return true;
    case 135:
        m.set_return(copy_tile_mappings(m));
        return true;
    case 136:
        copy_tiles(m);
        return true;
    case 137:
        update_tiles(m);
        return true;
    case 138:
        m.set_return(resize_tile_pool(m));
        return true;
    case 139:
        tiled_resource_barrier(m);
        return true;

    case 140:  // IsAnnotationEnabled: no capture tool is attached -- measured.
        m.set_return(0);
        return true;
    case 141: {  // SetMarkerInt(pLabel, Data) (void)
        const i32 data = static_cast<i32>(m.arg(2));
        annotation_marker(self, DebugMarkerKind::Signpost, m.arg_ptr(1), &data);
        m.set_return(0);
        return true;
    }
    case 142: {  // BeginEventInt(pLabel, Data) (void)
        const i32 data = static_cast<i32>(m.arg(2));
        annotation_marker(self, DebugMarkerKind::Push, m.arg_ptr(1), &data);
        m.set_return(0);
        return true;
    }
    case 143:  // EndEvent() (void)
        annotation_marker(self, DebugMarkerKind::Pop, 0, nullptr);
        m.set_return(0);
        return true;

    case 144: {
        // Flush1(ContextType, hEvent): flush, and set hEvent once the flushed
        // work completes -- measured.
        const GuestAddr event = m.arg_ptr(2);
        auto& signaler = GpuEventSignaler::instance();
        if (event != 0 && signaler.available()) {
            signaler.signal_after(m_backend.flush_completion(), event);
        } else {
            m_backend.flush();
            if (event != 0) {
                d3d11_diag("ID3D11DeviceContext3::Flush1 with an event but no "
                           "event bridge; the event is not set");
            }
        }
        m.set_return(0);
        return true;
    }

    case 145:  // SetHardwareProtectionState(BOOL): stored -- measured.
        m_context_states[self].hardware_protection =
            static_cast<u32>(m.arg(1)) != 0;
        m.set_return(0);
        return true;
    case 146: {  // GetHardwareProtectionState(BOOL*)
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            (void)m_memory.write_u32(
                out, m_context_states[self].hardware_protection ? 1u : 0u);
        }
        m.set_return(0);
        return true;
    }
    default:
        return false;
    }
}

void D3D11Manager::create_blend_state1(Marshaler& m, GuestAddr self) {
    // CreateBlendState1(pBlendStateDesc1, ppBlendState): the
    // D3D11_BLEND_DESC rules plus the logic-op ones (blend_desc_valid). A
    // logic op needs OutputMergerLogicOp (Learn
    // D3D11_RENDER_TARGET_BLEND_DESC1), which this device reports where the
    // GPU has framebuffer fetch: the pixel shader applies the op itself
    // (DxbcTranslationOptions::logic_op). Validation precedes the S_FALSE of
    // a NULL ppBlendState, and the pool is CreateBlendState's (measured,
    // d3d11_a04_state_objects_probe "blend1 dedup vs CreateBlendState").
    const GuestAddr desc_address = m.arg_ptr(1);
    const GuestAddr state_out = m.arg_ptr(2);
    if (state_out != 0) (void)m.write_ptr(state_out, 0);
    BlendStateDesc desc{};
    if (desc_address == 0 ||
        !read_blend_desc(desc_address, /*desc1=*/true, desc) ||
        !blend_desc_valid(desc, m_backend.supports_framebuffer_fetch())) {
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    if (state_out == 0) {
        m.set_return(1u);
        return;
    }
    GuestAddr state = 0;
    const u32 hr = acquire_blend_state(m_interfaces.find(self)->second.owner,
                                       desc, state);
    if (hr != d3d11::S_OK) {
        m.set_return(hr);
        return;
    }
    if (m.write_ptr(state_out, state).is_err()) {
        (void)release_object(state);
        m.set_return(d3d11::E_POINTER);
        return;
    }
    m.set_return(d3d11::S_OK);
}

}  // namespace vitreo::winapi
