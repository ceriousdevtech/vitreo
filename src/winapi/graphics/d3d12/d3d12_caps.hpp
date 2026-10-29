// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_caps.hpp
/// @brief ID3D12Device::CheckFeatureSupport for every D3D12_FEATURE of the
/// Windows SDK 10.0.26100.0, the device's maximum feature level, and the
/// per-format support its resource rules use.
///
/// The structure sizes and member offsets are generated from the SDK
/// (d3d12_feature_layout.inc, tools/generate_d3d12_feature_layout.py, gated
/// in CTest); the input rules -- which DataSize is accepted, which inputs are
/// refused, what is written on each failure -- are measured on Windows
/// (tools/oracle_probe/d3d12_caps_probe.c). The answers describe this
/// implementation: shader model 5.1, root signature 1.1, no DXIL features,
/// no raytracing / mesh / VRS / sampler feedback / work graphs / video, and
/// every tier the context says the frontend and backend implement.

#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d12/d3d12_layout.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::graphics {
class GraphicsBackendDevice;
}

namespace vitreo::winapi {
class Marshaler;
}

namespace vitreo::winapi::d3d12 {

/// D3D_SHADER_MODEL_5_1. The D3D_ROOT_SIGNATURE_VERSION values come from
/// d3d12_root_signature.hpp.
inline constexpr u32 SHADER_MODEL_5_1 = 0x51u;
/// E_UNEXPECTED: the 26100 runtime's answer to PREDICATION,
/// PLACED_RESOURCE_SUPPORT_INFO and HARDWARE_COPY (measured).
inline constexpr u32 E_UNEXPECTED_HR = 0x8000FFFFu;

/// D3D12_OPTIONS16 GPUUploadHeapSupported: on this unified-memory adapter
/// every heap is GPU-local and CPU-mappable, so D3D12_HEAP_TYPE_GPU_UPLOAD
/// is write-combined shared memory (d3d12_resource.cpp resolve_custom).
inline constexpr bool GPU_UPLOAD_HEAP_SUPPORTED = true;

/// What this device implements. Every member defaults to "not
/// implemented"; the device owner turns a member on only when the frontend
/// and backend serve all of it (a claimed tier must be complete).
struct CapsContext {
    bool is_64bit{true};
    /// The D3D11 per-format tables (the Windows FL 11_0 ceiling intersected
    /// with the Metal backend): FORMAT_SUPPORT, quality levels and the
    /// resource rules all answer from these.
    d3d11::FormatCapsContext formats{};
    /// D3D12_RESOURCE_BINDING_TIER: 2 when descriptor tables resolve into
    /// Metal Tier-2 argument buffers sized to the heap; 3 needs unbounded
    /// ranges of every class and is not claimed.
    u32 resource_binding_tier{1};
    /// D3D12_TILED_RESOURCES_TIER: the D3D11 frontend's tier (1 where the
    /// backend has sparse textures) once CreateReservedResource and
    /// UpdateTileMappings exist; 0 until then.
    u32 tiled_resources_tier{0};
    /// D3D12_RESOURCE_HEAP_TIER: 2 when one ID3D12Heap holds buffers and
    /// every texture kind (a Metal placement heap does).
    u32 resource_heap_tier{1};
    /// D3D_ROOT_SIGNATURE_VERSION the root signature parser accepts.
    u32 root_signature_version{ROOT_SIGNATURE_VERSION_1_1};
    /// SM5 doubles through the translator's exact emulation (as D3D11).
    bool double_precision_shader_ops{false};
    /// Output-merger logic ops in pipeline states.
    bool logic_ops{false};
    /// The 18 formats of typed UAV loads (Learn, Typed UAV loads).
    bool typed_uav_load_additional_formats{false};
    /// SV_RenderTargetArrayIndex / SV_ViewportArrayIndex from a VS or DS.
    bool vp_and_rt_array_index_from_any_shader{false};
    /// 64 KiB-aligned (small) MSAA textures in placement heaps.
    bool msaa_64kb_aligned_textures{false};
    /// Block-compressed textures of any top-level size (OPTIONS8).
    bool unaligned_block_textures{false};
    /// Hardware facts (MTLDevice): tile-based deferred renderer, unified and
    /// cache-coherent memory, maxBufferLength, SIMD-group width and the
    /// total SIMD lanes.
    bool tile_based_renderer{false};
    bool unified_memory{false};
    bool cache_coherent_uma{false};
    u64 max_buffer_length{0};
    u32 simd_width{0};
    u32 total_lane_count{0};
    /// D3D12_SHADER_CACHE_SUPPORT_FLAGS the frontend implements.
    u32 shader_cache_support{0};
    /// D3D12_COMMAND_LIST_SUPPORT_FLAGS of WriteBufferImmediate.
    u32 write_buffer_immediate_support{0};
    /// A fully typed resource viewed as another format of its family.
    bool casting_fully_typed_formats{false};
    /// SetSamplePositions (D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER).
    u32 programmable_sample_positions_tier{0};
    /// D3D12_OPTIONS19.SupportedSampleCountsWithNoOutputs: bit n set when
    /// UAV-only rendering runs at ForcedSampleCount 2^n.
    u32 sample_counts_with_no_outputs{0};
    /// D3D12_EXECUTE_INDIRECT_TIER (TIER_1_0 = 10 is the core API).
    u32 execute_indirect_tier{10};
    /// Enhanced barriers and relaxed format casting (ID3D12Device10/12).
    bool enhanced_barriers{false};
    bool relaxed_format_casting{false};
    /// Queue priorities the queue implementation honours: NORMAL always,
    /// HIGH when set. GLOBAL_REALTIME is never claimed.
    bool high_priority_queues{false};
};

/// The hardware facts of @p backend, read the way the D3D11 device reads
/// them (D3D11Manager::format_caps_context): MSAA counts, float32 filtering
/// and resolve, shareable storage, the tiled-format shapes, unified memory,
/// maxBufferLength. Everything the D3D12 frontend itself implements stays
/// off for the device owner to turn on; so do the SIMD width and lane count,
/// which the backend contract does not expose yet.
[[nodiscard]] CapsContext backend_caps_context(const graphics::GraphicsBackendDevice& backend,
                                               bool is_64bit);

/// The highest D3D_FEATURE_LEVEL whose required features the context has
/// (Learn, Hardware feature levels): 12_0 needs binding tier 2, tiled tier
/// 2 and the typed UAV load formats on top of 11_1, which needs logic ops,
/// and UAV-only rendering at ForcedSampleCount 16; 12_1 needs ROVs and
/// conservative rasterization, which this device has not.
[[nodiscard]] u32 max_feature_level(const CapsContext& caps) noexcept;

/// D3D12_FEATURE_FORMAT_SUPPORT: E_FAIL with 0 / 0 for a format the device
/// cannot use.
struct FormatSupport12 {
    u32 hr{0};
    u32 support1{0};
    u32 support2{0};
};
[[nodiscard]] FormatSupport12 format_support(u32 dxgi_format, const CapsContext& caps) noexcept;

/// D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: returns the HRESULT and sets
/// @p write / @p levels to what the runtime writes to NumQualityLevels.
[[nodiscard]] u32 multisample_quality_levels(u32 dxgi_format, u32 sample_count, u32 flags,
                                             const CapsContext& caps, bool& write,
                                             u32& levels) noexcept;

/// GPU_VIRTUAL_ADDRESS_SUPPORT {per resource, per process}.
void gpu_virtual_address_bits(const CapsContext& caps, u32& per_resource,
                              u32& per_process) noexcept;

/// The LayoutCaps the resource-layout rules take for this device.
[[nodiscard]] LayoutCaps layout_caps(const CapsContext& caps);

/// ID3D12Device::CheckFeatureSupport(Feature, pFeatureSupportData,
/// FeatureSupportDataSize) on guest memory: returns the HRESULT and writes
/// exactly the members Windows writes for that outcome.
[[nodiscard]] u32 check_feature_support(GuestMemory& memory, const CapsContext& caps,
                                        u32 feature, GuestAddr data, u32 data_size);

/// ID3D12Device slot 13, CheckFeatureSupport(This, Feature,
/// pFeatureSupportData, FeatureSupportDataSize): the whole guest call.
void marshal_check_feature_support(Marshaler& m, GuestMemory& memory, const CapsContext& caps);

}  // namespace vitreo::winapi::d3d12
