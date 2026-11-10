// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/d3d12/d3d12_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"

#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"

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
constexpr u32 SHADER_MODEL_6_5 = 0x65u;
constexpr u32 SHADER_MODEL_6_9 = 0x69u;
// d3d12.h D3D12_MESH_SHADER_TIER.
constexpr u32 D3D12_MESH_SHADER_TIER_1 = 0xAu;

/// DirectX-Specs "Mesh Shader": mesh shaders are shader model 6.5, so the
/// tier is claimed only when that model is reportable.
[[nodiscard]] bool mesh_shader_tier1(const CapsContext& caps) {
    return caps.mesh_pipelines && caps.highest_shader_model >= SHADER_MODEL_6_5;
}
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
constexpr u32 kSupport1Buffer = 0x1u;
constexpr u32 kSupport1Texture3D = 0x40u;  // D3D12_FORMAT_SUPPORT1_TEXTURE3D
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
    // and (where 1x exists) 4x. The tier is the frontend's to claim
    // (Frontend::caps, CapsContext::tiled_resources_tier).
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
    // Learn, "Hardware Tiers", tier 2: SRVs in all descriptor tables per
    // stage = the full heap (unbounded and long t# arrays read the heap in
    // place, DxbcTranslationOptions::heap_window), 2048 samplers per stage
    // (the whole sampler heap, copied); u# and b# arrays read it the same
    // way, which tier 3 needs (Frontend::caps raises it with FL 11_1). Every descriptor
    // table needs Metal Tier-2 argument buffers.
    caps.resource_binding_tier = backend.supports_descriptor_tables() ? 2u : 1u;
    caps.raytracing = const_cast<graphics::GraphicsBackendDevice&>(backend).raytracing() != nullptr;
    caps.unified_memory = backend.has_unified_memory();
    // Metal shared storage on a unified-memory GPU is coherent with the CPU
    // caches (write-back upload heaps, Learn D3D12_FEATURE_DATA_ARCHITECTURE1).
    caps.cache_coherent_uma = caps.unified_memory;
    caps.max_buffer_length = backend.max_buffer_length();
    // The translator's exact soft-double emulation, as the D3D11 device
    // reports DoublePrecisionFloatShaderOps.
    caps.double_precision_shader_ops = true;
    // The highest shader model whose every mandatory feature is real (the
    // DXIL lowering, src/graphics/shaders/dxil_lower*.cpp):
    //  - 6.0: every DXIL op of the model in VS/HS/DS/GS/PS/CS, with the
    //    optional caps Vitreo claims (WaveOps, Int64ShaderOps, doubles)
    //    complete;
    //  - 6.1: SV_ViewID and SV_Barycentrics are optional (ViewInstancingTier,
    //    BarycentricsSupported), so 6.1 adds nothing mandatory;
    //  - 6.2: "fp32-denorm-mode" (DXIL.rst, Functions; DXC wiki "Denorm
    //    Mode": preserve keeps float32 denormals, ftz flushes them) is the
    //    model's mandatory addition. The Apple GPU flushes float32
    //    denormals, so preserve runs exactly in software (VX_F32P,
    //    soft_float.hpp; bit-exact with WARP12 and the RTX 4080 on the
    //    d3d12_denorm corpus) and ftz on the hardware. 16-bit types are
    //    optional (Native16BitShaderOpsSupported);
    //  - 6.3: DirectX Raytracing and its libraries, optional (RaytracingTier);
    //  - 6.4: dot4add_u8packed / dot4add_i8packed are required ("a
    //    required/supported feature of Shader model 6.4", Learn HLSL SM
    //    6.4) and lowered (DXIL 163 / 164), dot2add rides on the
    //    low-precision bit; SV_ShadingRate is optional (VRS tier 2);
    //  - 6.5: WaveMatch / WaveMultiPrefix* (DXIL 165-167) are wave ops,
    //    claimed with WaveOps and lowered; mesh / amplification, sampler
    //    feedback and DXR 1.1 are optional (their tiers).
    // 6.6 is not claimed: it requires every 64-bit integer atomic on raw
    // buffers whenever Int64ShaderOps is set (HLSL SM 6.6 Int64 and Float
    // Atomics: "must support all atomic operations with 64-bit integer typed
    // value parameters that are methods of RWByteAddressBuffer" or of a
    // structured buffer, as root descriptors), and Metal has 64-bit atomic
    // min / max only (no fetch, no exchange, no add). The only emulation is
    // a lock (a hashed table of 32-bit CAS locks around a two-word update),
    // and it needs every lane that holds a lock to keep running while the
    // others spin: no Apple document promises that forward progress (the MSL
    // specification defines no progress guarantee for SIMD-groups or
    // threadgroups), and a stress run on the M4 Pro (256 x 1024 threads, one
    // lock, divergent lanes) kept a lane spinning past 4M iterations for 75 s
    // of GPU time, so it is not shipped. The rest of 6.6 is built:
    // dynamic resources, compute-shader derivatives and quads
    // (dxil_lower_quads.inc), IsHelperLane, pack / unpack, WaveSize 32.
    // Int64ShaderOps stays reported (it is real); it is what makes the
    // atomics mandatory, and is not withdrawn to claim 6.6.
    caps.highest_shader_model = SHADER_MODEL_6_5;
    // DirectX-Specs "Sampler Feedback": TIER_0_9 restricts feedback to WRAP
    // / CLAMP samplers and to SRVs of the whole resource; TIER_1_0 lifts
    // both. Every address mode is implemented (dxil_lower_feedback.cpp), but
    // the writes take the map's own mip chain and array as the sampled view,
    // so a partial SRV (MostDetailedMip, MipLevels, FirstArraySlice,
    // ResourceMinLODClamp) is not honoured: 0.9.
    caps.sampler_feedback_tier = caps.highest_shader_model >= SHADER_MODEL_6_5 ? 0x5Au : 0u;
    caps.mesh_pipelines = backend.supports_mesh_pipelines();
    // OPTIONS8 UnalignedBlockTexturesSupported (Learn): a block-compressed
    // texture's top level need not be a multiple of 4. Metal creates, fills
    // and reads back such a texture (measured on the M4 Pro under API
    // validation: BC1 6x6 with 3 levels, buffer copies of 6x6 and 3x3), and
    // the backend copies cut D3D12's whole-block boxes and footprints at the
    // level's edge (metal_device_copy.mm clamp_block_extent).
    caps.unaligned_block_textures = true;
    return caps;
}

u32 max_feature_level(const CapsContext& caps) noexcept {
    // Learn, Hardware feature levels ("Feature Level Support" table). A
    // level is reported only when every row it requires holds; the
    // "Optional" rows are not checked.
    const bool fl11_1 = caps.logic_ops && caps.target_independent_rasterization &&
                        (caps.sample_counts_with_no_outputs & 0x10u) != 0;
    if (!fl11_1) return D3D_FEATURE_LEVEL_11_0;
    const bool fl12_0 = caps.resource_binding_tier >= 2 && caps.tiled_resources_tier >= 2 &&
                        caps.typed_uav_load_additional_formats && caps.min_max_filtering &&
                        caps.highest_shader_model >= SHADER_MODEL_6_0;
    if (!fl12_0) return D3D_FEATURE_LEVEL_11_1;
    if (caps.conservative_rasterization_tier >= 1 && caps.rovs) return D3D_FEATURE_LEVEL_12_1;
    return D3D_FEATURE_LEVEL_12_0;
}

FormatSupport12 format_support(u32 dxgi_format, const CapsContext& caps) noexcept {
    // A4B4G4R4 (191) has no texture storage here; a typed buffer view of it
    // is read as raw memory (d3d12_typed_raw.cpp), so of the bits Windows
    // reports for it (d3d12_caps_probe [format] 191 s1=0x02e4d3f3) BUFFER
    // alone is honoured.
    if (dxgi_format == 191u && typed_raw_element_bytes(dxgi_format) != 0) {
        return {S_OK, kSupport1Buffer, 0};
    }
    // Measured on WARP and the RTX 4080 SUPER (d3d12_a41_format_sweep):
    // a sampler feedback format is TEXTURE2D | MIP and SAMPLER_FEEDBACK.
    if ((dxgi_format == 189 || dxgi_format == 190) && caps.sampler_feedback_tier != 0) {
        return {S_OK, 0x1020u, 0x8000u};
    }
    // The other D3D12-only formats have no Metal path here; a format the
    // runtime does not know fails as on Windows.
    if (format_plane_count(dxgi_format) == 0 || dxgi_format >= 189) {
        return {E_FAIL, 0, 0};
    }
    const d3d11::FormatCapsContext formats = d3d11_formats(caps);
    const auto one = d3d11::check_format_support(dxgi_format, formats);
    if (one.hr != S_OK) return {E_FAIL, 0, 0};
    u32 support1 = one.mask & kSupport1Defined & ~kSupport1Video;
    // BUFFER for a format the backend has no texture buffer of (B5G6R5,
    // B5G5R5A1, B8G8R8X8, B4G4R4A4, R9G9B9E5): its typed buffer views are
    // read as raw memory, so the Windows bit holds here too.
    if ((d3d11::unhonoured_format_support(dxgi_format, formats) &
         kSupport1Buffer) != 0 &&
        typed_raw_element_bytes(dxgi_format) != 0) {
        support1 |= kSupport1Buffer;
    }
    // TEXTURE3D for B8G8R8X8 (88, 92, 93): D3D11 binds a volume without a
    // view, so its table leaves the bit out, but a D3D12 volume SRV is a
    // view whose component mapping forces the X8 alpha to 1
    // (Replayer::bind_view), so the Windows bit holds here.
    // B4G4R4A4 (115) likewise: its volume view composes the ABGR4 storage
    // permutation into the component mapping.
    if ((dxgi_format == 88 || dxgi_format == 92 || dxgi_format == 93 ||
         dxgi_format == 115) &&
        (d3d11::unhonoured_format_support(dxgi_format, formats) &
         kSupport1Texture3D) != 0) {
        support1 |= kSupport1Texture3D;
    }
    // IA_VERTEX_BUFFER of B5G6R5, B5G5R5A1, B8G8R8X8 and B4G4R4A4 (85, 86,
    // 88, 115): Metal has no vertex format of them; a pipeline whose layout
    // reads one fetches its inputs in the vertex shader, which unpacks them
    // (d3d11::pulled_only_vertex_format_bytes, VertexPullKind).
    if (d3d11::pulled_only_vertex_format_bytes(dxgi_format) != 0) {
        support1 |= d3d11::unhonoured_format_support(dxgi_format, formats) &
                    d3d11::FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    }
    // R8G8_B8G8 / G8R8_G8B8 (68, 69): stored as decoded RGBA8, whose
    // uploads, copies and readbacks transcode the 2x1 blocks
    // (graphics::format_422.hpp, texture_format_for), so the texture, load,
    // sample, gather and mip bits Windows reports hold. The shared D3D11
    // table has no storage for them.
    if (dxgi_format == 68 || dxgi_format == 69) {
        constexpr u32 kDecoded422 =
            d3d11::FORMAT_SUPPORT_TEXTURE1D | d3d11::FORMAT_SUPPORT_TEXTURE2D |
            d3d11::FORMAT_SUPPORT_TEXTURE3D | d3d11::FORMAT_SUPPORT_TEXTURECUBE |
            d3d11::FORMAT_SUPPORT_SHADER_LOAD | d3d11::FORMAT_SUPPORT_SHADER_SAMPLE |
            d3d11::FORMAT_SUPPORT_SHADER_GATHER | d3d11::FORMAT_SUPPORT_MIP;
        support1 |= (one.mask | d3d11::unhonoured_format_support(dxgi_format, formats)) &
                    kDecoded422;
    }
    if (support1 == 0) return {E_FAIL, 0, 0};
    const auto two = d3d11::check_format_support2(dxgi_format, formats);
    u32 support2 = two.hr == S_OK ? two.mask : 0u;
    // DISPLAYABLE belongs to D3D12's displayable textures
    // (D3D12_FEATURE_DISPLAYABLE), which this device does not have; WARP
    // does not report it either (d3d12_caps_probe [format] 28).
    support2 &= ~(kSupport2Shareable | kSupport2Displayable);
    // UAV_ATOMIC_EXCHANGE of R32_FLOAT (WARP and RTX: s2 0x2c8): a float
    // typed UAV that takes atomics is bound through an R32_UINT view
    // (DxbcTranslationOptions::typed_uav_uint_views, Replayer::bind_view),
    // on which Metal's texture atomics run. The shared D3D11 table, whose
    // frontend binds no such view, keeps the bit out.
    if (dxgi_format == 41u) {
        support2 |= d3d11::windows_support2(dxgi_format) &
                    d3d11::FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE;
    }
    // Learn (Typed unordered access view loads): R32_FLOAT / R32_UINT /
    // R32_SINT (41..43) always load; every other format's UAV_TYPED_LOAD
    // belongs to the all-or-nothing group behind
    // OPTIONS::TypedUAVLoadAdditionalFormats, so the bit follows that cap.
    if (!caps.typed_uav_load_additional_formats && (dxgi_format < 41 || dxgi_format > 43)) {
        support2 &= ~d3d11::FORMAT_SUPPORT2_UAV_TYPED_LOAD;
    }
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
        out.put(f::D3D12_OPTIONS_ROVsSupported, caps.rovs);
        out.put(f::D3D12_OPTIONS_ConservativeRasterizationTier,
                caps.conservative_rasterization_tier);
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
        // untouched); the answer is the lower of it and the device's
        // (CapsContext::highest_shader_model).
        u32 requested = 0;
        if (!out.read(f::SHADER_MODEL_HighestShaderModel, requested)) return E_INVALIDARG;
        if (requested != SHADER_MODEL_5_1 &&
            (requested < SHADER_MODEL_6_0 || requested > SHADER_MODEL_6_9)) {
            return E_INVALIDARG;
        }
        out.put(f::SHADER_MODEL_HighestShaderModel, std::min(requested, caps.highest_shader_model));
        return S_OK;
    }
    case F_D3D12_OPTIONS1:
        // SM6 wave intrinsics lower to the SIMD-group functions
        // (dxil_lower_wave.cpp, dxbc_to_spirv_vitreo_ext.cpp), so they need
        // a known SIMD width. The wave width and lane count are the
        // hardware's SIMD groups; with no lane total from the backend the
        // width itself is the (true) lower bound.
        out.put(f::D3D12_OPTIONS1_WaveOps, caps.simd_width != 0 ? 1u : 0u);
        out.put(f::D3D12_OPTIONS1_WaveLaneCountMin, caps.simd_width);
        out.put(f::D3D12_OPTIONS1_WaveLaneCountMax, caps.simd_width);
        out.put(f::D3D12_OPTIONS1_TotalLaneCount,
                caps.total_lane_count != 0 ? caps.total_lane_count : caps.simd_width);
        // Learn: "If CheckFeatureSupport succeeds this value will always be
        // true."
        out.put(f::D3D12_OPTIONS1_ExpandedComputeResourceStates, 1);
        // 64-bit integer arithmetic (dxil_lower_wide.cpp, VX_I64 on MSL
        // long / ulong). 64-bit atomics are the separate SM 6.6 caps
        // (OPTIONS9 / OPTIONS11), not claimed.
        out.put(f::D3D12_OPTIONS1_Int64ShaderOps, 1);
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
        // ViewInstancingTier 3 (DirectX-Specs "View Instancing": "The
        // implementation of view instancing always occurs at the first
        // shader stage that references SV_ViewID or the rasterizer,
        // whichever is earlier"). Each pipeline records that stage
        // (PipelineState::vi_first): a vertex shader reading SV_ViewID loops
        // the whole draw per view; otherwise the vertex shader runs once as
        // a capture and the views start at the hull, domain or geometry
        // shader that reads it, or at the rasterizer (a pass-through per
        // view), each view with its own pipeline and location
        // (Replayer::draw_lowered at_stage). Mesh pipelines: the
        // amplification shader runs once (records replayed per view) and a
        // mesh shader that does not read SV_ViewID runs once, its outputs
        // replayed per view (d3d12_replay_view_mesh.cpp). Pipeline
        // statistics: "For the stages that come before X, view instances do
        // not increase those stages' pipeline statistics" (the input
        // assembler counts once too). Metal vertex amplification is not
        // used: it reruns the vertex function per view (measured on the M4
        // Pro). Probed: d3d12_a66_c6_probe "vi *" / "vi stats" against the
        // spec and the RTX 4080 SUPER (tier 3); where the RTX differs, the
        // spec is followed: a pixel shader behind a view-blind amplification
        // + mesh pair reads its true SV_ViewID (the RTX gives 0, but tier 3
        // is "functionally the same as Tier 1"), GSInvocations of a geometry
        // shader reading SV_ViewID count per view, and CInvocations of a draw
        // whose only SV_ViewID reader is the pixel shader count once ("view
        // instancing affects PSInvocations but not the other statistics").
        // Within the first SV_ViewID stage itself the whole stage runs per
        // view ("Redundant shading is limited to just the first SV_ViewID
        // dependent shader stage's code, if not avoided completely").
        out.put(f::D3D12_OPTIONS3_ViewInstancingTier, D3D12_VIEW_INSTANCING_TIER_3);
        // SV_Barycentrics and GetAttributeAtVertex (SM 6.1) in every pipeline
        // shape that rasterizes: vertex shader (lists, strips, lines,
        // points), with vertex-shader stream output, geometry shader
        // (triangles, lines), hull + domain shader (triangles, isolines),
        // mesh shader, and each of them under conservative or
        // target-independent rasterization (pass C's own weights and
        // per-vertex copies); a line's third weight is exactly 0. Each view
        // of view instancing is one of these. d3d12_a66_c6_probe "bary *",
        // d3d12_bary_probe, d3d12_a60_c4_probe "prim *".
        out.put(f::D3D12_OPTIONS3_BarycentricsSupported, 1);
        return S_OK;
    case F_EXISTING_HEAPS:
        // OpenExistingHeapFromAddress(1) and OpenExistingHeapFromFileMapping
        // (d3d12_resource.cpp): system memory wrapped as the GPU's.
        out.put(f::EXISTING_HEAPS_Supported, 1);
        return S_OK;
    case F_D3D12_OPTIONS4:
        out.put(f::D3D12_OPTIONS4_MSAA64KBAlignedTextureSupported, caps.msaa_64kb_aligned_textures);
        out.put(f::D3D12_OPTIONS4_SharedResourceCompatibilityTier, 0);
        // Native half / int16 (dxc -enable-16bit-types): IEEE binary16
        // results, 2-byte loads and stores (dxil_lower_wide.cpp).
        out.put(f::D3D12_OPTIONS4_Native16BitShaderOpsSupported, 1);
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
        // Learn (D3D12_RENDER_PASS_TIER): 0 is software emulation. Suspending
        // and resuming passes keep local render-target contents, but this
        // device does not accelerate those writes and does not make UAV
        // writes inside a pass more efficient than the same draws without
        // a pass. Tier 1 is a driver implementation; tier 2 is that UAV
        // efficiency. Neither is this device.
        out.put(f::D3D12_OPTIONS5_RenderPassesTier, 0);
        // D3D12_RAYTRACING_TIER_1_1 (11): DXR 1.0 pipelines and DXR 1.1
        // (inline RayQuery, AddToStateObject, ExecuteIndirect DispatchRays,
        // GeometryIndex, SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES) on a
        // backend with ray tracing (DirectX-Specs Raytracing.md, "Tier 1.1").
        // Per bitness as Windows answers: a 32-bit process gets
        // NOT_SUPPORTED (0) from the same RTX 4080 SUPER that reports 1.2 to
        // a 64-bit one (measured, d3d12_a47_dxr_probe_x86.winoracle-gpu
        // caps "RaytracingTier=0").
        out.put(f::D3D12_OPTIONS5_RaytracingTier, caps.raytracing && caps.is_64bit ? 11u : 0u);
        return S_OK;
    case F_DISPLAYABLE:
        out.put(f::DISPLAYABLE_DisplayableTexture, 0);
        out.put(f::DISPLAYABLE_SharedResourceCompatibilityTier, 0);
        return S_OK;
    case F_D3D12_OPTIONS6:
        // Variable-rate shading stays NOT_SUPPORTED (DirectX-Specs "Variable
        // Rate Shading"). Tier 1 promises, per draw, one pixel-shader
        // invocation per coarse pixel (1x2, 2x1, 2x2) whose result is
        // broadcast to every covered sample, attributes and SV_Position at
        // the coarse pixel's centre, and gradients taken between coarse
        // pixels; only the listed cases (1x2, programmable sample
        // positions, conservative raster, SV_Coverage, partial SampleMask,
        // sample-rate shading) may fall back to fine shading -- MSAA is not
        // one of them, and coarse pixels cover up to 16 samples. Metal has
        // no per-draw rate: rasterization rate maps are per pass and
        // screen-space, and they resample the target (they change the
        // pixel grid and the sample count each coarse pixel writes), which
        // is not coarse shading. The emulations considered do not meet it:
        //  - drawing the coarse draw into a 1/2-resolution pass with the
        //    fine pixels as its samples (and copying the targets in and
        //    out) needs coarse-pixel-area x sample-count samples: 2x1 at 4x
        //    MSAA needs 8 and 2x2 at 4x needs 16, and this GPU's largest
        //    sample count is 4 (MTLDevice supportsTextureSampleCount 8 is
        //    false on the M4 Pro);
        //  - sharing one invocation across a 2x2 fragment quad needs Metal
        //    quads aligned to the even pixel grid (Metal does not document
        //    quad placement), coarse-pixel gradients (a quad's lanes are
        //    one coarse pixel apart only if each lane shades a different
        //    coarse pixel, i.e. a whole pixel-shader rewrite with
        //    attributes re-evaluated at other pixels' centres), and
        //    single-invocation UAV side effects; not built.
        // Tier 2 additionally guarantees the rate exactly, SV_ShadingRate
        // (SM 6.4, above the reportable 6.1) and screen-space rate images;
        // it is further out. RSSetShadingRate / RSSetShadingRateImage fail
        // loudly unless they restore the default (d3d12_command_list.cpp).
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
        // d3d12.h D3D12_MESH_SHADER_TIER_1 = 0xA (measured 10 on both
        // oracles, d3d12_a40_mesh_probe "caps").
        out.put(f::D3D12_OPTIONS7_MeshShaderTier,
                mesh_shader_tier1(caps) ? D3D12_MESH_SHADER_TIER_1 : 0u);
        // Sampler feedback (CapsContext::sampler_feedback_tier): feedback
        // maps, CreateSamplerFeedbackUnorderedAccessView, WriteSampler-
        // Feedback* (DXIL 174-177), clears and ENCODE / DECODE transcodes,
        // checked against winoracle-gpu (d3d12_a41_feedback_probe). MIN_MIP
        // decodes a mip-m request over its 2^m x 2^m group of mip-0
        // regions, as the RTX 4080 SUPER does and the spec permits.
        out.put(f::D3D12_OPTIONS7_SamplerFeedbackTier, caps.sampler_feedback_tier);
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
        // AS / MS invocations and MSPrimitives are counted
        // (d3d12_replay_statistics.cpp); a mesh primitive's
        // [[render_target_array_index]] is a full uint.
        out.put(f::D3D12_OPTIONS9_MeshShaderPipelineStatsSupported, mesh_shader_tier1(caps));
        out.put(f::D3D12_OPTIONS9_MeshShaderSupportsFullRangeRenderTargetArrayIndex,
                mesh_shader_tier1(caps));
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
        // MSPrimitives counts every primitive a mesh group outputs, culled
        // ones included (TRUE, as on the RTX 4080); FALSE without mesh
        // shaders (D3D12_TRI_STATE_FALSE).
        out.put(f::D3D12_OPTIONS12_MSPrimitivesPipelineStatisticIncludesCulledPrimitives,
                mesh_shader_tier1(caps) ? 1u : 0u);
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
        // Both are shader model 6.7 features ("Devices that support
        // D3D_SHADER_MODEL_6_7 may optionally support", DirectX-Specs "HLSL
        // Advanced Texture Operations", Device Capability), so neither can be
        // reported while HighestShaderModel is 6.5 (see highest_shader_model
        // in backend_caps_context). Built and tested regardless
        // (tests/oracle_data/d3d12_c4, d3d12_a60_c4_probe): programmable
        // offsets on Sample* / Load (low 4 bits, VX_TEX_OFFSET) and GatherRaw
        // (16-, 32- and 64-bit). Still missing for AdvancedTextureOps:
        // integer sampling with D3D12_SAMPLER_DESC2 / static UINT border
        // colours (dxc 1.9 refuses SampleLevel on a uint texture, so no DXIL
        // exercises it); RelaxedFormatCastingSupported, the spec's stated
        // prerequisite for the raw-gather aliasing, is its own cap.
        // WriteableMSAATextures is unreachable on Metal: TextureStoreSample
        // needs a writable multisampled texture and MSL's texture2d_ms is
        // access::read only (MSL specification 2.9), so it fails loudly in
        // the DXIL lowering.
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
        // D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED, and unreachable rather than
        // missing. DirectX-Specs "Work Graphs", Shader target: node shaders
        // are "compute shaders authored in a library, target lib_6_8 or
        // above", and generic programs "are available to any device that
        // supports SM6.8". HighestShaderModel 6.8 includes 6.6, and 6.6
        // with Int64ShaderOps set "must support all atomic operations with
        // 64-bit integer typed value parameters that are methods of
        // RWByteAddressBuffer" (HLSL SM 6.6 Int64 and Float Atomics, Integer
        // 64-bit Capabilities), which Metal cannot provide (64-bit atomic
        // min / max only; see caps.highest_shader_model in
        // backend_caps_context). So WorkGraphsTier stays 0 for as long as
        // HighestShaderModel is 6.5, whatever else is built, and SetProgram /
        // DispatchGraph fail loudly (d3d12_command_list.cpp).
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
