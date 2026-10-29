// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/d3d12/d3d12_caps.hpp"

#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace vitreo::winapi::d3d12 {

namespace feature_layout {
#include "winapi/graphics/d3d12/d3d12_feature_layout.inc"
}  // namespace feature_layout

namespace {

using feature_layout::FieldOffset;
namespace f = feature_layout::field;

// D3D12_FEATURE values (SDK 26100 enum D3D12_FEATURE; the generated table
// carries the same numbers and names).
enum : u32 {
    F_D3D12_OPTIONS = 0,
    F_ARCHITECTURE = 1,
    F_FEATURE_LEVELS = 2,
    F_FORMAT_SUPPORT = 3,
    F_MULTISAMPLE_QUALITY_LEVELS = 4,
    F_FORMAT_INFO = 5,
    F_GPU_VIRTUAL_ADDRESS_SUPPORT = 6,
    F_SHADER_MODEL = 7,
    F_D3D12_OPTIONS1 = 8,
    F_PROTECTED_RESOURCE_SESSION_SUPPORT = 10,
    F_ROOT_SIGNATURE = 12,
    F_ARCHITECTURE1 = 16,
    F_D3D12_OPTIONS2 = 18,
    F_SHADER_CACHE = 19,
    F_COMMAND_QUEUE_PRIORITY = 20,
    F_D3D12_OPTIONS3 = 21,
    F_EXISTING_HEAPS = 22,
    F_D3D12_OPTIONS4 = 23,
    F_SERIALIZATION = 24,
    F_CROSS_NODE = 25,
    F_D3D12_OPTIONS5 = 27,
    F_DISPLAYABLE = 28,
    F_D3D12_OPTIONS6 = 30,
    F_QUERY_META_COMMAND = 31,
    F_D3D12_OPTIONS7 = 32,
    F_PROTECTED_RESOURCE_SESSION_TYPE_COUNT = 33,
    F_PROTECTED_RESOURCE_SESSION_TYPES = 34,
    F_D3D12_OPTIONS8 = 36,
    F_D3D12_OPTIONS9 = 37,
    F_D3D12_OPTIONS10 = 39,
    F_D3D12_OPTIONS11 = 40,
    F_D3D12_OPTIONS12 = 41,
    F_D3D12_OPTIONS13 = 42,
    F_D3D12_OPTIONS14 = 43,
    F_D3D12_OPTIONS15 = 44,
    F_D3D12_OPTIONS16 = 45,
    F_D3D12_OPTIONS17 = 46,
    F_D3D12_OPTIONS18 = 47,
    F_D3D12_OPTIONS19 = 48,
    F_D3D12_OPTIONS20 = 49,
    F_PREDICATION = 50,
    F_PLACED_RESOURCE_SUPPORT_INFO = 51,
    F_HARDWARE_COPY = 52,
    F_D3D12_OPTIONS21 = 53,
    F_APPLICATION_SPECIFIC_DRIVER_STATE = 56,
    F_BYTECODE_BYPASS_HASH_SUPPORTED = 57,
    F_SHADER_CACHE_ABI_SUPPORT = 61,
};

// d3dcommon.h / d3d12.h (SDK 26100) values the answers use (the feature
// levels and command-list types come from d3d12.hpp).
constexpr u32 SHADER_MODEL_6_0 = 0x60u;
constexpr u32 SHADER_MODEL_6_9 = 0x69u;
constexpr u32 SHADER_MIN_PRECISION_SUPPORT_10_BIT = 0x1u;
constexpr u32 SHADER_MIN_PRECISION_SUPPORT_16_BIT = 0x2u;
constexpr u32 COMMAND_QUEUE_PRIORITY_NORMAL = 0u;
constexpr u32 COMMAND_QUEUE_PRIORITY_HIGH = 100u;
constexpr u32 COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME = 10000u;
constexpr u32 MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE = 2048u;
constexpr u32 MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 = 1000000u;
constexpr u32 MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE = 0x1u;

// D3D12_FORMAT_SUPPORT1 has no MIP_AUTOGEN (0x2000) or CPU_LOCKABLE
// (0x20000) and D3D12_FORMAT_SUPPORT2 no SHAREABLE (0x400) (SDK 26100); the
// video bits describe D3D12 video, which this device has none of.
constexpr u32 kSupport1Defined = ~(0x2000u | 0x20000u);
constexpr u32 kSupport1Video = 0x08000000u | 0x10000000u | 0x20000000u | 0x40000000u;
constexpr u32 kSupport2Tiled = 0x200u;
constexpr u32 kSupport2Shareable = 0x400u;
constexpr u32 kSupport2Displayable = 0x10000u;

const feature_layout::FeatureLayout* find_layout(u32 feature) {
    for (const auto& row : feature_layout::kFeatureLayouts) {
        if (row.feature == feature) return &row;
    }
    return nullptr;
}

/// Writes members of one guest structure at their SDK offsets.
class Fields {
public:
    Fields(GuestMemory& memory, GuestAddr base, bool is_64bit)
        : m_memory(memory), m_base(base), m_is_64bit(is_64bit) {}

    [[nodiscard]] bool read(const FieldOffset& field, u32& value) const {
        const auto v = m_memory.read_u32(m_base + field.at(m_is_64bit));
        if (v.is_err()) return false;
        value = v.value();
        return true;
    }
    [[nodiscard]] bool read_pointer(const FieldOffset& field, GuestAddr& value) const {
        if (m_is_64bit) {
            const auto v = m_memory.read_u64(m_base + field.x64);
            if (v.is_err()) return false;
            value = v.value();
            return true;
        }
        u32 v32 = 0;
        if (!read(field, v32)) return false;
        value = v32;
        return true;
    }
    void put(const FieldOffset& field, u32 value) {
        (void)m_memory.write_u32(m_base + field.at(m_is_64bit), value);
    }
    void put8(const FieldOffset& field, u8 value) {
        (void)m_memory.write_bytes(m_base + field.at(m_is_64bit), &value, 1);
    }

private:
    GuestMemory& m_memory;
    GuestAddr m_base;
    bool m_is_64bit;
};

u32 floor_log2(u64 value) {
    u32 bits = 0;
    while (bits < 63 && (2ull << bits) <= value) ++bits;
    return bits;
}

d3d11::FormatCapsContext d3d11_formats(const CapsContext& caps) {
    d3d11::FormatCapsContext formats = caps.formats;
    formats.feature_level = D3D_FEATURE_LEVEL_11_0;
    formats.logic_ops = caps.logic_ops;
    formats.tiled_resources = formats.tiled_resources && caps.tiled_resources_tier >= 1;
    return formats;
}

}  // namespace

CapsContext backend_caps_context(const graphics::GraphicsBackendDevice& backend, bool is_64bit) {
    CapsContext caps;
    caps.is_64bit = is_64bit;
    d3d11::FormatCapsContext& f = caps.formats;
    f.feature_level = D3D_FEATURE_LEVEL_11_0;
    f.sample_count_2 = backend.supports_texture_sample_count(2);
    f.sample_count_4 = backend.supports_texture_sample_count(4);
    f.sample_count_8 = backend.supports_texture_sample_count(8);
    f.sample_count_16 = backend.supports_texture_sample_count(16);
    f.float32_filtering = backend.supports_float32_filtering();
    f.float32_msaa_resolve = backend.supports_float32_multisample_resolve();
    for (usize i = 1; i < f.shared_storage.size(); ++i) {
        f.shared_storage[i] = backend.supports_shared_texture(static_cast<graphics::PixelFormat>(i));
    }
    f.extended_sharing = backend.supports_extended_sharing();
    // Tiled formats as the D3D11 device finds them: Windows reports TILED
    // and a sparse tile of the storage format has the standard shape, at 1x
    // and (where 1x exists) 4x. The tier itself stays 0 until reserved
    // resources exist in this frontend.
    f.tiled_resources = backend.supports_tiled_resources();
    for (u32 format = 0; f.tiled_resources && format < f.tiled_format.size(); ++format) {
        if ((d3d11::windows_support2(format) & d3d11::FORMAT_SUPPORT2_TILED) == 0) continue;
        const graphics::PixelFormat storage = d3d11::texture_storage_format(format);
        for (const u32 samples : {1u, 4u}) {
            u32 want_w = 0, want_h = 0;
            unsigned have_w = 0, have_h = 0;
            const bool same = d3d11::standard_tile_shape(format, samples, want_w, want_h) &&
                              backend.tiled_texture_tile_shape(storage, samples, have_w, have_h) &&
                              have_w == want_w && have_h == want_h;
            (samples == 1 ? f.tiled_format : f.tiled_format_4x)[format] = same;
        }
        f.tiled_format_4x[format] = f.tiled_format_4x[format] && f.tiled_format[format];
    }
    caps.unified_memory = backend.has_unified_memory();
    // Metal shared storage on a unified-memory GPU is coherent with the CPU
    // caches (write-back upload heaps, Learn D3D12_FEATURE_DATA_ARCHITECTURE1).
    caps.cache_coherent_uma = caps.unified_memory;
    caps.max_buffer_length = backend.max_buffer_length();
    // The translator's exact soft-double emulation, as the D3D11 device
    // reports DoublePrecisionFloatShaderOps.
    caps.double_precision_shader_ops = true;
    return caps;
}

u32 max_feature_level(const CapsContext& caps) noexcept {
    // Learn, Hardware feature levels: 11_1 adds logic ops (Yes), MRT with
    // ForcedSampleCount 1 and UAV-only rendering at ForcedSampleCount 16;
    // 12_0 adds resource binding tier 2, tiled resources tier 2 and the 18
    // typed UAV load formats.
    const bool fl11_1 = caps.logic_ops && (caps.sample_counts_with_no_outputs & 0x10u) != 0;
    if (!fl11_1) return D3D_FEATURE_LEVEL_11_0;
    if (caps.resource_binding_tier >= 2 && caps.tiled_resources_tier >= 2 &&
        caps.typed_uav_load_additional_formats) {
        return D3D_FEATURE_LEVEL_12_0;
    }
    return D3D_FEATURE_LEVEL_11_1;
}

FormatSupport12 format_support(u32 dxgi_format, const CapsContext& caps) noexcept {
    // The D3D12-only formats (sampler feedback, A4B4G4R4) have no Metal
    // path here; a format the runtime does not know fails as on Windows.
    if (format_plane_count(dxgi_format) == 0 || dxgi_format >= 189) {
        return {E_FAIL, 0, 0};
    }
    const d3d11::FormatCapsContext formats = d3d11_formats(caps);
    const auto one = d3d11::check_format_support(dxgi_format, formats);
    if (one.hr != S_OK) return {E_FAIL, 0, 0};
    const u32 support1 = one.mask & kSupport1Defined & ~kSupport1Video;
    if (support1 == 0) return {E_FAIL, 0, 0};
    const auto two = d3d11::check_format_support2(dxgi_format, formats);
    u32 support2 = two.hr == S_OK ? two.mask : 0u;
    // DISPLAYABLE belongs to D3D12's displayable textures
    // (D3D12_FEATURE_DISPLAYABLE), which this device does not have; WARP
    // does not report it either (d3d12_caps_probe [format] 28).
    support2 &= ~(kSupport2Shareable | kSupport2Displayable);
    if (caps.tiled_resources_tier == 0) {
        support2 &= ~kSupport2Tiled;
    } else if (dxgi_format == 0) {
        support2 |= kSupport2Tiled;  // reserved buffers (WARP: UNKNOWN s2 = TILED)
    }
    return {S_OK, support1, support2};
}

u32 multisample_quality_levels(u32 dxgi_format, u32 sample_count, u32 flags,
                               const CapsContext& caps, bool& write, u32& levels) noexcept {
    write = false;
    levels = 0;
    // Measured (d3d12_caps_probe [format] msaa): a format the runtime does
    // not know fails E_FAIL with NumQualityLevels untouched; a count of 0 or
    // above 32 fails E_FAIL with 0; one sample is one level for every known
    // format; any non-zero Flags answers as TILED_RESOURCE, which is 0
    // unless the format tiles (one sample: FORMAT_SUPPORT2_TILED; R32G32B32
    // answers 0 where R8G8B8A8 answers 1).
    if (format_plane_count(dxgi_format) == 0) return E_FAIL;
    write = true;
    if (sample_count == 0 || sample_count > 32u) return E_FAIL;
    const bool tiled = flags != 0;  // every non-zero Flags (measured: 2 answers as 1)
    (void)MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE;
    if (sample_count == 1) {
        levels = !tiled || (format_support(dxgi_format, caps).support2 & kSupport2Tiled) != 0 ? 1u : 0u;
        return S_OK;
    }
    if (dxgi_format >= 189) return S_OK;  // no D3D12-only format multisamples here
    u32 d3d11_levels = 0;
    const d3d11::FormatCapsContext formats = d3d11_formats(caps);
    if (d3d11::check_multisample_quality_levels(dxgi_format, sample_count, formats,
                                                d3d11_levels) != S_OK) {
        return S_OK;
    }
    if (tiled) {
        // A reserved MSAA texture: the tiled tier serves 4x only, per format
        // (as D3D11's CheckMultisampleQualityLevels1(TILED)).
        const bool tiled_4x = caps.tiled_resources_tier >= 1 && sample_count == 4 &&
                              dxgi_format < formats.tiled_format_4x.size() &&
                              formats.tiled_format_4x[dxgi_format];
        levels = tiled_4x ? d3d11_levels : 0u;
        return S_OK;
    }
    levels = d3d11_levels;
    return S_OK;
}

void gpu_virtual_address_bits(const CapsContext& caps, u32& per_resource,
                              u32& per_process) noexcept {
    // A 32-bit guest: its own 2 GiB address space bounds both (WARP x86
    // reports 31 / 31). A 64-bit guest: the largest Metal buffer
    // (maxBufferLength) bounds a resource, and GPU virtual addresses come
    // from the frontend's allocator in a 47-bit space (WARP x64: 47).
    if (!caps.is_64bit) {
        per_resource = 31;
        per_process = 31;
        return;
    }
    per_process = 47;
    per_resource = caps.max_buffer_length != 0
                       ? std::min(floor_log2(caps.max_buffer_length), per_process)
                       : 32u;
}

LayoutCaps layout_caps(const CapsContext& caps) {
    LayoutCaps layout;
    layout.format_support1 = [caps](u32 format) { return format_support(format, caps).support1; };
    layout.quality_levels = [caps](u32 format, u32 count) {
        bool write = false;
        u32 levels = 0;
        (void)multisample_quality_levels(format, count, 0, caps, write, levels);
        return levels;
    };
    layout.max_buffer_width = caps.max_buffer_length != 0 ? caps.max_buffer_length : 0xFFFFFFFFull;
    layout.node_mask = 1;
    layout.relaxed_format_casting = caps.relaxed_format_casting;
    layout.unaligned_block_textures = caps.unaligned_block_textures;
    return layout;
}

u32 check_feature_support(GuestMemory& memory, const CapsContext& caps, u32 feature,
                          GuestAddr data, u32 data_size) {
    // Learn CheckFeatureSupport: E_INVALIDARG for an unsupported feature or
    // a size mismatch. Measured for every feature value 0..70 and every
    // DataSize 0..sizeof+32 (d3d12_caps_probe [scan]): exactly the SDK
    // structure's size is accepted, on each bitness, and a refusal writes
    // nothing. Feature values the 26100 SDK does not define are refused
    // (WARP also answers undocumented 54, 55 and 58; see the area report).
    const auto* layout = find_layout(feature);
    if (layout == nullptr) return E_INVALIDARG;
    const u32 expected = caps.is_64bit ? layout->size64 : layout->size32;
    // A NULL pFeatureSupportData faults inside the Windows runtime; the
    // documented failure is the answer here.
    if (data_size != expected || data == 0) return E_INVALIDARG;
    Fields out(memory, data, caps.is_64bit);

    const auto node_ok = [&out](const FieldOffset& node_field) {
        u32 node = 0;
        // One node: NodeIndex 0 only (measured: 1, 2, 0xFFFFFFFF fail with
        // nothing written).
        return out.read(node_field, node) && node == 0;
    };

    switch (feature) {
    case F_D3D12_OPTIONS: {
        u32 per_resource = 0, per_process = 0;
        gpu_virtual_address_bits(caps, per_resource, per_process);
        out.put(f::D3D12_OPTIONS_DoublePrecisionFloatShaderOps, caps.double_precision_shader_ops);
        out.put(f::D3D12_OPTIONS_OutputMergerLogicOp, caps.logic_ops);
        // Minimum precision is a floor (Learn, HLSL minimum precision): the
        // translator runs min16/min10 at 32 bits, as D3D11 reports it.
        out.put(f::D3D12_OPTIONS_MinPrecisionSupport,
                SHADER_MIN_PRECISION_SUPPORT_10_BIT | SHADER_MIN_PRECISION_SUPPORT_16_BIT);
        out.put(f::D3D12_OPTIONS_TiledResourcesTier, caps.tiled_resources_tier);
        out.put(f::D3D12_OPTIONS_ResourceBindingTier, caps.resource_binding_tier);
        out.put(f::D3D12_OPTIONS_PSSpecifiedStencilRefSupported, 0);
        out.put(f::D3D12_OPTIONS_TypedUAVLoadAdditionalFormats,
                caps.typed_uav_load_additional_formats);
        out.put(f::D3D12_OPTIONS_ROVsSupported, 0);
        out.put(f::D3D12_OPTIONS_ConservativeRasterizationTier, 0);
        out.put(f::D3D12_OPTIONS_MaxGPUVirtualAddressBitsPerResource, per_resource);
        out.put(f::D3D12_OPTIONS_StandardSwizzle64KBSupported, 0);
        out.put(f::D3D12_OPTIONS_CrossNodeSharingTier, 0);
        out.put(f::D3D12_OPTIONS_CrossAdapterRowMajorTextureSupported, 0);
        out.put(f::D3D12_OPTIONS_VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation,
                caps.vp_and_rt_array_index_from_any_shader);
        out.put(f::D3D12_OPTIONS_ResourceHeapTier, caps.resource_heap_tier);
        return S_OK;
    }
    case F_ARCHITECTURE:
        if (!node_ok(f::ARCHITECTURE_NodeIndex)) return E_INVALIDARG;
        out.put(f::ARCHITECTURE_TileBasedRenderer, caps.tile_based_renderer);
        out.put(f::ARCHITECTURE_UMA, caps.unified_memory);
        out.put(f::ARCHITECTURE_CacheCoherentUMA, caps.cache_coherent_uma);
        return S_OK;
    case F_ARCHITECTURE1:
        if (!node_ok(f::ARCHITECTURE1_NodeIndex)) return E_INVALIDARG;
        out.put(f::ARCHITECTURE1_TileBasedRenderer, caps.tile_based_renderer);
        out.put(f::ARCHITECTURE1_UMA, caps.unified_memory);
        out.put(f::ARCHITECTURE1_CacheCoherentUMA, caps.cache_coherent_uma);
        // IsolatedMMU: the GPU does not honour the guest's page protections
        // (write watch, PAGE_READONLY) -- Metal maps its own view.
        out.put(f::ARCHITECTURE1_IsolatedMMU, 0);
        return S_OK;
    case F_FEATURE_LEVELS: {
        u32 count = 0;
        GuestAddr levels = 0;
        if (!out.read(f::FEATURE_LEVELS_NumFeatureLevels, count) ||
            !out.read_pointer(f::FEATURE_LEVELS_pFeatureLevelsRequested, levels) || count == 0 ||
            levels == 0) {
            return E_INVALIDARG;  // measured: nothing written
        }
        // Measured: the highest requested value not above the device's
        // level, whatever it is (0x1234 counts); none -> 0 written and
        // DXGI_ERROR_UNSUPPORTED.
        const u32 device_max = max_feature_level(caps);
        u32 best = 0;
        for (u32 i = 0; i < count; ++i) {
            const auto level = memory.read_u32(levels + static_cast<u64>(i) * 4);
            if (level.is_err()) return E_INVALIDARG;
            if (level.value() <= device_max && level.value() > best) best = level.value();
        }
        out.put(f::FEATURE_LEVELS_MaxSupportedFeatureLevel, best);
        return best == 0 ? DXGI_ERROR_UNSUPPORTED : S_OK;
    }
    case F_FORMAT_SUPPORT: {
        u32 format = 0;
        if (!out.read(f::FORMAT_SUPPORT_Format, format)) return E_INVALIDARG;
        const FormatSupport12 answer = format_support(format, caps);
        out.put(f::FORMAT_SUPPORT_Support1, answer.support1);
        out.put(f::FORMAT_SUPPORT_Support2, answer.support2);
        return answer.hr;
    }
    case F_MULTISAMPLE_QUALITY_LEVELS: {
        u32 format = 0, count = 0, flags = 0;
        if (!out.read(f::MULTISAMPLE_QUALITY_LEVELS_Format, format) ||
            !out.read(f::MULTISAMPLE_QUALITY_LEVELS_SampleCount, count) ||
            !out.read(f::MULTISAMPLE_QUALITY_LEVELS_Flags, flags)) {
            return E_INVALIDARG;
        }
        bool write = false;
        u32 levels = 0;
        const u32 hr = multisample_quality_levels(format, count, flags, caps, write, levels);
        if (write) out.put(f::MULTISAMPLE_QUALITY_LEVELS_NumQualityLevels, levels);
        return hr;
    }
    case F_FORMAT_INFO: {
        u32 format = 0;
        if (!out.read(f::FORMAT_INFO_Format, format)) return E_INVALIDARG;
        const u32 planes = format_plane_count(format);
        if (planes == 0) return E_INVALIDARG;  // measured: PlaneCount untouched
        out.put8(f::FORMAT_INFO_PlaneCount, static_cast<u8>(planes));
        return S_OK;
    }
    case F_GPU_VIRTUAL_ADDRESS_SUPPORT: {
        u32 per_resource = 0, per_process = 0;
        gpu_virtual_address_bits(caps, per_resource, per_process);
        out.put(f::GPU_VIRTUAL_ADDRESS_SUPPORT_MaxGPUVirtualAddressBitsPerResource, per_resource);
        out.put(f::GPU_VIRTUAL_ADDRESS_SUPPORT_MaxGPUVirtualAddressBitsPerProcess, per_process);
        return S_OK;
    }
    case F_SHADER_MODEL: {
        // Measured: HighestShaderModel must be a D3D_SHADER_MODEL value
        // (5_1 or 6_0..6_9; 0, 4_0, 5_0, 0x52 and 0x6A fail with it
        // untouched); the answer is the lower of it and the device's.
        // DXBC only (PLAN decision 1): 5.1.
        u32 requested = 0;
        if (!out.read(f::SHADER_MODEL_HighestShaderModel, requested)) return E_INVALIDARG;
        if (requested != SHADER_MODEL_5_1 &&
            (requested < SHADER_MODEL_6_0 || requested > SHADER_MODEL_6_9)) {
            return E_INVALIDARG;
        }
        out.put(f::SHADER_MODEL_HighestShaderModel, std::min(requested, SHADER_MODEL_5_1));
        return S_OK;
    }
    case F_D3D12_OPTIONS1:
        // No SM6 wave intrinsics or int64 (DXBC only). The wave width and
        // lane count are the hardware's SIMD groups; with no lane total
        // from the backend the width itself is the (true) lower bound.
        out.put(f::D3D12_OPTIONS1_WaveOps, 0);
        out.put(f::D3D12_OPTIONS1_WaveLaneCountMin, caps.simd_width);
        out.put(f::D3D12_OPTIONS1_WaveLaneCountMax, caps.simd_width);
        out.put(f::D3D12_OPTIONS1_TotalLaneCount,
                caps.total_lane_count != 0 ? caps.total_lane_count : caps.simd_width);
        // Learn: "If CheckFeatureSupport succeeds this value will always be
        // true."
        out.put(f::D3D12_OPTIONS1_ExpandedComputeResourceStates, 1);
        out.put(f::D3D12_OPTIONS1_Int64ShaderOps, 0);
        return S_OK;
    case F_PROTECTED_RESOURCE_SESSION_SUPPORT:
        if (!node_ok(f::PROTECTED_RESOURCE_SESSION_SUPPORT_NodeIndex)) return E_INVALIDARG;
        out.put(f::PROTECTED_RESOURCE_SESSION_SUPPORT_Support, 0);
        return S_OK;
    case F_ROOT_SIGNATURE: {
        // Measured: HighestVersion must be 1_0..1_2 (0 and 4+ fail
        // untouched); the answer is the lower of it and the parser's.
        u32 requested = 0;
        if (!out.read(f::ROOT_SIGNATURE_HighestVersion, requested)) return E_INVALIDARG;
        if (requested < ROOT_SIGNATURE_VERSION_1_0 || requested > ROOT_SIGNATURE_VERSION_1_2) {
            return E_INVALIDARG;
        }
        out.put(f::ROOT_SIGNATURE_HighestVersion, std::min(requested, caps.root_signature_version));
        return S_OK;
    }
    case F_D3D12_OPTIONS2:
        out.put(f::D3D12_OPTIONS2_DepthBoundsTestSupported, 0);  // Metal has none
        out.put(f::D3D12_OPTIONS2_ProgrammableSamplePositionsTier,
                caps.programmable_sample_positions_tier);
        return S_OK;
    case F_SHADER_CACHE:
        out.put(f::SHADER_CACHE_SupportFlags, caps.shader_cache_support);
        return S_OK;
    case F_COMMAND_QUEUE_PRIORITY: {
        // Measured: CommandListType 0..6 and Priority NORMAL / HIGH /
        // GLOBAL_REALTIME, anything else E_INVALIDARG with nothing written.
        u32 type = 0, priority = 0;
        if (!out.read(f::COMMAND_QUEUE_PRIORITY_CommandListType, type) ||
            !out.read(f::COMMAND_QUEUE_PRIORITY_Priority, priority) ||
            type > D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE ||
            (priority != COMMAND_QUEUE_PRIORITY_NORMAL && priority != COMMAND_QUEUE_PRIORITY_HIGH &&
             priority != COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)) {
            return E_INVALIDARG;
        }
        // Queues exist for DIRECT, COMPUTE and COPY; a bundle is no queue
        // and there is no video. GLOBAL_REALTIME is never honoured.
        const bool queue = type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
                           type == D3D12_COMMAND_LIST_TYPE_COPY;
        const bool supported =
            queue && (priority == COMMAND_QUEUE_PRIORITY_NORMAL ||
                      (priority == COMMAND_QUEUE_PRIORITY_HIGH && caps.high_priority_queues));
        out.put(f::COMMAND_QUEUE_PRIORITY_PriorityForTypeIsSupported, supported);
        return S_OK;
    }
    case F_D3D12_OPTIONS3:
        // Every queue shares the one backend stream, so a COPY list's
        // TIMESTAMP is recorded like a DIRECT one's (d3d12_replay.cpp).
        out.put(f::D3D12_OPTIONS3_CopyQueueTimestampQueriesSupported, 1);
        out.put(f::D3D12_OPTIONS3_CastingFullyTypedFormatSupported, caps.casting_fully_typed_formats);
        out.put(f::D3D12_OPTIONS3_WriteBufferImmediateSupportFlags,
                caps.write_buffer_immediate_support);
        out.put(f::D3D12_OPTIONS3_ViewInstancingTier, 0);
        out.put(f::D3D12_OPTIONS3_BarycentricsSupported, 0);
        return S_OK;
    case F_EXISTING_HEAPS:
        out.put(f::EXISTING_HEAPS_Supported, 0);
        return S_OK;
    case F_D3D12_OPTIONS4:
        out.put(f::D3D12_OPTIONS4_MSAA64KBAlignedTextureSupported, caps.msaa_64kb_aligned_textures);
        out.put(f::D3D12_OPTIONS4_SharedResourceCompatibilityTier, 0);
        out.put(f::D3D12_OPTIONS4_Native16BitShaderOpsSupported, 0);
        return S_OK;
    case F_SERIALIZATION:
        if (!node_ok(f::SERIALIZATION_NodeIndex)) return E_INVALIDARG;
        out.put(f::SERIALIZATION_HeapSerializationTier, 0);
        return S_OK;
    case F_CROSS_NODE:
        out.put(f::CROSS_NODE_SharingTier, 0);
        out.put(f::CROSS_NODE_AtomicShaderInstructions, 0);
        return S_OK;
    case F_D3D12_OPTIONS5:
        out.put(f::D3D12_OPTIONS5_SRVOnlyTiledResourceTier3, 0);
        out.put(f::D3D12_OPTIONS5_RenderPassesTier, 0);  // D3D12_RENDER_PASS_TIER_0
        out.put(f::D3D12_OPTIONS5_RaytracingTier, 0);
        return S_OK;
    case F_DISPLAYABLE:
        out.put(f::DISPLAYABLE_DisplayableTexture, 0);
        out.put(f::DISPLAYABLE_SharedResourceCompatibilityTier, 0);
        return S_OK;
    case F_D3D12_OPTIONS6:
        out.put(f::D3D12_OPTIONS6_AdditionalShadingRatesSupported, 0);
        out.put(f::D3D12_OPTIONS6_PerPrimitiveShadingRateSupportedWithViewportIndexing, 0);
        out.put(f::D3D12_OPTIONS6_VariableShadingRateTier, 0);
        out.put(f::D3D12_OPTIONS6_ShadingRateImageTileSize, 0);
        out.put(f::D3D12_OPTIONS6_BackgroundProcessingSupported, 0);
        return S_OK;
    case F_QUERY_META_COMMAND:
        // No meta command exists on this device: every CommandId is unknown
        // (WARP likewise fails every query, writing nothing).
        return E_INVALIDARG;
    case F_D3D12_OPTIONS7:
        out.put(f::D3D12_OPTIONS7_MeshShaderTier, 0);
        out.put(f::D3D12_OPTIONS7_SamplerFeedbackTier, 0);
        return S_OK;
    case F_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
        if (!node_ok(f::PROTECTED_RESOURCE_SESSION_TYPE_COUNT_NodeIndex)) return E_INVALIDARG;
        out.put(f::PROTECTED_RESOURCE_SESSION_TYPE_COUNT_Count, 0);
        return S_OK;
    case F_PROTECTED_RESOURCE_SESSION_TYPES: {
        // Count must equal TYPE_COUNT's answer (0); measured: 1 and 2 fail,
        // 0 succeeds whatever pTypes is, and nothing is written.
        u32 count = 0;
        if (!node_ok(f::PROTECTED_RESOURCE_SESSION_TYPES_NodeIndex) ||
            !out.read(f::PROTECTED_RESOURCE_SESSION_TYPES_Count, count) || count != 0) {
            return E_INVALIDARG;
        }
        return S_OK;
    }
    case F_D3D12_OPTIONS8:
        out.put(f::D3D12_OPTIONS8_UnalignedBlockTexturesSupported, caps.unaligned_block_textures);
        return S_OK;
    case F_D3D12_OPTIONS9:
        out.put(f::D3D12_OPTIONS9_MeshShaderPipelineStatsSupported, 0);
        out.put(f::D3D12_OPTIONS9_MeshShaderSupportsFullRangeRenderTargetArrayIndex, 0);
        out.put(f::D3D12_OPTIONS9_AtomicInt64OnTypedResourceSupported, 0);
        out.put(f::D3D12_OPTIONS9_AtomicInt64OnGroupSharedSupported, 0);
        out.put(f::D3D12_OPTIONS9_DerivativesInMeshAndAmplificationShadersSupported, 0);
        out.put(f::D3D12_OPTIONS9_WaveMMATier, 0);
        return S_OK;
    case F_D3D12_OPTIONS10:
        out.put(f::D3D12_OPTIONS10_VariableRateShadingSumCombinerSupported, 0);
        out.put(f::D3D12_OPTIONS10_MeshShaderPerPrimitiveShadingRateSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS11:
        out.put(f::D3D12_OPTIONS11_AtomicInt64OnDescriptorHeapResourceSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS12:
        // No mesh shaders: the culled-primitives statistic is FALSE (WARP).
        out.put(f::D3D12_OPTIONS12_MSPrimitivesPipelineStatisticIncludesCulledPrimitives, 0);
        out.put(f::D3D12_OPTIONS12_EnhancedBarriersSupported, caps.enhanced_barriers);
        out.put(f::D3D12_OPTIONS12_RelaxedFormatCastingSupported, caps.relaxed_format_casting);
        return S_OK;
    case F_D3D12_OPTIONS13:
        out.put(f::D3D12_OPTIONS13_UnrestrictedBufferTextureCopyPitchSupported, 0);
        out.put(f::D3D12_OPTIONS13_UnrestrictedVertexElementAlignmentSupported, 0);
        out.put(f::D3D12_OPTIONS13_InvertedViewportHeightFlipsYSupported, 0);
        out.put(f::D3D12_OPTIONS13_InvertedViewportDepthFlipsZSupported, 0);
        out.put(f::D3D12_OPTIONS13_TextureCopyBetweenDimensionsSupported, 0);
        out.put(f::D3D12_OPTIONS13_AlphaBlendFactorSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS14:
        out.put(f::D3D12_OPTIONS14_AdvancedTextureOpsSupported, 0);
        out.put(f::D3D12_OPTIONS14_WriteableMSAATexturesSupported, 0);
        out.put(f::D3D12_OPTIONS14_IndependentFrontAndBackStencilRefMaskSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS15:
        out.put(f::D3D12_OPTIONS15_TriangleFanSupported, 0);
        out.put(f::D3D12_OPTIONS15_DynamicIndexBufferStripCutSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS16:
        out.put(f::D3D12_OPTIONS16_DynamicDepthBiasSupported, 0);
        out.put(f::D3D12_OPTIONS16_GPUUploadHeapSupported, GPU_UPLOAD_HEAP_SUPPORTED ? 1u : 0u);
        return S_OK;
    case F_D3D12_OPTIONS17:
        out.put(f::D3D12_OPTIONS17_NonNormalizedCoordinateSamplersSupported, 0);
        out.put(f::D3D12_OPTIONS17_ManualWriteTrackingResourceSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS18:
        out.put(f::D3D12_OPTIONS18_RenderPassesValid, 0);
        return S_OK;
    case F_D3D12_OPTIONS19:
        out.put(f::D3D12_OPTIONS19_MismatchingOutputDimensionsSupported, 0);
        out.put(f::D3D12_OPTIONS19_SupportedSampleCountsWithNoOutputs,
                caps.sample_counts_with_no_outputs);
        out.put(f::D3D12_OPTIONS19_PointSamplingAddressesNeverRoundUp, 0);
        out.put(f::D3D12_OPTIONS19_RasterizerDesc2Supported, 0);
        out.put(f::D3D12_OPTIONS19_NarrowQuadrilateralLinesSupported, 0);
        out.put(f::D3D12_OPTIONS19_AnisoFilterWithPointMipSupported, 0);
        // The D3D12 shader-visible heap limits (d3d12.h
        // D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE and
        // _DESCRIPTOR_HEAP_SIZE_TIER_1/2), the Metal argument-buffer sizes
        // descriptor tables resolve into.
        out.put(f::D3D12_OPTIONS19_MaxSamplerDescriptorHeapSize, MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
        out.put(f::D3D12_OPTIONS19_MaxSamplerDescriptorHeapSizeWithStaticSamplers,
                MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
        out.put(f::D3D12_OPTIONS19_MaxViewDescriptorHeapSize,
                MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
        out.put(f::D3D12_OPTIONS19_ComputeOnlyCustomHeapSupported, 0);
        return S_OK;
    case F_D3D12_OPTIONS20:
        out.put(f::D3D12_OPTIONS20_ComputeOnlyWriteWatchSupported, 0);
        out.put(f::D3D12_OPTIONS20_RecreateAtTier, 0);
        return S_OK;
    case F_PREDICATION:
    case F_PLACED_RESOURCE_SUPPORT_INFO:
    case F_HARDWARE_COPY:
        // The 26100 runtime answers these three E_UNEXPECTED at the right
        // size, writing nothing (d3d12_caps_probe [scan]/[input]).
        return E_UNEXPECTED_HR;
    case F_D3D12_OPTIONS21:
        out.put(f::D3D12_OPTIONS21_WorkGraphsTier, 0);
        out.put(f::D3D12_OPTIONS21_ExecuteIndirectTier, caps.execute_indirect_tier);
        out.put(f::D3D12_OPTIONS21_SampleCmpGradientAndBiasSupported, 0);
        out.put(f::D3D12_OPTIONS21_ExtendedCommandInfoSupported, 0);
        return S_OK;
    case F_APPLICATION_SPECIFIC_DRIVER_STATE:
        out.put(f::APPLICATION_SPECIFIC_DRIVER_STATE_Supported, 0);
        return S_OK;
    case F_BYTECODE_BYPASS_HASH_SUPPORTED:
        out.put(f::BYTECODE_BYPASS_HASH_SUPPORTED_Supported, 0);
        return S_OK;
    case F_SHADER_CACHE_ABI_SUPPORT: {
        // No driver shader-cache ABI: WARP zeroes the whole structure and
        // fails E_FAIL (d3d12_caps_probe [dump]).
        const std::vector<u8> zeros(expected, 0);
        (void)memory.write_bytes(data, zeros.data(), zeros.size());
        return E_FAIL;
    }
    default:
        return E_INVALIDARG;
    }
}

void marshal_check_feature_support(Marshaler& m, GuestMemory& memory, const CapsContext& caps) {
    m.set_return(check_feature_support(memory, caps, m.arg32(1), m.arg_ptr(2), m.arg32(3)));
}

}  // namespace vitreo::winapi::d3d12
