// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_caps.cpp
/// @brief ID3D11Device capability queries: CheckFormatSupport,
///        CheckMultisampleQualityLevels and CheckFeatureSupport (vtable
///        slots 29, 30 and 33).
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slots here once it has found the device. Every answer comes from the
/// one capability table in d3d11_format_caps.cpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::device_caps_method(Marshaler& m, usize slot,
                                      Device* device) {
    switch (slot) {
    case 29: {
        // CheckFormatSupport(Format, pFormatSupport). Learn: S_OK, or E_FAIL
        // when the device does not support the format. Measured on Windows
        // (d3d11_a01_device_caps_probe): DXGI_FORMAT_UNKNOWN answers S_OK
        // with BUFFER | CPU_LOCKABLE (structured and raw buffers), and a
        // format the runtime does not know is E_FAIL. A real format this
        // device cannot use answers S_OK with an empty mask. A failing
        // NV12/YUY2/P010 query is treated as a hard failure and disables the
        // path that carries the content.
        const CapsQueryLog caps_log(m, CapsQueryLog::Kind::CheckFormatSupport);
        const u32 dxgi_format = static_cast<u32>(m.arg(1));
        const GuestAddr support_out = m.arg_ptr(2);
        if (support_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const auto answer = d3d11::check_format_support(
            dxgi_format, format_caps_context(*device));
        if (m.write_u32(support_out, answer.mask).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CheckFormatSupport format=%u -> 0x%x hr=0x%x",
            dxgi_format, answer.mask, answer.hr);
        m.set_return(answer.hr);
        return;
    }
    case 30: {
        // CheckMultisampleQualityLevels. One rule with
        // CheckMultisampleQualityLevels1 (which differs only in its flags).
        // Callers walk powers of two until the count is 0.
        const CapsQueryLog caps_log(
            m, CapsQueryLog::Kind::CheckMultisampleQualityLevels);
        const u32 dxgi_format = static_cast<u32>(m.arg(1));
        const u32 sample_count = static_cast<u32>(m.arg(2));
        const GuestAddr levels_out = m.arg_ptr(3);
        if (levels_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        u32 levels = 0;
        const u32 hr = d3d11::check_multisample_quality_levels(
            dxgi_format, sample_count,
            format_caps_context(*device), levels);
        if (hr != d3d11::E_INVALIDARG &&
            m.write_u32(levels_out, levels).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CheckMultisampleQualityLevels "
            "format=%u samples=%u -> %u hr=0x%x",
            dxgi_format, sample_count, levels, hr);
        m.set_return(hr);
        return;
    }
    case 33: {
        const CapsQueryLog caps_log(m, CapsQueryLog::Kind::CheckFeatureSupport);
        const u32 feature = static_cast<u32>(m.arg(1));
        const GuestAddr data = m.arg_ptr(2);
        const u32 data_size = static_cast<u32>(m.arg(3));
        u32 expected_size = 0;
        // Sizes match Microsoft Learn D3D11_FEATURE_DATA_* structs. Unknown
        // features return E_INVALIDARG (Windows contract).
        switch (feature) {
        case 0: expected_size = 8; break;   // THREADING
        case 1: expected_size = 4; break;   // DOUBLES
        case 2: expected_size = 8; break;   // FORMAT_SUPPORT
        case 3: expected_size = 8; break;   // FORMAT_SUPPORT2
        case 4: expected_size = 4; break;   // D3D10_X_HARDWARE_OPTIONS
        case 5: expected_size = 56; break;  // D3D11_OPTIONS
        case 6: expected_size = 4; break;   // ARCHITECTURE_INFO
        case 7: expected_size = 4; break;   // D3D9_OPTIONS
        case 8: expected_size = 8; break;   // SHADER_MIN_PRECISION_SUPPORT
        case 9: expected_size = 4; break;   // D3D9_SHADOW_SUPPORT
        case 10: expected_size = 16; break; // D3D11_OPTIONS1
        case 11: expected_size = 4; break;  // D3D9_SIMPLE_INSTANCING_SUPPORT
        case 12: expected_size = 4; break;  // MARKER_SUPPORT
        case 13: expected_size = 16; break; // D3D9_OPTIONS1
        case 14: expected_size = 32; break; // D3D11_OPTIONS2
        case 15: expected_size = 4; break;  // D3D11_OPTIONS3
        case 16: expected_size = 8; break;  // GPU_VIRTUAL_ADDRESS_SUPPORT
        case 17: expected_size = 4; break;  // D3D11_OPTIONS4
        case 18: expected_size = 4; break;  // SHADER_CACHE
        case 19: expected_size = 4; break;  // D3D11_OPTIONS5
        case 20: expected_size = 8; break;  // DISPLAYABLE
        default:
            d3d11_diag(
                "ID3D11Device::CheckFeatureSupport feature=%u size=%u "
                "-> E_INVALIDARG",
                feature, data_size);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Measured for every feature and every size 0..96 (d3d11_capsmall_probe
        // "sizes"): only the structure's size is accepted, a refusal leaves
        // the buffer untouched -- except D3D11_OPTIONS2, which the runtime
        // also takes at 36 bytes and answers with a ninth member of 0.
        const bool options2_long = feature == 14 && data_size == 36;
        if (data == 0 || (data_size != expected_size && !options2_long)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (options2_long) expected_size = 36;
        // Default: capability bits FALSE. Fill only features this backend
        // actually honors — advertising unimplemented 11.1 paths (ClearView,
        // UAV-only, …) would invite callers onto broken code.
        std::array<u8, 64> payload{};
        if (feature == 10) {
            // D3D11_FEATURE_DATA_D3D11_OPTIONS1: TiledResourcesTier
            // (d3d11_tiled.cpp: TIER_1 from feature level 11_0 where the
            // backend has tiled resources -- measured WARP 1 at 11_0, 0 at
            // 10_1), then MinMaxFiltering at byte 4: the MINIMUM_ / MAXIMUM_
            // filters reduce in the translated shader, every stage
            // (DxbcTranslationOptions::minmax_samplers; D3D11.3 5.9.4.5.6,
            // measured against WARP, d3d11_minmax_probe).
            payload[0] = static_cast<u8>(
                tiled_resources_tier(device->feature_level));
            payload[4] = 1u;
            // ClearViewAlsoSupportsDepthOnlyFormats: ClearView on every
            // depth-stencil view (clear_view), at every level as on WARP.
            payload[8] = 1;
            // MapOnDefaultBuffers: "TRUE if the hardware is capable of at
            // least D3D_FEATURE_LEVEL_11_0" (Learn); CreateBuffer and Map
            // take CPU access on DEFAULT SRV / UAV buffers from there
            // (buffer_desc_check, map_buffer).
            payload[12] =
                device->feature_level >= d3d11::FEATURE_LEVEL_11_0 ? 1u : 0u;
        } else if (feature == 8) {
            // SHADER_MIN_PRECISION_SUPPORT: D3D11_SHADER_MIN_PRECISION_10_BIT
            // | _16_BIT for every stage. Minimum precision is a floor (Learn,
            // "Using HLSL minimum precision": the driver may execute at any
            // precision at or above it), and the translator runs every
            // min16float / min10float / min16int / min16uint at 32 bits --
            // exactly what WARP does while reporting {3, 3} at every level
            // (d3d11_capsmall_probe "minprec": 1 + 2^-12 stays 1.000244,
            // 40000 * 3 in min16int is 120000).
            payload[0] = payload[4] = 3;
        } else if (feature == 12) {
            // MARKER_SUPPORT.Profile: the ID3D11DeviceContext2 markers (and
            // ID3DUserDefinedAnnotation's) reach the GPU command stream as
            // debug groups and signposts that GPU capture and profiling tools
            // show (annotation_marker).
            payload[0] = m_backend.supports_debug_markers() ? 1u : 0u;
        } else if (feature == 0) {
            // Learn D3D11_FEATURE_DATA_THREADING: concurrent Create* is OK;
            // command lists are the runtime's recordings, not the driver's
            // (d3d11_deferred.cpp), as on WARP -- measured 1 / 0.
            payload[0] = 1; // DriverConcurrentCreates
            payload[4] = d3d11::kDriverCommandLists ? 1u : 0u;
        } else if (feature == 2 || feature == 3) {
            // Learn FORMAT_SUPPORT / FORMAT_SUPPORT2: InFormat is an input.
            // Preserve it and write OutFormatSupport* only.
            const auto in_format = m_memory.read_u32(data);
            if (in_format.is_err()) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            const u32 dxgi_format = in_format.value();
            // Learn CheckFeatureSupport: FORMAT_SUPPORT is the same query
            // as CheckFormatSupport. FORMAT_SUPPORT2 answers E_FAIL with 0
            // for a format that has no SUPPORT2 capability (measured).
            const auto context = format_caps_context(*device);
            const auto answer =
                feature == 2
                    ? d3d11::check_format_support(dxgi_format, context)
                    : d3d11::check_format_support2(dxgi_format, context);
            if (m.write_u32(data + 4, answer.mask).is_err()) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            d3d11_diag(
                "ID3D11Device::CheckFeatureSupport feature=%u fmt=%u "
                "-> 0x%x hr=0x%x",
                feature, dxgi_format, answer.mask, answer.hr);
            m.set_return(answer.hr);
            return;
        } else if (feature == 1) {
            // D3D11_FEATURE_DATA_DOUBLES.DoublePrecisionFloatShaderOps: the
            // SM5 double instructions (D3D11.3 22.14-22.17) run as exact
            // IEEE-754 integer emulation (graphics/shaders/soft_double.hpp)
            // -- Apple GPUs have no fp64. Doubles exist only in shader model
            // 5 (fxc: "cs_4_0 does not support doubles"), so WARP answers
            // TRUE at 11_x and FALSE at 10_x / 9_x (a01 "feature 1" rows).
            payload[0] = device->feature_level >= d3d11::FEATURE_LEVEL_11_0;
        } else if (feature == 4) {
            // D3D10_X_HARDWARE_OPTIONS.ComputeShaders_Plus_RawAndStructured
            // Buffers_Via_Shader_4_x. An FL 11_0 device is required to have
            // CS 5.0, and this device runs cs_4_x as well; FALSE at FL 11_0
            // contradicted its own feature level. Measured TRUE at 11_0,
            // 10_1 and 10_0, FALSE at 9_x (Windows).
            payload[0] = device->feature_level >= d3d11::FEATURE_LEVEL_10_0;
        } else if (feature == 5) {
            // D3D11_FEATURE_DATA_D3D11_OPTIONS: fourteen BOOLs, each reported
            // only for what this device implements, at the levels WARP
            // reports it (a01 probe "FL.. feature 5": all TRUE at 11_x;
            // UAVOnlyRenderingForcedSampleCount, SAD4 and extended doubles
            // FALSE at 10_x; MapNoOverwriteOnDynamicBufferSRV FALSE at 9_x).
            // Learn: a device at FL 11_1 reports every member TRUE but SAD4
            // and the extended doubles -- which is why this one stops at
            // 11_0 (select_feature_level).
            const u32 level = device->feature_level;
            const bool fl11 = level >= d3d11::FEATURE_LEVEL_11_0;
            std::array<u32, 14> options{};
            // OutputMergerLogicOp: CreateBlendState1 logic ops, run by the
            // pixel shader through framebuffer fetch on every renderable
            // UINT format (D3D11.3 17.7).
            options[0] = m_backend.supports_framebuffer_fetch() ? 1u : 0u;
            // UAVOnlyRenderingForcedSampleCount promises UAV-only rendering
            // at ForcedSampleCount 1, 4 and 8; Metal rasterizes at a sample
            // count the device supports, and Apple GPUs refuse 8.
            options[1] = fl11 && m_backend.supports_texture_sample_count(4) &&
                                 m_backend.supports_texture_sample_count(8)
                             ? 1u : 0u;
            // DiscardAPIsSeenByDriver, FlagsForUpdateAndCopySeenByDriver:
            // DiscardResource / DiscardView(1) (a kept value is one valid
            // undefined value) and the CopySubresourceRegion1 /
            // UpdateSubresource1 CopyFlags (validated as measured; an ordered
            // copy keeps both promises).
            options[2] = options[3] = 1u;
            // ClearView, CopyWithOverlap (a same-subresource copy is a
            // memmove), ConstantBufferPartialUpdate (UpdateSubresource1 with
            // a 16-byte-aligned box and NO_OVERWRITE / DISCARD),
            // ConstantBufferOffsetting (*SetConstantBuffers1 windows; every
            // cb# read past one reads 0, fill_constant_buffer_limits) and
            // MapNoOverwriteOnDynamicConstantBuffer: one group (Learn).
            for (u32 member = 4; member <= 8; ++member) options[member] = 1u;
            // MapNoOverwriteOnDynamicBufferSRV: Map allows NO_OVERWRITE on
            // every dynamic buffer, whatever it is bound as (D3D11.3 5.6.1.2:
            // "required to be supported for all D3D10+ hardware"; the
            // dynamic-buffer-SRV Map in map_buffer, tested by
            // MapNoOverwriteTakesEveryDynamicBuffer). It does not depend on
            // the device's sample positions. WARP: TRUE at 11_x and 10_x,
            // FALSE at 9_x (a01 "feature 5" rows).
            options[9] = level >= d3d11::FEATURE_LEVEL_10_0 ? 1u : 0u;
            // MultisampleRTVWithForcedSampleCountOne: the pass's samples at
            // the pixel centre.
            options[10] = m_backend.supports_programmable_sample_positions()
                              ? 1u : 0u;
            // SAD4ShaderInstructions: msad (D3D11.3 22.12.15), shader model 5.
            options[11] = fl11 ? 1u : 0u;
            // ExtendedDoublesShaderInstructions: ddiv, dfma, drcp, dtoi,
            // dtou, itod, utod, emulated exactly with the base set (feature
            // 1); WARP: TRUE at 11_x, FALSE at 10_x.
            options[12] = fl11 ? 1u : 0u;
            // ExtendedResourceSharing promises sharing of mipmapped and array
            // textures, UAV / no binds, GENERATE_MIPS, the video formats and
            // the RESTRICT_SHARED_RESOURCE flags (Learn, Direct3D 11.1
            // features, "Extended support for shared Texture2D resources"):
            // every shape of every shareable format is shared here -- an
            // IOSurface image or a store-mirrored resource
            // (d3d11_sharing.cpp, metal_shared_resource.hpp).
            options[13] = m_backend.supports_extended_sharing() ? 1u : 0u;
            std::memcpy(payload.data(), options.data(), sizeof(options));
        } else if (feature == 7) {
            // D3D9_OPTIONS.FullNonPow2TextureSupport: "The runtime sets this
            // member to TRUE for hardware at Direct3D 10 and higher feature
            // levels" (Learn); Metal has no pow2 restriction at any level,
            // and WARP answers TRUE at 9_x too.
            payload[0] = 1;
        } else if (feature == 9) {
            // D3D9_SHADOW_SUPPORT: comparison sampling of a depth texture
            // (Learn: TRUE at FL >= 10; WARP TRUE at every level).
            payload[0] = 1;
        } else if (feature == 11) {
            // D3D9_SIMPLE_INSTANCING_SUPPORT: instanced draws are implemented.
            payload[0] = 1;
        } else if (feature == 13) {
            // D3D9_OPTIONS1: FullNonPow2TextureSupported,
            // DepthAsTextureWithLessEqualComparisonFilterSupported (TRUE at
            // FL >= 10, Learn), SimpleInstancingSupported (as feature 11),
            // TextureCubeFaceRenderTargetWithNonCubeDepthStencilSupported (a
            // cube-face RTV with a 2D DSV is an ordinary Metal render pass).
            payload[0] = payload[4] = payload[8] = payload[12] = 1;
        } else if (feature == 14) {
            // Learn D3D11_FEATURE_DATA_D3D11_OPTIONS2: eight 4-byte members,
            // TypedUAVLoadAdditionalFormats second. Learn (Typed UAV loads)
            // makes that group all-or-nothing, so a device reporting
            // UAV_TYPED_LOAD for R16_FLOAT or R16G16B16A16_FLOAT has to report
            // the flag here too. Typed UAVs exist only at FL 11_0 (WARP: 0 at
            // FL 10_x). TiledResourcesTier (byte 16) repeats OPTIONS1's
            // (measured WARP at 11_0: 1 in both). ROVs and conservative
            // raster stay FALSE: this backend does not implement them.
            // PSSpecifiedStencilRefSupported (byte 0): a pixel shader's
            // SV_StencilRef output, which exists only in shader model 5
            // (see ps_specified_stencil_ref_supported).
            payload[0] = ps_specified_stencil_ref_supported(
                device->feature_level);
            payload[4] = device->feature_level >= d3d11::FEATURE_LEVEL_11_0;
            payload[16] = static_cast<u8>(
                tiled_resources_tier(device->feature_level));
            // MapOnDefaultTextures (member 5) and StandardSwizzle (member 6):
            // DEFAULT textures with CPU access, Map with a NULL
            // pMappedResource, ID3D11Device3 Write/ReadFromSubresource, and
            // the ROW_MAJOR / 64K_STANDARD_SWIZZLE layouts as the CPU sees
            // them (texture_desc_violation, map_texture,
            // transfer_mapped_subresource, d3d11_standard_swizzle.hpp), all
            // measured against WARP (d3d11_texmap_probe), which reports both.
            payload[20] = 1;
            payload[24] = 1;
            // UnifiedMemoryArchitecture: FALSE at every feature level, by the
            // adapter profile policy (.docs/contracts/dxgi.md "Adapter profile
            // policy (discrete-class)"): the adapter presents itself as a
            // discrete-class GPU whatever the host is, as a real discrete
            // adapter answers (RTX 4080 SUPER, d3d11_a01 caps probe: member 7
            // = 0 at 11_0 and 11_1). The member is informational (Learn): what
            // a caller may map is decided by MapOnDefaultTextures above and
            // OPTIONS1 MapOnDefaultBuffers, which do not change. The host fact
            // stays available as GraphicsBackendDevice::has_unified_memory
            // (the D3D12 frontend's heap pools are built on it).
            payload[28] = 0;
        } else if (feature == 15) {
            // Learn D3D11_FEATURE_DATA_D3D11_OPTIONS3: one BOOL,
            // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer. A vertex
            // shader's SV_RenderTargetArrayIndex and SV_ViewportArrayIndex
            // both rasterize as on Windows (oracle scenes vs_layer_clip and
            // vs_viewport_index); a domain shader cannot be created at all
            // yet, so no shader this device accepts contradicts it.
            payload[0] = 1;
        } else if (feature == 16) {
            // GPU_VIRTUAL_ADDRESS_SUPPORT {MaxGPUVirtualAddressBitsPerResource,
            // MaxGPUVirtualAddressBitsPerProcess}. 0 is not an answer. A
            // 32-bit guest reports 31/31 (the address space of a 32-bit
            // process, as WARP does); a 64-bit guest 32 bits per resource
            // (ByteWidth is a UINT, and the device's maxBufferLength exceeds
            // it) and 47 per process -- the arm64 macOS user address space
            // (MACH_VM_MAX_ADDRESS, 2^47), which Metal buffers live in.
            const u64 max_buffer = m_backend.max_buffer_length();
            u32 resource_bits = 0;
            while (resource_bits < 32 && (1ull << resource_bits) < max_buffer) {
                ++resource_bits;
            }
            if (resource_bits == 0) resource_bits = 32;
            const u32 per_resource = m_is_64bit ? resource_bits
                                                : std::min(resource_bits, 31u);
            const u32 per_process = m_is_64bit ? 47u : 31u;
            std::memcpy(payload.data(), &per_resource, sizeof(u32));
            std::memcpy(payload.data() + 4, &per_process, sizeof(u32));
        } else if (feature == 17) {
            // D3D11_OPTIONS4.ExtendedNV12SharedTextureSupported: "if NV12
            // textures can be shared across processes and D3D devices"
            // (Learn). NV12 Texture2Ds share like every other shape here
            // (d3d11_sharing.cpp): same-process openings share storage,
            // other processes mirror it.
            const u32 nv12 =
                extended_nv12_sharing(device->feature_level) ? 1u : 0u;
            std::memcpy(payload.data(), &nv12, sizeof(u32));
        } else if (feature == 19 || feature == 20) {
            // D3D11_OPTIONS5.SharedResourceTier, and DISPLAYABLE
            // {DisplayableTexture, SharedResourceTier}. Learn
            // D3D11_SHARED_RESOURCE_TIER: TIER_1 is the support "when
            // ExtendedResourceSharing and ExtendedNV12SharedTextureSupported
            // are TRUE" -- both hold here. TIER_2 adds sharing of the
            // ExtendedResourceSharing formats "across APIs between Direct3D
            // 11 and Direct3D 12", and TIER_3 R11G11B10_FLOAT NT-handle
            // sharing with D3D12: the D3D12 frontend opens no shared handle
            // (ID3D12Device::OpenSharedHandle is not implemented), so neither
            // holds. TIER_3's Direct3D 11 half -- R11G11B10_FLOAT NT-handle
            // sharing -- works here (d3d11_capsmall_probe "share"); its
            // cross-API half needs the same D3D12 interop. No texture is
            // displayable (D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE is not
            // implemented).
            const u32 tier =
                m_backend.supports_extended_sharing() &&
                        extended_nv12_sharing(device->feature_level)
                    ? 1u   // D3D11_SHARED_RESOURCE_TIER_1
                    : 0u;  // D3D11_SHARED_RESOURCE_TIER_0
            std::memcpy(payload.data() + (feature == 20 ? 4 : 0), &tier,
                        sizeof(u32));
        }
        if (m_memory.write_bytes(data, payload.data(),
                                 expected_size).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CheckFeatureSupport feature=%u -> ok",
            feature);
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
