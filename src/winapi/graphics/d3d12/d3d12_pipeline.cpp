// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_pipeline.cpp
/// @brief ID3D12RootSignature and ID3D12PipelineState: root signature
/// creation (through the root-signature pass's parser), graphics and compute
/// pipeline state creation, shader translation, and the backend pipelines a
/// draw needs.
///
/// Shaders are DXBC (SM 4.0-5.1) through the existing translator; DXIL (SM 6)
/// is lowered to SM 5.1 first (read_shader_bytes, graphics/shaders/dxil_to_dxbc).
/// Vertex, pixel and compute shaders read their registers from descriptor
/// tables (Metal Tier-2 argument buffers), one per register class: set 0 = b#,
/// 1 = t#, 2 = s#, 3 = u#. Geometry, hull and domain shaders are the D3D11
/// compute lowering. A stage whose resources fit the flat constant-buffer
/// window stays flat; shader model 5.1 and any texture, sampler or other
/// resource use those same descriptor tables. Descriptor set 4, the expansion
/// buffers, stays on the flat expansion window.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/shaders/minmax_reduction.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"
#include "graphics/shaders/dxil_container.hpp"
#include "graphics/shaders/dxil_to_dxbc.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_cut_rewrite.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/indirect_count.hpp"
#include "graphics/shaders/raster_emulation.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/tessellator.hpp"
#include "core/memory/guest_memory.hpp"

#if __has_include("winapi/graphics/d3d12/d3d12_root_signature.hpp")
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#define VITREO_D3D12_ROOT_SIGNATURE_PARSER 1
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

namespace vitreo::winapi::d3d12 {
namespace {

// D3D_NAME (d3dcommon.h), stored in DxbcSignatureElement::system_value_type.
// The same values the D3D11 geometry frontend matches.
constexpr u32 D3D_NAME_POSITION = 1;
constexpr u32 D3D_NAME_CLIP_DISTANCE = 2;
constexpr u32 D3D_NAME_CULL_DISTANCE = 3;
constexpr u32 D3D_NAME_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr u32 D3D_NAME_VIEWPORT_ARRAY_INDEX = 5;
constexpr u32 D3D_NAME_BARYCENTRICS = 23;

/// A pixel shader whose input signature holds SV_Barycentrics.
[[nodiscard]] static bool reads_barycentrics(const std::vector<u8>& bytecode) {
    const auto ps = graphics::parse_dxbc(bytecode.data(), bytecode.size());
    return std::any_of(ps.input_signature.begin(), ps.input_signature.end(),
                       [](const graphics::DxbcSignatureElement& e) {
                           return e.system_value_type == D3D_NAME_BARYCENTRICS;
                       });
}

/// u# table size for every stage. SM 5.0 declares at most 64 UAV registers
/// (D3D11_1_UAV_SLOT_COUNT); the table only has to cover what a shader can
/// name, and an unused slot costs eight bytes.
constexpr u32 UAV_TABLE_SLOTS = D3D12_UAV_SLOT_COUNT;

/// GetCachedBlob's blob: magic, version, reserved, the description digest.
constexpr char kCachedPsoMagic[8] = {'V', 'T', 'R', 'M', 'P', 'S', 'O', '1'};
constexpr u32 kCachedPsoVersion = 1;
constexpr usize kCachedPsoBytes = 24;

[[nodiscard]] bool fourcc_present(const std::vector<u8>& bytes,
                                  const char* fourcc) {
    // A DXBC container: "DXBC", 16-byte hash, version, size, part count, then
    // the part offsets (Learn, "Shader Model 5 assembly"; the layout
    // parse_dxbc reads). Each part begins with its FourCC.
    if (bytes.size() < 32 || std::memcmp(bytes.data(), "DXBC", 4) != 0) {
        return false;
    }
    u32 parts = 0;
    std::memcpy(&parts, bytes.data() + 28, 4);
    for (u32 index = 0; index < parts && 32 + index * 4 + 4 <= bytes.size();
         ++index) {
        u32 offset = 0;
        std::memcpy(&offset, bytes.data() + 32 + index * 4, 4);
        if (offset + 4 <= bytes.size() &&
            std::memcmp(bytes.data() + offset, fourcc, 4) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] u32 register_high_water(
    const std::vector<graphics::DxbcResourceBinding>& bindings) {
    u32 high = 0;
    for (const auto& binding : bindings) {
        const u32 count = binding.bind_count == 0 ? 1u : binding.bind_count;
        high = std::max(high, binding.bind_point + count);
    }
    return high;
}

[[nodiscard]] bool semantic_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] graphics::BackendBlendFactor blend_factor(u32 v) {
    using F = graphics::BackendBlendFactor;
    switch (v) {
    case D3D12_BLEND_ZERO: return F::Zero;
    case D3D12_BLEND_ONE: return F::One;
    case D3D12_BLEND_SRC_COLOR: return F::SourceColor;
    case D3D12_BLEND_INV_SRC_COLOR: return F::OneMinusSourceColor;
    case D3D12_BLEND_SRC_ALPHA: return F::SourceAlpha;
    case D3D12_BLEND_INV_SRC_ALPHA: return F::OneMinusSourceAlpha;
    case D3D12_BLEND_DEST_ALPHA: return F::DestinationAlpha;
    case D3D12_BLEND_INV_DEST_ALPHA: return F::OneMinusDestinationAlpha;
    case D3D12_BLEND_DEST_COLOR: return F::DestinationColor;
    case D3D12_BLEND_INV_DEST_COLOR: return F::OneMinusDestinationColor;
    case D3D12_BLEND_SRC_ALPHA_SAT: return F::SourceAlphaSaturated;
    case D3D12_BLEND_BLEND_FACTOR: return F::BlendColor;
    case D3D12_BLEND_INV_BLEND_FACTOR: return F::OneMinusBlendColor;
    case D3D12_BLEND_SRC1_COLOR: return F::Source1Color;
    case D3D12_BLEND_INV_SRC1_COLOR: return F::OneMinusSource1Color;
    case D3D12_BLEND_SRC1_ALPHA: return F::Source1Alpha;
    case D3D12_BLEND_INV_SRC1_ALPHA: return F::OneMinusSource1Alpha;
    default: return F::One;
    }
}

[[nodiscard]] bool blend_factor_valid(u32 v) {
    return (v >= D3D12_BLEND_ZERO && v <= D3D12_BLEND_SRC_ALPHA_SAT) ||
           (v >= D3D12_BLEND_BLEND_FACTOR && v <= D3D12_BLEND_INV_SRC1_ALPHA);
}

[[nodiscard]] graphics::BackendBlendOperation blend_op(u32 v) {
    using O = graphics::BackendBlendOperation;
    switch (v) {
    case D3D12_BLEND_OP_SUBTRACT: return O::Subtract;
    case D3D12_BLEND_OP_REV_SUBTRACT: return O::ReverseSubtract;
    case D3D12_BLEND_OP_MIN: return O::Min;
    case D3D12_BLEND_OP_MAX: return O::Max;
    default: return O::Add;
    }
}

[[nodiscard]] graphics::BackendPrimitiveClass primitive_class(u32 type) {
    switch (type) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        return graphics::BackendPrimitiveClass::Point;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        return graphics::BackendPrimitiveClass::Line;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
        return graphics::BackendPrimitiveClass::Triangle;
    default:
        return graphics::BackendPrimitiveClass::Unspecified;
    }
}

struct Bytecode {
    GuestAddr pointer{0};
    u64 size{0};
    [[nodiscard]] bool present() const { return pointer != 0 && size != 0; }
};

}  // namespace

// =============================================================================
// Root signatures
// =============================================================================

#if VITREO_D3D12_ROOT_SIGNATURE_PARSER
namespace {

/// The binding form of a parsed root signature: every range's offset from its
/// table start resolved (D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND follows the
/// previous range), and static samplers as sampler states.
[[nodiscard]] std::shared_ptr<const RootLayout> make_root_layout(
    const RootSignatureDesc& desc) {
    auto layout = std::make_shared<RootLayout>();
    layout->flags = desc.flags;
    for (const auto& parameter : desc.parameters) {
        BindingParameter out{};
        out.type = parameter.parameter_type;
        out.visibility = parameter.shader_visibility;
        out.shader_register = parameter.shader_register;
        out.space = parameter.register_space;
        out.num_32bit_values = parameter.num_32bit_values;
        out.flags = parameter.flags;
        u32 cursor = 0;
        for (const auto& range : parameter.ranges) {
            BindingRange r{};
            r.type = range.range_type;
            r.count = range.num_descriptors;
            r.base_register = range.base_shader_register;
            r.space = range.register_space;
            r.flags = range.flags;
            r.table_offset =
                range.offset_in_descriptors_from_table_start ==
                        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? cursor
                    : range.offset_in_descriptors_from_table_start;
            cursor = r.count == 0xFFFFFFFFu ? r.table_offset
                                             : r.table_offset + r.count;
            out.ranges.push_back(r);
        }
        layout->parameters.push_back(std::move(out));
    }
    for (const auto& sampler : desc.static_samplers) {
        graphics::compile_profile::note(graphics::compile_profile::StaticSamplerCreated);
        if (d3d_minmax_reduction_filter(sampler.filter)) {
            graphics::compile_profile::note(graphics::compile_profile::StaticSamplerMinMax);
            // Before any pipeline can use this root signature: shaders now
            // compile with the reduction (graphics/shaders/minmax_reduction.hpp).
            if (graphics::enable_minmax_reduction()) {
                d3d12_diag("first MINIMUM_/MAXIMUM_ static sampler (filter 0x%x): "
                           "table reductions compile in from now on", sampler.filter);
            }
        }
        BindingSampler out{};
        out.state.filter = sampler.filter;
        out.state.address_u = sampler.address_u;
        out.state.address_v = sampler.address_v;
        out.state.address_w = sampler.address_w;
        out.state.mip_lod_bias = sampler.mip_lod_bias;
        out.state.max_anisotropy = sampler.max_anisotropy;
        out.state.comparison_func = sampler.comparison_func;
        out.state.min_lod = sampler.min_lod;
        out.state.max_lod = sampler.max_lod;
        out.state.flags = sampler.flags;
        // D3D12_STATIC_BORDER_COLOR: transparent black, opaque black, opaque
        // white, and (SDK 26100) their UINT forms.
        const float one = 1.0f;
        u32 one_bits = 0;
        std::memcpy(&one_bits, &one, 4);
        switch (sampler.border_color) {
        case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK:
            out.state.border = {0, 0, 0, one_bits};
            break;
        case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE:
            out.state.border = {one_bits, one_bits, one_bits, one_bits};
            break;
        case 3u:  // D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT
            out.state.border = {0u, 0u, 0u, 1u};
            out.state.flags |= 0x1u;
            break;
        case 4u:  // D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT
            out.state.border = {1u, 1u, 1u, 1u};
            out.state.flags |= 0x1u;
            break;
        default:
            out.state.border = {0, 0, 0, 0};
            break;
        }
        out.shader_register = sampler.shader_register;
        out.space = sampler.register_space;
        out.visibility = sampler.shader_visibility;
        layout->static_samplers.push_back(out);
    }
    return layout;
}

}  // namespace
#endif

std::shared_ptr<const RootLayout> root_layout_from_part(const u8* part, usize size, u32& error) {
#if VITREO_D3D12_ROOT_SIGNATURE_PARSER
    RootSignatureDesc desc{};
    error = parse_root_signature_part_for_device(part, size, desc);
    return error == S_OK ? make_root_layout(desc) : nullptr;
#else
    (void)part;
    (void)size;
    error = E_NOTIMPL;
    return nullptr;
#endif
}

void Frontend::create_root_signature(Marshaler& m, Device& device) {
    // CreateRootSignature(UINT nodeMask, const void *pBlobWithRootSignature,
    //     SIZE_T blobLengthInBytes, REFIID riid, void **ppvRootSignature).
    const u32 node_mask = m.arg32(1);
    const GuestAddr blob = m.arg_ptr(2);
    const u64 size = m.arg_ptr(3);
    const GuestAddr out = m.arg_ptr(5);
    if (out != 0) (void)write_pointer(out, 0);
    if (blob == 0 || size == 0 || out == 0 || node_mask > 1 ||
        size > 0x10000000ull) {
        m.set_return(refuse(E_INVALIDARG,
                            "CreateRootSignature: blob 0x%llx, size %llu, ppv 0x%llx, "
                            "nodeMask %u refused",
                            static_cast<unsigned long long>(blob),
                            static_cast<unsigned long long>(size),
                            static_cast<unsigned long long>(out), node_mask));
        return;
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    if (m_memory.read_bytes(blob, bytes.data(), bytes.size()).is_err()) {
        m.set_return(refuse(E_INVALIDARG, "CreateRootSignature: the blob is unreadable"));
        return;
    }
#if VITREO_D3D12_ROOT_SIGNATURE_PARSER
    RootSignatureDesc desc{};
    u32 result =
        parse_root_signature_blob_for_device(bytes.data(), bytes.size(), desc);
    if (result == S_OK) result = validate_root_signature_for_device(desc);
    if (result != S_OK) {
        d3d12_diag("CreateRootSignature: blob refused 0x%08x", result);
        m.set_return(result);
        return;
    }
    // Measured (a05 pso "CreateRootSignature twice from one blob"): the
    // same blob gives the same object while one is alive.
    const u64 hash = fnv1a(bytes.data(), bytes.size());
    for (auto& [address, object] : m_objects) {
        if (object->kind != Kind::RootSignature) continue;
        auto& existing = static_cast<RootSignature&>(*object);
        if (existing.blob_hash == hash && existing.node_mask == node_mask &&
            existing.blob == bytes) {
            GuidBytes iid{};
            const GuestAddr answer = read_guid(m.arg_ptr(4), iid)
                                         ? query_interface(existing, iid)
                                         : 0;
            (void)write_pointer(out, answer);
            m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
            return;
        }
    }
    auto root = std::make_unique<RootSignature>();
    root->device = device.address;
    root->node_mask = node_mask;
    root->blob = std::move(bytes);
    root->blob_hash = hash;
    root->layout = make_root_layout(desc);
    m.set_return(return_new_object(std::move(root), m.arg_ptr(4), out));
#else
    (void)device;
    unimplemented("ID3D12Device::CreateRootSignature",
                  "root signature parser not linked");
    m.set_return(E_NOTIMPL);
#endif
}

void Frontend::root_signature_method(Marshaler& m, RootSignature& root,
                                     usize slot) {
    (void)root;
    // ID3D12RootSignature adds nothing to ID3D12DeviceChild.
    unimplemented("ID3D12RootSignature", "slot");
    d3d12_diag("ID3D12RootSignature slot %zu", slot);
    m.set_return(E_NOTIMPL);
}

// =============================================================================
// Shaders
// =============================================================================

u32 Frontend::read_shader_bytes(GuestAddr bytecode_struct,
                                std::vector<u8>& out) {
    // D3D12_SHADER_BYTECODE { const void *pShaderBytecode; SIZE_T
    // BytecodeLength; }.
    out.clear();
    if (bytecode_struct == 0) return S_OK;
    const auto& l = layout::D3D12_SHADER_BYTECODE[m_is_64bit];
    GuestAddr pointer = 0;
    GuestAddr length = 0;
    if (!read_pointer(bytecode_struct + l.pShaderBytecode, pointer) ||
        !read_pointer(bytecode_struct + l.BytecodeLength, length)) {
        return refuse(E_INVALIDARG, "D3D12_SHADER_BYTECODE at 0x%llx is unreadable",
                      static_cast<unsigned long long>(bytecode_struct));
    }
    if (pointer == 0 || length == 0) return S_OK;  // no shader at this stage
    if (length > 0x4000000ull) {
        return refuse(E_INVALIDARG, "shader BytecodeLength %llu exceeds 64 MiB",
                      static_cast<unsigned long long>(length));
    }
    out.resize(static_cast<usize>(length));
    if (m_memory.read_bytes(pointer, out.data(), out.size()).is_err()) {
        out.clear();
        return refuse(E_INVALIDARG, "shader bytecode 0x%llx (%llu bytes) is unreadable",
                      static_cast<unsigned long long>(pointer),
                      static_cast<unsigned long long>(length));
    }
    if (graphics::dxil_container_present(out.data(), out.size())) {
        return lower_dxil(out);
    }
    return S_OK;
}

namespace {

/// Lowered DXIL programs by container digest: a pipeline reads each stage's
/// bytecode more than once (lower_shader_stages, stream output, the root
/// signature lookup), and the digest of a signed container names its
/// contents.
struct DxilMemo {
    std::mutex mutex;
    std::map<std::pair<std::array<u8, 16>, usize>, std::vector<u8>> lowered;
};

DxilMemo& dxil_memo() {
    static DxilMemo memo;
    return memo;
}

}  // namespace

u32 Frontend::lower_dxil(std::vector<u8>& bytes) {
    // Measured (d3d12_sm6_exec_probe "container", WARP12 and the RTX 4080):
    // a DXIL container whose digest is zero (unsigned), wrong, or that is
    // cut short is E_INVALIDARG -- not the E_FAIL of a DXBC hash mismatch.
    if (!graphics::dxbc_container_verified(bytes.data(), bytes.size())) {
        d3d12_diag("DXIL container is unsigned, truncated or its digest does not match");
        bytes.clear();
        return E_INVALIDARG;
    }
    std::pair<std::array<u8, 16>, usize> key;
    std::memcpy(key.first.data(), bytes.data() + 4, 16);
    key.second = bytes.size();
    DxilMemo& memo = dxil_memo();
    {
        std::lock_guard lock(memo.mutex);
        if (auto it = memo.lowered.find(key); it != memo.lowered.end()) {
            bytes = it->second;
            return S_OK;
        }
    }
    graphics::DxilToDxbcResult lowered = graphics::dxil_to_dxbc(bytes.data(), bytes.size());
    if (!lowered.ok) {
        if (lowered.unimplemented) {
            unimplemented("CreatePipelineState (DXIL)", lowered.failure.c_str());
        } else {
            d3d12_diag("DXIL: %s", lowered.failure.c_str());
        }
        bytes.clear();
        return E_INVALIDARG;
    }
    // HLSL SM 6.6 WaveSize / SM 6.8 WaveSize ranges: a required wave size
    // outside [WaveLaneCountMin, WaveLaneCountMax] fails validation.
    if (lowered.wave_size_min != 0) {
        const u32 lanes = caps().simd_width;
        if (lanes == 0 || lanes < lowered.wave_size_min || lanes > lowered.wave_size_max) {
            d3d12_diag("DXIL: WaveSize %u..%u excludes the device's %u lanes", lowered.wave_size_min,
                       lowered.wave_size_max, lanes);
            bytes.clear();
            return E_INVALIDARG;
        }
    }
    bytes = std::move(lowered.container);
    std::lock_guard lock(memo.mutex);
    memo.lowered.emplace(key, bytes);
    return S_OK;
}

namespace {

/// The D3D name of a stage, for refusal reasons.
[[nodiscard]] const char* stage_label(graphics::ShaderStage stage) {
    switch (stage) {
    case graphics::ShaderStage::Vertex: return "VS";
    case graphics::ShaderStage::Fragment: return "PS";
    case graphics::ShaderStage::Compute: return "CS";
    case graphics::ShaderStage::Geometry: return "GS";
    case graphics::ShaderStage::Hull: return "HS";
    case graphics::ShaderStage::Domain: return "DS";
    case graphics::ShaderStage::Mesh: return "MS";
    case graphics::ShaderStage::Amplification: return "AS";
    }
    return "?";
}

/// Descriptor-table set of a DXBC register class (set 0 = b#, 1 = t#,
/// 2 = s#, 3 = u#).
[[nodiscard]] u32 set_of_class(graphics::DxbcRegisterClass cls) {
    switch (cls) {
    case graphics::DxbcRegisterClass::ConstantBuffer: return 0;
    case graphics::DxbcRegisterClass::ShaderResource: return 1;
    case graphics::DxbcRegisterClass::Sampler: return 2;
    default: return 3;
    }
}

/// D3D12_DESCRIPTOR_RANGE_TYPE of set @p set.
[[nodiscard]] u32 range_type_of_set(u32 set) {
    switch (set) {
    case 0: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case 1: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case 2: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    default: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
}

/// A bounded descriptor array longer than this reads the heap through a
/// window (CompiledShader::heap_window) rather than a per-draw copy.
constexpr u32 kHeapWindowMinArray = 128;

/// The descriptor-table slots of the float typed UAVs the translation
/// declared as uint images for their atomics
/// (DxbcToSpirvResult::typed_uav_uint_view_registers): bind_view puts an
/// R32_UINT view of the R32_FLOAT resource there. A range the shader reads
/// from the heap in place has no per-draw view to choose, so it is refused.
bool note_uint_view_uavs(CompiledShader& out,
                         const graphics::DxbcToSpirvResult& spirv,
                         const graphics::DxbcTranslationOptions& options,
                         const char* api) {
    out.uint_view_uavs.clear();
    for (const u32 reg : spirv.typed_uav_uint_view_registers) {
        for (const auto& binding : spirv.bindings) {
            if (binding.declaration.register_class !=
                    graphics::DxbcRegisterClass::UnorderedAccess ||
                binding.declaration.range_id != reg) {
                continue;
            }
            if (options.heap_window.enabled &&
                binding.descriptor_set == options.heap_window.descriptor_set) {
                unimplemented(api, "an atomic on a float typed UAV in a "
                                   "descriptor range read from the heap in "
                                   "place");
                return false;
            }
            out.uint_view_uavs.push_back(
                {binding.descriptor_set, binding.binding,
                 std::max(1u, binding.descriptor_count)});
        }
    }
    return true;
}

/// Descriptors an unbounded SM 5.1 array is given when its root-signature
/// range is unbounded too: the table the pipeline is compiled against is
/// fixed, and one past this reads a null descriptor.
constexpr u32 UNBOUNDED_ARRAY_SLOTS = 1024;

/// The descriptor-table translation of configure_descriptor_bindings:
/// shader model 5.1, and a shader model 5.0 shader that declares a sampler.
/// Its descriptor samplers need the sampler-state records, whose custom-
/// border flag routes a sample through the border twin (Metal has three
/// border colours; D3D12_SAMPLER_DESC::BorderColor is any RGBA, measured
/// d3d12_a03_descriptors_probe [samplers]). The flat model has push-block
/// biases only and no twins.
[[nodiscard]] bool table_translation(const graphics::DxbcContainer& container) {
    if (container.is_sm51_plus()) return true;
    for (const auto& declaration : container.binding_declarations) {
        if (declaration.register_class == graphics::DxbcRegisterClass::Sampler) {
            return true;
        }
    }
    return false;
}

/// A lowered geometry, hull, domain or capture stage binds flat only while
/// every resource fits that window. Shader model 5.1, another register space,
/// a texture, a sampler, or a register past the flat windows uses the same
/// descriptor-table translation as compile_shader.
[[nodiscard]] bool needs_descriptor_tables(
    const graphics::DxbcContainer& container) {
    if (container.is_sm51_plus()) return true;
    for (const auto& declaration : container.binding_declarations) {
        if (declaration.space != 0 || declaration.unbounded()) return true;
        if (declaration.register_class == graphics::DxbcRegisterClass::Sampler ||
            declaration.opcode == 88u || declaration.opcode == 156u) {
            return true;
        }
        if (declaration.register_class !=
            graphics::DxbcRegisterClass::ConstantBuffer) {
            return true;
        }
        if (declaration.upper >= graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS) {
            return true;
        }
    }
    return false;
}

/// The descriptor-table translation compile_shader uses: register remap,
/// sampler-state records, view sizes, constant-buffer limits and null flags.
/// A lowered stage calls this instead of building a second copy.
void configure_tiled_status(const graphics::DxbcContainer& container,
                            CompiledShader& out,
                            graphics::DxbcTranslationOptions& options,
                            u32 cbv_slot);

u32 configure_descriptor_bindings(const graphics::DxbcContainer& container,
                                  const RootLayout* root, CompiledShader& out,
                                  graphics::DxbcTranslationOptions& options,
                                  const char* api) {
    std::array<u32, 4> next_slot{};
    bool image_srv = false;
    bool image_uav = false;
    for (const auto& declaration : out.declarations) {
        if (declaration.opcode == 88u) image_srv = true;
        if (declaration.opcode == 156u) image_uav = true;
    }
    if (table_translation(container)) {
        // Shader model 5.1 (register spaces, descriptor arrays). Each
        // declared range takes the next slots of its class's table, bounded
        // ranges first. Windows allows several unbounded ranges of one class;
        // they differ in register space or base register, and each is its
        // own descriptor array (dxbc_to_spirv.hpp DxbcRegisterRemap).
        //
        // A runtime array occupies every slot from its binding through the
        // end of the table — that is how the Metal argument buffer sizes
        // OpTypeRuntimeArray — so a second one in the same set would be
        // laid over the first. Only the last unbounded non-sampler range of
        // a class is a runtime array. Every earlier unbounded range is a
        // sized array of the descriptors this table gives it. Samplers stay
        // sized even when the declaration is unbounded: the border twins
        // follow them in the same argument buffer.
        //
        // slot_of maps (space, register) to one slot and returns the first
        // run that covers the register. Two unbounded windows that overlap
        // in one space cannot both be bound, so that shader fails here
        // instead of aliasing the later range onto the earlier run.
        out.table_mode = true;
        std::vector<const graphics::DxbcBindingDeclaration*> order;
        for (const auto& d : out.declarations) order.push_back(&d);
        std::stable_sort(order.begin(), order.end(),
                         [](const auto* a, const auto* b) {
                             return !a->unbounded() && b->unbounded();
                         });
        // Resource binding tiers 2 and 3 (Learn, "Hardware Tiers": SRVs,
        // and at tier 3 CBVs and UAVs, in all descriptor tables per stage =
        // full heap): every unbounded t#, u# and b# array, and every one
        // longer than kHeapWindowMinArray, reads the shader-visible heap
        // itself (heap_window), so its draws copy nothing and no root
        // signature truncates it, whatever other ranges of the space it
        // has. The SM 6.6 heap-indexed arrays (space kDxbcResourceHeapSpace)
        // are one segment from heap slot 0.
        const auto is_window = [&](const graphics::DxbcBindingDeclaration& d) {
            if (d.register_class == graphics::DxbcRegisterClass::Sampler) {
                return false;
            }
            if (d.space == graphics::kDxbcResourceHeapSpace) return true;
            // The raytracing megakernel's reserved arrays (local root
            // views, companions) are per-dispatch copies.
            if (d.space == graphics::kDxilRtSpace) return false;
            return d.is_array() &&
                   (d.unbounded() || d.count() > kHeapWindowMinArray);
        };
        std::vector<const graphics::DxbcBindingDeclaration*> windows;
        for (const auto* d : order) {
            if (is_window(*d)) windows.push_back(d);
        }
        u32 window_words = 1;  // word 0: the heap's descriptor count
        for (u32 k = 0; k < windows.size(); ++k) {
            const auto* d = windows[k];
            graphics::DxbcRegisterRemap entry{};
            entry.register_class = d->register_class;
            entry.space = d->space;
            entry.first_register = d->lower;
            entry.register_count = graphics::kDescriptorHeapWindowLength;
            entry.descriptor_set = graphics::SPIRV_HEAP_WINDOW_DESCRIPTOR_SET;
            entry.first_slot = k;
            options.register_remap.push_back(entry);
            graphics::DxbcTranslationOptions::DescriptorHeapWindow::Array array{};
            array.register_class = d->register_class;
            array.space = d->space;
            array.first_register = d->lower;
            if (d->space == graphics::kDxbcResourceHeapSpace) {
                array.segments.push_back(
                    {0u, graphics::kDxbcUnboundedRange, window_words});
                out.heap_window.segments.push_back(
                    {CompiledShader::HeapWindow::kHeapStart, 0u, window_words});
                ++window_words;
            } else if (root != nullptr) {
                // Measured (d3d12_a22_heapwin_probe multi, WARP and an RTX
                // 4080 SUPER alike): element e is descriptor e of the table
                // range holding the array's base register, read on past
                // that range's end into the heap -- not the descriptor of
                // another range that names register base + e. So each
                // range holding the base is one segment from element 0 to
                // the heap's end; the replay keeps the one its stage sees.
                const u32 range_type = range_type_of_set(set_of_class(d->register_class));
                for (u32 index = 0; index < root->parameters.size(); ++index) {
                    const auto& p = root->parameters[index];
                    if (p.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                        continue;
                    }
                    for (const auto& r : p.ranges) {
                        if (r.type != range_type || r.space != d->space ||
                            d->lower < r.base_register ||
                            (r.count != 0xFFFFFFFFu &&
                             d->lower - r.base_register >= r.count)) {
                            continue;
                        }
                        const u64 in_table =
                            u64{r.table_offset} + (d->lower - r.base_register);
                        if (in_table > 0xFFFFFFFFull) continue;  // past any heap
                        array.segments.push_back(
                            {0u, graphics::kDxbcUnboundedRange, window_words});
                        out.heap_window.segments.push_back(
                            {index, static_cast<u32>(in_table), window_words});
                        ++window_words;
                    }
                }
            }
            options.heap_window.arrays.push_back(std::move(array));
            out.heap_window.present = true;
            if (d->register_class == graphics::DxbcRegisterClass::UnorderedAccess) {
                out.heap_window.writes = true;
            }
        }
        std::array<u32, 4> unbounded_left{};
        for (const auto* d : order) {
            if (!is_window(*d) && d->unbounded() &&
                d->register_class != graphics::DxbcRegisterClass::Sampler) {
                ++unbounded_left[set_of_class(d->register_class)];
            }
        }
        struct UnboundedWindow {
            u32 space;
            u32 first;
            u32 count;
        };
        std::array<std::vector<UnboundedWindow>, 4> unbounded_windows;
        for (const auto* d : order) {
            const u32 set = set_of_class(d->register_class);
            if (is_window(*d)) continue;
            u32 count = d->count();
            if (d->unbounded()) {
                // Learn, "Hardware Tiers": tier 2 shaders reach 2048
                // samplers per stage, the whole shader-visible sampler heap
                // (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE).
                count = d->register_class ==
                                graphics::DxbcRegisterClass::Sampler
                            ? D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE
                            : UNBOUNDED_ARRAY_SLOTS;
                // The root signature's range, when it bounds the array.
                if (root != nullptr) {
                    for (const auto& p : root->parameters) {
                        if (p.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                            continue;
                        }
                        for (const auto& r : p.ranges) {
                            if (r.type == range_type_of_set(set) &&
                                r.space == d->space &&
                                r.base_register <= d->lower &&
                                r.count != 0xFFFFFFFFu &&
                                d->lower - r.base_register < r.count) {
                                count = r.count - (d->lower - r.base_register);
                            }
                        }
                    }
                }
                const u64 begin = d->lower;
                const u64 end = begin + count;
                for (const auto& window : unbounded_windows[set]) {
                    if (window.space != d->space) continue;
                    // SamplerDescriptorHeap[] ranges (one per sampler type,
                    // distinct lower bounds in the heap space, operands by
                    // range ID) each read the whole sampler heap from slot
                    // 0 (d3d12_replay_bindings.cpp), so they overlap by
                    // design.
                    if (d->space == graphics::kDxbcSamplerHeapSpace) continue;
                    const u64 w0 = window.first;
                    const u64 w1 = w0 + window.count;
                    if (w0 < end && begin < w1) {
                        unimplemented(api, "two unbounded arrays of one register "
                                           "class in one shader");
                        return E_INVALIDARG;
                    }
                }
                unbounded_windows[set].push_back(
                    UnboundedWindow{d->space, d->lower, count});
            }
            bool runtime = false;
            if (d->unbounded() &&
                d->register_class != graphics::DxbcRegisterClass::Sampler) {
                --unbounded_left[set];
                // The last unbounded range of the class sits at the end of
                // the table, which is the one place a runtime array fits.
                // Never a b# array: the CBV table also holds the side
                // buffers after the runs, which a runtime array would
                // cover (Metal: "id set location ... minimum is ...").
                runtime = unbounded_left[set] == 0u &&
                          d->register_class !=
                              graphics::DxbcRegisterClass::ConstantBuffer;
            }
            graphics::DxbcRegisterRemap entry{};
            entry.register_class = d->register_class;
            entry.space = d->space;
            entry.first_register = d->lower;
            entry.register_count =
                runtime ? graphics::kDxbcUnboundedRange : count;
            entry.descriptor_set = set;
            entry.first_slot = next_slot[set];
            options.register_remap.push_back(entry);
            out.runs[set].push_back(
                CompiledShader::Run{d->space, d->lower, count, next_slot[set]});
            next_slot[set] += count;
        }
        if (next_slot[2] != 0) {
            // MipLODBias, POINT-mip rounding, border colours and min/max
            // reduction of descriptor samplers: one record per real sampler
            // slot, in the CBV table's last slot. Twins bind at
            // next_slot[2] + binding, so the argument buffer is twice as
            // long (applied to sampler_slots after translation).
            options.sampler_state_buffer.enabled = true;
            options.sampler_state_buffer.descriptor_set = 0;
            options.sampler_state_buffer.binding = next_slot[0];
            options.border_twin_binding_base = next_slot[2];
            out.sampler_state_slot = next_slot[0];
            ++next_slot[0];
        }
        if (out.heap_window.present) {
            // The window's params words (limit, segment bases) and the
            // heap's side buffer: two more CBV-table slots (heap_window).
            options.heap_window.enabled = true;
            options.heap_window.descriptor_set =
                graphics::SPIRV_HEAP_WINDOW_DESCRIPTOR_SET;
            options.heap_window.params_set = 0;
            options.heap_window.params_binding = next_slot[0];
            out.heap_window.params_slot = next_slot[0];
            ++next_slot[0];
            options.heap_window.side_set = 0;
            options.heap_window.side_binding = next_slot[0];
            out.heap_window.side_slot = next_slot[0];
            ++next_slot[0];
            if (graphics::dxbc_uses_tiled_status(container)) {
                // Tiled status of a window buffer element: the heap's
                // buffer residency records, by record id.
                options.heap_window.residency_set = 0;
                options.heap_window.residency_binding = next_slot[0];
                out.heap_window.residency_slot = next_slot[0];
                ++next_slot[0];
            }
        }
        // Learn (ByteAddressBuffer / StructuredBuffer): a read past the view
        // returns 0, a write is dropped, and bufinfo reports the view.
        // dxbc_parser.hpp: dcl_uav_raw 157, dcl_uav_structured 158,
        // dcl_resource_raw 161, dcl_resource_structured 162. The push-block
        // guard is refused for a descriptor-table translation, so the sizes
        // are one uint per table slot in the CBV table.
        bool raw_or_structured_srv = false;
        bool raw_or_structured_uav = false;
        for (const auto& declaration : out.declarations) {
            if (declaration.opcode == 161u || declaration.opcode == 162u) {
                raw_or_structured_srv = true;
            }
            if (declaration.opcode == 157u || declaration.opcode == 158u) {
                raw_or_structured_uav = true;
            }
        }
        if (raw_or_structured_srv || raw_or_structured_uav) {
            options.view_size_buffer.enabled = true;
            if (raw_or_structured_srv) {
                options.view_size_buffer.srv = true;
                options.view_size_buffer.srv_set = 0;
                options.view_size_buffer.srv_binding = next_slot[0];
                out.srv_view_size_slot = next_slot[0];
                ++next_slot[0];
            }
            if (raw_or_structured_uav) {
                options.view_size_buffer.uav = true;
                options.view_size_buffer.uav_set = 0;
                options.view_size_buffer.uav_binding = next_slot[0];
                out.uav_view_size_slot = next_slot[0];
                ++next_slot[0];
            }
        }
        // D3D11.3 7.5: a read past the bound constant buffer returns 0.
        // dxbc_parser.hpp dcl_constantbuffer is opcode 89.
        bool declares_cbv = false;
        for (const auto& declaration : out.declarations) {
            if (declaration.opcode == 89u &&
                declaration.register_class ==
                    graphics::DxbcRegisterClass::ConstantBuffer) {
                declares_cbv = true;
            }
        }
        if (declares_cbv) {
            options.constant_buffer_limits.enabled = true;
            options.constant_buffer_limits.descriptor_set = 0;
            options.constant_buffer_limits.binding = next_slot[0];
            out.cbv_limit_slot = next_slot[0];
            ++next_slot[0];
        }
        if (image_srv || image_uav) {
            options.null_descriptors.enabled = true;
            if (image_srv) {
                options.null_descriptors.srv = true;
                options.null_descriptors.srv_set = 0;
                options.null_descriptors.srv_binding = next_slot[0];
                out.srv_null_slot = next_slot[0];
                ++next_slot[0];
            }
            if (image_uav) {
                options.null_descriptors.uav = true;
                options.null_descriptors.uav_set = 0;
                options.null_descriptors.uav_binding = next_slot[0];
                out.uav_null_slot = next_slot[0];
                ++next_slot[0];
            }
        }
        // Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): FirstElement is any
        // element. The push-block bias is refused for a descriptor table,
        // so the packed entry is one uint per table slot in the CBV table,
        // after the sampler-state, view-size, constant-buffer-limit and
        // null-flag slots. dcl_resource / dcl_uav_typed dimension 1 is a
        // buffer (dxbc_parser.hpp).
        bool typed_srv = false;
        bool typed_uav = false;
        for (const auto& declaration : out.declarations) {
            if (declaration.opcode == 88u && declaration.dimension == 1u) {
                typed_srv = true;
            }
            if (declaration.opcode == 156u && declaration.dimension == 1u) {
                typed_uav = true;
            }
        }
        if (typed_srv || typed_uav) {
            options.typed_buffer_bias_buffer.enabled = true;
            if (typed_srv) {
                options.typed_buffer_bias_buffer.srv = true;
                options.typed_buffer_bias_buffer.srv_set = 0;
                options.typed_buffer_bias_buffer.srv_binding = next_slot[0];
                out.srv_typed_bias_slot = next_slot[0];
                ++next_slot[0];
            }
            if (typed_uav) {
                options.typed_buffer_bias_buffer.uav = true;
                options.typed_buffer_bias_buffer.uav_set = 0;
                options.typed_buffer_bias_buffer.uav_binding = next_slot[0];
                out.uav_typed_bias_slot = next_slot[0];
                ++next_slot[0];
            }
        }
    } else {
        // Raw and structured views carry their size in the push block's size
        // table (member 2), which the replay fills from each bound
        // descriptor: bufinfo reads it, and a load or store past the view
        // returns 0 / is dropped (Learn, ByteAddressBuffer /
        // StructuredBuffer).
        options.guard_buffer_bounds = true;
        // The same 7.5 rule, from the push block's per-register limits.
        // Shader model 5.0 names at most 15 cb# slots.
        options.guard_constant_buffers = true;
        // Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): FirstElement is any
        // element. A non-table shader model 5.0 translation keeps the
        // push-block bias. A descriptor table uses typed_buffer_bias_buffer
        // instead, and refuses this option.
        options.typed_buffer_bias = true;
        // Shader model 5.0 names at most b0..b14, so bindings 15 and 16 are
        // free for the null flags. The argument buffer is widened to cover
        // them after translation.
        if (image_srv || image_uav) {
            options.null_descriptors.enabled = true;
            if (image_srv) {
                options.null_descriptors.srv = true;
                options.null_descriptors.srv_binding = 15;
                out.srv_null_slot = 15;
            }
            if (image_uav) {
                options.null_descriptors.uav = true;
                options.null_descriptors.uav_binding = 16;
                out.uav_null_slot = 16;
            }
        }
        // A typed buffer view read as raw memory (graphics::kTypedBufferRaw)
        // takes its address and format from the per-slot side buffer, as a
        // descriptor table does; the push block keeps the bias. Bindings
        // 17 and 18 follow the null flags.
        bool typed_srv = false;
        bool typed_uav = false;
        for (const auto& declaration : out.declarations) {
            typed_srv |= declaration.opcode == 88u && declaration.dimension == 1u;
            typed_uav |= declaration.opcode == 156u && declaration.dimension == 1u;
        }
        if (typed_srv || typed_uav) {
            options.typed_buffer_bias_buffer.enabled = true;
            if (typed_srv) {
                options.typed_buffer_bias_buffer.srv = true;
                options.typed_buffer_bias_buffer.srv_binding = 17;
                out.srv_typed_bias_slot = 17;
            }
            if (typed_uav) {
                options.typed_buffer_bias_buffer.uav = true;
                options.typed_buffer_bias_buffer.uav_binding = 18;
                out.uav_typed_bias_slot = 18;
            }
        }
    }
    // Class-table widths before the CBV side buffers (sampler state, view
    // sizes, limits, null flags). finish_register_counts extends b# to
    // cover those slots.
    if (out.table_mode) {
        out.registers.cbv_slots = 0;
        out.registers.srv_slots = 0;
        out.registers.sampler_slots = 0;
        out.registers.uav_slots = 0;
        for (u32 set = 0; set < 4; ++set) {
            u32 width = 0;
            for (const auto& run : out.runs[set]) {
                width = std::max(width, run.first_slot + run.count);
            }
            if (set == 0) out.registers.cbv_slots = width;
            if (set == 1) out.registers.srv_slots = width;
            if (set == 2) out.registers.sampler_slots = width;
            if (set == 3) out.registers.uav_slots = width;
        }
    }
    configure_tiled_status(container, out, options,
                           out.table_mode ? next_slot[0] : 17u);
    return S_OK;
}

void configure_tiled_status(const graphics::DxbcContainer& container,
                            CompiledShader& out,
                            graphics::DxbcTranslationOptions& options,
                            u32 cbv_slot) {
    // Tiled resources tier 2 (DxbcTranslationOptions::tiled_status), only
    // for a shader that uses a status instruction. The SRV table grows
    // [t# | t# companions | u# companions], the sampler table gets a block
    // of status twins after the real samplers (and, for a descriptor table,
    // their border twins), and the CBV table two residency-record slots
    // (b0..b14, then the 5.0 null flags at 15 and 16, so 17 and 18).
    if (!graphics::dxbc_uses_tiled_status(container)) return;
    u32 srv_width = 0;
    u32 sampler_width = 0;
    u32 uav_width = 0;
    if (out.table_mode) {
        srv_width = out.registers.srv_slots;
        sampler_width = out.registers.sampler_slots;
        uav_width = out.registers.uav_slots;
    } else {
        for (const auto& d : out.declarations) {
            if (d.space != 0) continue;
            if (d.register_class == graphics::DxbcRegisterClass::ShaderResource) {
                srv_width = std::max(srv_width, d.upper + 1u);
            } else if (d.register_class == graphics::DxbcRegisterClass::Sampler) {
                sampler_width = std::max(sampler_width, d.upper + 1u);
            }
        }
        uav_width = UAV_TABLE_SLOTS;
    }
    out.tiled_status = true;
    out.srv_companion_base = srv_width;
    out.uav_companion_base = 2u * srv_width;
    out.status_sampler_count = sampler_width;
    out.status_sampler_base = out.table_mode ? 2u * sampler_width : sampler_width;
    out.srv_residency_slot = cbv_slot;
    out.uav_residency_slot = cbv_slot + 1u;
    auto& t = options.tiled_status;
    t.enabled = true;
    t.srv_companion_base = out.srv_companion_base;
    t.uav_companion_base = out.uav_companion_base;
    t.status_sampler_base = out.status_sampler_base;
    t.srv_residency = true;
    t.srv_residency_set = 0;
    t.srv_residency_binding = out.srv_residency_slot;
    t.uav_residency = true;
    t.uav_residency_set = 0;
    t.uav_residency_binding = out.uav_residency_slot;
    // The declared shape of each texture slot: a companion slot binds a
    // texture of it (dcl_resource 88 / dcl_uav_typed 156, dimension per
    // D3D10_SB_RESOURCE_DIMENSION).
    out.srv_slot_dimension.assign(srv_width, 0u);
    out.uav_slot_dimension.assign(uav_width, 0u);
    for (const auto& d : out.declarations) {
        const bool srv = d.opcode == 88u &&
            d.register_class == graphics::DxbcRegisterClass::ShaderResource;
        const bool uav = d.opcode == 156u &&
            d.register_class == graphics::DxbcRegisterClass::UnorderedAccess;
        if (!srv && !uav) continue;
        const u32 set = srv ? 1u : 3u;
        std::vector<u8>& dims = srv ? out.srv_slot_dimension : out.uav_slot_dimension;
        const u64 count = d.unbounded() ? UNBOUNDED_ARRAY_SLOTS
                                        : static_cast<u64>(d.upper) - d.lower + 1u;
        for (u64 k = 0; k < count; ++k) {
            const u32 slot = out.slot_of(set, d.space, d.lower + static_cast<u32>(k));
            if (slot < dims.size()) dims[slot] = static_cast<u8>(d.dimension);
        }
    }
}

void finish_tiled_status_counts(CompiledShader& out) {
    if (!out.tiled_status) return;
    out.registers.srv_slots = std::max(
        out.registers.srv_slots,
        out.uav_companion_base + static_cast<u32>(out.uav_slot_dimension.size()));
    out.registers.sampler_slots = std::max(
        out.registers.sampler_slots,
        out.status_sampler_base + out.status_sampler_count);
    out.registers.cbv_slots =
        std::max(out.registers.cbv_slots, out.uav_residency_slot + 1u);
}

/// The SRV slots declared as acceleration structures (table mode: the
/// DXIL lowering is the only producer).
void mark_acceleration_structure_slots(CompiledShader& out) {
    out.srv_slot_acceleration.assign(out.registers.srv_slots, 0u);
    out.rt_reserved.clear();
    // Bounded acceleration-structure ranges in declaration order: their
    // companions sit at t(kRtCompanionBase + ...) in that order, one element
    // per element (dxil_lower_rt.inc add_rq_companions, the megakernel's).
    struct AsRange {
        u32 first_slot;
        u32 count;
    };
    std::vector<AsRange> as_ranges;
    for (const auto& d : out.declarations) {
        if (d.opcode != graphics::dxbc_vitreo_ext::kOpDclAccelerationStructure) continue;
        const u64 count = d.unbounded() ? UNBOUNDED_ARRAY_SLOTS
                                        : static_cast<u64>(d.upper) - d.lower + 1u;
        for (u64 k = 0; k < count; ++k) {
            const u32 slot = out.slot_of(1u, d.space, d.lower + static_cast<u32>(k));
            if (slot < out.srv_slot_acceleration.size()) out.srv_slot_acceleration[slot] = 1u;
        }
        if (!d.unbounded()) {
            as_ranges.push_back({out.slot_of(1u, d.space, d.lower), static_cast<u32>(count)});
        }
    }
    for (const auto& d : out.declarations) {
        if (d.space != graphics::kDxilRtSpace) continue;
        const u32 set = set_of_class(d.register_class);
        const u32 count = d.unbounded() ? 0u : d.upper - d.lower + 1u;
        CompiledShader::RtReserved r{set, out.slot_of(set, d.space, d.lower), 0, 0, count};
        if (d.register_class == graphics::DxbcRegisterClass::ConstantBuffer &&
            d.lower < graphics::kRtLocalViewBase) {
            r.what = 4;  // the DispatchRays constants
        } else if (d.register_class == graphics::DxbcRegisterClass::Sampler) {
            r.what = 8;
            r.as_slot = d.lower - graphics::kRtLocalSamplerBase;
        } else if (d.lower >= graphics::kRtLocalViewBase) {
            r.what = 6;
            r.as_slot = (d.lower - graphics::kRtLocalViewBase) / graphics::kRtLocalViewSlots;
        } else if (set == 1u && d.lower < 4) {
            r.what = d.lower;  // a shader table
        } else if (set == 1u && d.lower == graphics::kRtTranslationRegister) {
            r.what = 7;
        } else if (set == 1u && d.lower >= graphics::kRtCompanionBase) {
            u32 at = graphics::kRtCompanionBase;
            bool found = false;
            for (const AsRange& range : as_ranges) {
                if (at == d.lower) {
                    r.what = 5;
                    r.as_slot = range.first_slot;
                    r.count = range.count;
                    found = true;
                    break;
                }
                at += range.count;
            }
            if (!found) continue;
        } else {
            continue;
        }
        out.rt_reserved.push_back(r);
    }
}

void finish_register_counts(CompiledShader& out,
                            const graphics::DxbcToSpirvResult& spirv,
                            const graphics::DxbcTranslationOptions& options) {
    if (out.table_mode) {
        if (out.sampler_state_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.sampler_state_slot + 1u);
        }
        if (out.srv_view_size_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.srv_view_size_slot + 1u);
        }
        if (out.uav_view_size_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.uav_view_size_slot + 1u);
        }
        if (out.cbv_limit_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.cbv_limit_slot + 1u);
        }
        if (out.srv_null_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.srv_null_slot + 1u);
        }
        if (out.uav_null_slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, out.uav_null_slot + 1u);
        }
        if (out.srv_typed_bias_slot != ~0u) {
            out.registers.cbv_slots = std::max(out.registers.cbv_slots,
                                               out.srv_typed_bias_slot + 1u);
        }
        if (out.uav_typed_bias_slot != ~0u) {
            out.registers.cbv_slots = std::max(out.registers.cbv_slots,
                                               out.uav_typed_bias_slot + 1u);
        }
        if (out.heap_window.present) {
            out.registers.cbv_slots =
                std::max({out.registers.cbv_slots,
                          out.heap_window.params_slot + 1u,
                          out.heap_window.side_slot + 1u});
            if (out.heap_window.residency_slot != ~0u) {
                out.registers.cbv_slots = std::max(
                    out.registers.cbv_slots, out.heap_window.residency_slot + 1u);
            }
            out.heap_window.aux = spirv.heap_window_aux;
        }
        if (options.sampler_state_buffer.enabled &&
            out.registers.sampler_slots != 0u) {
            out.registers.sampler_slots *= 2u;
        }
        finish_tiled_status_counts(out);
        mark_acceleration_structure_slots(out);
        return;
    }
    out.registers.cbv_slots = register_high_water(spirv.cbuffer_bindings);
    if (out.srv_null_slot != ~0u) {
        out.registers.cbv_slots =
            std::max(out.registers.cbv_slots, out.srv_null_slot + 1u);
    }
    if (out.uav_null_slot != ~0u) {
        out.registers.cbv_slots =
            std::max(out.registers.cbv_slots, out.uav_null_slot + 1u);
    }
    for (const u32 slot : {out.srv_typed_bias_slot, out.uav_typed_bias_slot}) {
        if (slot != ~0u) {
            out.registers.cbv_slots =
                std::max(out.registers.cbv_slots, slot + 1u);
        }
    }
    out.registers.srv_slots = register_high_water(spirv.texture_bindings);
    out.registers.sampler_slots = register_high_water(spirv.sampler_bindings);
    out.registers.uav_slots = UAV_TABLE_SLOTS;
    if (out.tiled_status &&
        (out.registers.srv_slots > out.srv_companion_base ||
         out.registers.sampler_slots > out.status_sampler_base)) {
        // A register past the declarations would share a companion's slot.
        unimplemented("ID3D12Device::Create*PipelineState",
                      "tiled status with an undeclared t# or s# register");
    }
    finish_tiled_status_counts(out);
}

void argument_buffer_options(const CompiledShader& shader,
                             graphics::MslCompileOptions& msl) {
    msl.argument_buffers = true;
    const auto index = [](u32 set) {
        return graphics::BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    msl.argument_buffer_sets[0] = {shader.registers.cbv_slots, index(0),
                                   graphics::MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {shader.registers.srv_slots, index(1),
                                   graphics::MslDescriptorClass::Texture};
    msl.argument_buffer_sets[2] = {shader.registers.sampler_slots, index(2),
                                   graphics::MslDescriptorClass::Sampler};
    msl.argument_buffer_sets[3] = {shader.registers.uav_slots, index(3),
                                   graphics::MslDescriptorClass::Texture};
    if (shader.heap_window.present) {
        msl.heap_window.enabled = true;
        msl.heap_window.descriptor_set =
            graphics::SPIRV_HEAP_WINDOW_DESCRIPTOR_SET;
        msl.heap_window.metal_buffer_index =
            index(graphics::BACKEND_DESCRIPTOR_SET_HEAP_WINDOW);
    }
}

/// Argument-buffer compilation leaves descriptor set 4 discrete, so
/// SPIRV-Cross numbers those buffers from zero. The draw already binds them
/// at the flat expansion window (or the vertex capture index). Rewrite only
/// the kernel parameter's `[[buffer(n)]]`.
void retarget_expansion_buffer(std::string& msl, const char* name, u32 index) {
    const std::string key = std::string(name) + " [[buffer(";
    for (usize at = 0; (at = msl.find(key, at)) != std::string::npos;) {
        const usize number = at + key.size();
        const usize end = msl.find(')', number);
        if (end == std::string::npos) return;
        msl.replace(number, end - number, std::to_string(index));
        at = number;
    }
}

void retarget_lowered_buffers(std::string& msl, bool vertex_capture) {
    if (vertex_capture) {
        retarget_expansion_buffer(
            msl, "vitreo_gs_in", graphics::BACKEND_VERTEX_CAPTURE_BUFFER_INDEX);
        return;
    }
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    const std::pair<const char*, u32> buffers[] = {
        {"vitreo_gs_in", 0},
        {"vitreo_gs_out", 1},
        {"vitreo_gs_counter", 2},
        {"vitreo_gs_index", 3},
        {"vitreo_hs_in", 0},
        {"vitreo_hs_cp_out", 1},
        {"vitreo_tess_control", 2},
        {"vitreo_hs_index", 3},
        {"vitreo_hs_pc_out", 4},
        {"vitreo_hs_tess_factors", 5},
        {"vitreo_ds_points", 0},
        {"vitreo_ds_records", 3},
    };
    for (const auto& buffer : buffers) {
        retarget_expansion_buffer(msl, buffer.first, base + buffer.second);
    }
}

/// Scalar fragment inputs of a raster-emulated pixel shader with pass C's
/// edge data as varyings: a float4 / int4 per user input register (v#,
/// clip / cull distances; the translator declares whole registers), three
/// copies of each GetAttributeAtVertex register, the SV_Barycentrics
/// weights, and the edge data's 6 int4 -- counted as a quad's 7 whatever
/// the primitive, so a pixel shader translated again for the primitive a
/// geometry or domain shader turns out to rasterize keeps its choice.
[[nodiscard]] u32 raster_fragment_components(const graphics::DxbcContainer& ps,
                                             const graphics::DxbcToSpirvResult& spirv) {
    u32 registers = 0;
    u32 scalars = 0;
    for (const auto& input : ps.input_signature) {
        const u32 sv = input.system_value_type;
        // Scalar rasterizer inputs take a component each (measured on the
        // M4 Pro: 31 float4 inputs and the UAV leader's [[sample_id]] are
        // "125 varying components"): SV_RenderTargetArrayIndex,
        // SV_ViewportArrayIndex, SV_PrimitiveID, SV_IsFrontFace,
        // SV_SampleIndex.
        if (sv == 4u || sv == 5u || sv == 7u || sv == 9u || sv == 10u) ++scalars;
        if (input.register_num >= 32 ||
            !(sv == 0 || sv == D3D_NAME_CLIP_DISTANCE || sv == D3D_NAME_CULL_DISTANCE)) {
            continue;
        }
        registers |= 1u << input.register_num;
    }
    // The once-per-pixel UAV gate reads [[sample_id]]; the edge buffer
    // [[primitive_id]] (counted here whichever way the edges go).
    if (graphics::dxbc_writes_memory(ps)) ++scalars;
    if (std::none_of(ps.input_signature.begin(), ps.input_signature.end(),
                     [](const graphics::DxbcSignatureElement& e) { return e.system_value_type == 7u; })) {
        ++scalars;
    }
    u32 components = 4u * static_cast<u32>(std::popcount(registers)) + scalars;
    components += 12u * static_cast<u32>(std::popcount(spirv.per_vertex_inputs));
    components += 4u * static_cast<u32>(std::popcount(spirv.raster_barycentrics));
    components += 4u * (graphics::kRasterEmulationLocations + 1u);
    return components;
}

}  // namespace

u32 Frontend::compile_shader(std::vector<u8> bytecode,
                             graphics::ShaderStage stage, CompiledShader& out,
                             const char* api, const RootLayout* root,
                             u32 visibility_stage,
                             const LogicOpVariant* logic,
                             const ShaderVariantOptions* variant,
                             const graphics::RasterEmulationOptions* raster,
                             const graphics::MeshStageLayout* mesh,
                             u32 mesh_payload_words) {
    out = CompiledShader{};
    if (bytecode.empty()) return S_OK;  // no shader at this stage
    out.bytecode = std::move(bytecode);
    if (fourcc_present(out.bytecode, "DXIL")) {
        // read_shader_bytes lowers DXIL to SM 5.1 (lower_dxil); a DXIL
        // container reaching here came by another route.
        unimplemented(api, "DXIL that was not lowered");
        return E_INVALIDARG;
    }
    graphics::DxbcTranslationOptions options{};
    options.view_id = build_scratch().view_id;
    // SV_CullDistance of a vertex shader: the draw culls through a vertex
    // capture (PipelineState::vertex_cull), or a later stage reads it.
    options.vertex_cull_distances_culled_by_caller = true;
    if (stage == graphics::ShaderStage::Fragment) {
        options.pixel_primitive_split = build_scratch().primitive_split;
    }
    // Measured (a05 pso "CS: bytecode truncated to 64 bytes"): a container
    // shorter than its TotalSize, or no container at all, is E_INVALIDARG;
    // ("CS: corrupted checksum") a whole one whose hash does not match is
    // E_FAIL.
    u32 total_size = 0;
    if (out.bytecode.size() >= 32) {
        std::memcpy(&total_size, out.bytecode.data() + 24, 4);
    }
    if (out.bytecode.size() < 32 ||
        std::memcmp(out.bytecode.data(), "DXBC", 4) != 0 ||
        total_size > out.bytecode.size()) {
        d3d12_diag("%s: %s: not a whole DXBC container (%zu bytes, TotalSize %u)", api,
                   stage_label(stage), out.bytecode.size(), total_size);
        return E_INVALIDARG;
    }
    if (!graphics::dxbc_container_verified(out.bytecode.data(),
                                           out.bytecode.size())) {
        d3d12_diag("%s: %s: DXBC container hash mismatch", api, stage_label(stage));
        return E_FAIL;
    }
    const auto container =
        graphics::parse_dxbc(out.bytecode.data(), out.bytecode.size());
    // A mesh / amplification shader is a lowered compute program whose VXMS
    // chunk names the stage (graphics/shaders/mesh_stage.hpp).
    const bool mesh_stage = stage == graphics::ShaderStage::Mesh ||
                            stage == graphics::ShaderStage::Amplification;
    const u32 mesh_kind = stage == graphics::ShaderStage::Mesh
                              ? graphics::DxbcMeshState::Mesh
                              : graphics::DxbcMeshState::Amplification;
    if (!container.valid() || !container.is_sm4_plus() ||
        (mesh_stage ? container.stage != graphics::ShaderStage::Compute ||
                          container.mesh.kind != mesh_kind
                    : container.stage != stage || container.mesh.kind != 0)) {
        d3d12_diag("%s: %s bytecode is not a DXBC shader of the stage (parsed %s, "
                   "SM4+ %d, its stage %s, mesh kind %u)",
                   api, stage_label(stage), container.valid() ? "valid" : "invalid",
                   container.is_sm4_plus() ? 1 : 0, stage_label(container.stage),
                   static_cast<unsigned>(container.mesh.kind));
        return E_INVALIDARG;
    }
    out.declarations = container.binding_declarations;
    out.cb_values.assign(out.declarations.size(), 0u);
    for (usize index = 0; index < out.declarations.size(); ++index) {
        const auto& d = out.declarations[index];
        if (d.register_class != graphics::DxbcRegisterClass::ConstantBuffer ||
            d.name.empty()) {
            continue;
        }
        for (const auto& cb : container.constant_buffers) {
            if (cb.name != d.name) continue;
            u32 end = 0;
            for (const auto& v : cb.variables) end = std::max(end, v.offset + v.size);
            out.cb_values[index] = (end + 3u) / 4u;
        }
    }
    if (logic != nullptr && logic->targets != 0 &&
        stage == graphics::ShaderStage::Fragment) {
        options.logic_op = logic->op;
        options.logic_op_per_target = logic->per_target;
        options.logic_ops = logic->ops;
        options.logic_op_targets = logic->targets;
        options.logic_op_component_masks = logic->component_masks;
    }
    if (variant != nullptr) {
        options.count_pixel_invocations =
            variant->count_pixel_invocations &&
            stage == graphics::ShaderStage::Fragment;
        options.primitive_id_varying =
            variant->primitive_id_varying &&
            stage == graphics::ShaderStage::Fragment;
        options.line_barycentrics = variant->line_barycentrics &&
                                    stage == graphics::ShaderStage::Fragment;
        if (variant->capture_rasterized &&
            stage == graphics::ShaderStage::Vertex) {
            options.capture_rasterized_vertex_outputs = true;
            options.capture_draw_relative = true;
        }
        options.vertex_input_unpack_uint1010102 =
            variant->vertex_input_unpack_uint1010102;
        options.vertex_input_component_types =
            variant->vertex_input_component_types;
        if (stage == graphics::ShaderStage::Vertex) {
            options.vertex_pulling = variant->vertex_pulling;
            options.vertex_pull = variant->vertex_pull;
        }
    }
    if (u32 error = configure_descriptor_bindings(container, root, out, options,
                                                  api);
        error != S_OK) {
        return error;
    }
    // Learn (Rasterizer Order Views): a pixel shader's RasterizerOrdered*
    // UAVs are ordered per pixel in primitive order -- Metal raster order
    // group 0 (OPTIONS::ROVsSupported, from the backend).
    options.rasterizer_ordered_views = caps().rovs;
    if (raster != nullptr && stage == graphics::ShaderStage::Fragment &&
        raster->mode != graphics::RasterEmulationMode::Off) {
        // Target-independent / conservative rasterization: the pixel shader
        // decides coverage from pass C's flat edge functions, and Metal draws
        // three triangles for each D3D one (SV_PrimitiveID).
        options.raster_emulation = *raster;
        options.line_quad_primitive_divisor =
            graphics::kRasterTrianglesPerPrimitive;
    }
    options.mesh_layout = mesh;
    options.uavs_in_every_stage = true;  // D3D12: UAVs in all stages
    options.typed_uav_uint_views = true;  // Replayer::bind_view
    auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (spirv.valid() && options.raster_emulation.mode != graphics::RasterEmulationMode::Off) {
        const u32 components = raster_fragment_components(container, spirv);
        const u32 edge_components = 4u * (graphics::kRasterEmulationLocations + 1u);
        if (components - edge_components > graphics::kMaxFragmentInputComponents) {
            unimplemented(api, "conservative / target-independent rasterization of a "
                               "pixel shader whose own inputs exceed Metal's 124 "
                               "fragment input components");
            return E_INVALIDARG;
        }
        if (!options.raster_emulation.edges_in_buffer &&
            components > graphics::kMaxFragmentInputComponents) {
            // The edge data does not fit beside the shader's own inputs: it
            // is read from the edge buffer instead.
            options.raster_emulation.edges_in_buffer = true;
            spirv = graphics::translate_dxbc_to_spirv(container, options);
        }
    }
    out.raster_edges_in_buffer = options.raster_emulation.edges_in_buffer;
    if (!spirv.valid()) {
        const std::string why = std::string(stage_label(stage)) +
                                " DXBC->SPIR-V: " +
                                (spirv.failure.empty() ? "no reason given" : spirv.failure);
        if (spirv.invalid_program) {
            // The program is one the spec calls invalid.
            return refuse(E_INVALIDARG, "%s: %s", api, why.c_str());
        }
        unimplemented(api, why.c_str());
        return E_INVALIDARG;
    }
    if (!note_uint_view_uavs(out, spirv, options, api)) return E_INVALIDARG;
    if (variant != nullptr && variant->capture_registers != nullptr) {
        *variant->capture_registers = spirv.captures_vertex_outputs
                                          ? spirv.capture_registers
                                          : std::vector<u32>{};
    }
    finish_register_counts(out, spirv, options);
    out.input_interpolation = spirv.pixel_input_interpolation;
    out.per_vertex_inputs = spirv.per_vertex_inputs;
    out.raster_barycentrics = spirv.raster_barycentrics;
    // Learn (CreateUnorderedAccessView): the counter is the UAV descriptor's
    // counter resource, placed at CounterOffsetInBytes. The draw binds it.
    out.uav_counter_registers = spirv.uav_counter_registers;
    out.uav_counter_spaces = spirv.uav_counter_spaces;
    out.typed_uav_atomics = spirv.uses_typed_uav_atomics;
    graphics::MslCompileOptions msl{};
    argument_buffer_options(out, msl);
    // The logic-op variant reads each target through framebuffer fetch.
    msl.framebuffer_fetch = options.logic_op_targets != 0;
    if (variant != nullptr && stage == graphics::ShaderStage::Fragment) {
        msl.fragment_output_mask = variant->color_output_mask;
    }
    if (spirv.uses_typed_uav_atomics && msl.msl_version < 30100u) {
        // SPIRV-Cross lowers typed UAV atomics natively only from MSL 3.1
        // (see D3D11's raise_msl_version_for_typed_uav_atomics).
        msl.msl_version = 30100u;
    }
    // Precise ops stay on the isolated helpers. The file-scope contraction
    // pragma is not applied: it disabled FMAs for every op in the shader.
    msl.precise_without_helpers = true;
    msl.fp_contract_off = false;
    for (const auto& binding : spirv.bindings) {
        if (binding.declaration.rasterizer_ordered) {
            msl.raster_ordered_bindings.emplace_back(binding.descriptor_set,
                                                     binding.binding);
        }
    }
    if (mesh_stage) {
        msl.mesh_stage = mesh;
        msl.mesh_payload_words = mesh_payload_words;
    }
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), stage, msl);
    if (!compiled.valid()) {
        // A SPIR-V module the translator made that MSL cannot express.
        const std::string why =
            std::string(stage_label(stage)) + " SPIR-V->MSL: " +
            (compiled.failure.empty() ? "SPIRV-Cross produced no MSL" : compiled.failure);
        unimplemented(api, why.c_str());
        return E_INVALIDARG;
    }
    out.present = true;
    out.msl = compiled.msl_text();
    // Argument-buffer compilation numbers the discrete capture buffer from
    // zero; the draw binds it at the vertex capture index.
    if (options.capture_rasterized_vertex_outputs) {
        retarget_lowered_buffers(out.msl, /*vertex_capture=*/true);
    }
    out.threads_x = compiled.workgroup_size_x;
    out.threads_y = compiled.workgroup_size_y;
    out.threads_z = compiled.workgroup_size_z;
    // D3D11.3 3.1.3.1 / 22.3.2 / 22.19.2, as the D3D11 frontend applies them:
    // without refactoringAllowed, or with precise, the Safe math mode.
    const auto contract = graphics::dxbc_math_contract(container);
    out.math_mode = !contract.refactoring_allowed || contract.uses_precise
                        ? graphics::BackendMathMode::Safe
                        : graphics::BackendMathMode::Relaxed;
    // Every rasterized position is [[invariant]]
    // (dxbc_to_spirv_signature.cpp): passes over the same geometry with
    // different shaders agree bit for bit, as on D3D.
    out.preserve_invariance = true;
    for (const auto& input : container.input_signature) {
        CompiledShader::Input in{};
        in.semantic = input.semantic_name;
        in.semantic_index = input.semantic_index;
        in.register_num = input.register_num;
        in.component_type = input.component_type;
        in.system_value = input.system_value_type;
        out.inputs.push_back(std::move(in));
    }
    for (const auto& output : container.output_signature) {
        if (output.system_value_type == 4u) out.writes_layer = true;
    }
    if (stage == graphics::ShaderStage::Fragment) {
        for (const auto& output : container.output_signature) {
            if (semantic_equal(output.semantic_name, "SV_Target") &&
                output.semantic_index < 8) {
                out.target_mask |= 1u << output.semantic_index;
            }
        }
    }
    (void)visibility_stage;
    return S_OK;
}

// =============================================================================
// Pipeline state objects
// =============================================================================

namespace {

void read_blend(Frontend& f, GuestAddr at, GraphicsState& state) {
    const auto& l = layout::D3D12_BLEND_DESC[f.m_is_64bit];
    const auto& rt = layout::D3D12_RENDER_TARGET_BLEND_DESC[f.m_is_64bit];
    state.alpha_to_coverage = f.read_u32(at + l.AlphaToCoverageEnable) != 0;
    state.independent_blend = f.read_u32(at + l.IndependentBlendEnable) != 0;
    for (u32 index = 0; index < 8; ++index) {
        const GuestAddr target = at + l.RenderTarget + index * rt.size;
        RenderTargetBlend& b = state.blend[index];
        b.blend_enable = f.read_u32(target + rt.BlendEnable) != 0;
        b.logic_op_enable = f.read_u32(target + rt.LogicOpEnable) != 0;
        b.src_blend = f.read_u32(target + rt.SrcBlend);
        b.dest_blend = f.read_u32(target + rt.DestBlend);
        b.blend_op = f.read_u32(target + rt.BlendOp);
        b.src_blend_alpha = f.read_u32(target + rt.SrcBlendAlpha);
        b.dest_blend_alpha = f.read_u32(target + rt.DestBlendAlpha);
        b.blend_op_alpha = f.read_u32(target + rt.BlendOpAlpha);
        b.logic_op = f.read_u32(target + rt.LogicOp);
        const auto mask = f.m_memory.read_u8(target + rt.RenderTargetWriteMask);
        b.write_mask = mask.is_ok() ? mask.value() : 0u;
    }
}

void read_rasterizer(Frontend& f, GuestAddr at, GraphicsState& state) {
    const auto& l = layout::D3D12_RASTERIZER_DESC[f.m_is_64bit];
    state.fill_mode = f.read_u32(at + l.FillMode);
    state.cull_mode = f.read_u32(at + l.CullMode);
    state.front_ccw = f.read_u32(at + l.FrontCounterClockwise) != 0;
    state.depth_bias =
        static_cast<float>(static_cast<i32>(f.read_u32(at + l.DepthBias)));
    state.depth_bias_clamp = f.read_f32(at + l.DepthBiasClamp);
    state.slope_scaled_depth_bias = f.read_f32(at + l.SlopeScaledDepthBias);
    state.depth_clip = f.read_u32(at + l.DepthClipEnable) != 0;
    state.multisample = f.read_u32(at + l.MultisampleEnable) != 0;
    state.antialiased_line = f.read_u32(at + l.AntialiasedLineEnable) != 0;
    state.forced_sample_count = f.read_u32(at + l.ForcedSampleCount);
    state.conservative = f.read_u32(at + l.ConservativeRaster);
}

/// The rasterizer rows of a PSO description every device refuses
/// (E_INVALIDARG), checked before the pixel shader is translated.
[[nodiscard]] bool raster_state_valid(const GraphicsState& state,
                                      u32 conservative_tier, bool pixel_shader) {
    // Measured (d3d12_a31_raster_probe "pso" rows, WARP and an RTX 4080
    // SUPER alike): ConservativeRaster is OFF or ON, and ON is SOLID
    // triangles only ("the only valid Fill Mode for Conservative
    // Rasterization is D3D11_FILL_SOLID"; lines and points are undefined);
    // a tier-0 device refuses it. Lines and points are refused only with a
    // pixel shader: with none the pipeline is accepted, with or without a
    // depth-stencil format (measured, d3d12_a66_c6_probe "pso crnops cr
    // lines / points [depth]" S_OK, "pso crnops cr lines ps" E_INVALIDARG).
    if (state.conservative > D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) {
        return false;
    }
    if (state.conservative == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON &&
        (conservative_tier == 0 ||
         state.fill_mode != D3D12_FILL_MODE_SOLID ||
         (pixel_shader &&
          (state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ||
           state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT)))) {
        return false;
    }
    // D3D11.3 3.5.6.1 / 3.5.6.4: ForcedSampleCount is one of the standard
    // patterns 0, 1, 4, 8, 16 (2 is not one, measured); with a forced count
    // no depth-stencil view may be bound (measured: a DSVFormat is refused
    // whether depth is enabled or not), and above 1 the render targets are
    // single-sample (measured: a 4x target is refused). Every accepted
    // combination rasterizes through the emulation (raster_emulation_for).
    const u32 forced = state.forced_sample_count;
    if ((forced != 0 && forced != 1 && forced != 4 && forced != 8 &&
         forced != 16) ||
        (forced != 0 && state.dsv_format != DXGI_FORMAT_UNKNOWN) ||
        (forced > 1 && state.sample_count > 1)) {
        return false;
    }
    return true;
}

/// D3D11.3 3.5.6 / ConservativeRasterization.md, as the PSO describes it:
/// which emulation draws it (Off: ordinary rasterization). ForcedSampleCount
/// N > 1 -- over single-sample targets or none -- and 1 over multisampled
/// targets are target-independent; 0, and 1 over one sample, are ordinary.
/// finish_graphics_pipeline refuses the descriptions no device accepts.
/// No colour output: NumRenderTargets 0, or every format below it UNKNOWN
/// (measured, d3d12_a05 "NumRenderTargets 1, RTVFormats[0] UNKNOWN": S_OK;
/// a pixel shader writing SV_Target0 needs the count, d3d12_a66_c6_probe's
/// UAV-only pipelines are built so), as UAV-only rendering draws.
[[nodiscard]] bool no_color_formats(const GraphicsState& g) {
    for (u32 index = 0; index < g.num_render_targets && index < 8; ++index) {
        if (g.rtv_formats[index] != DXGI_FORMAT_UNKNOWN) return false;
    }
    return true;
}

/// The SV_Target locations the pipeline has a colour attachment for: below
/// NumRenderTargets with a format. A pixel shader's other SV_Target writes
/// have no target and are discarded (Windows accepts them, a72 pso; a72
/// draw: the bound targets keep exactly what the shader wrote to them, an
/// unbound output changes nothing), so its MSL does not write them.
[[nodiscard]] u32 color_output_mask(const GraphicsState& g) {
    u32 mask = 0;
    for (u32 index = 0; index < g.num_render_targets && index < 8; ++index) {
        if (g.rtv_formats[index] != DXGI_FORMAT_UNKNOWN) mask |= 1u << index;
    }
    return mask;
}

/// What pass C draws for a primitive of @p vertices vertices under @p mode
/// (raster_emulation_types.hpp RasterPrimitive), and a Quad line's width:
///   - a triangle, its three edges;
///   - a point, the width-1 square (D3D11.3 3.4.6, 15.14.2: always as if
///     MultisampleEnable);
///   - a line with MultisampleEnable (LINE_RASTERIZATION_MODE_QUADRILATERAL_
///     WIDE / _NARROW), the 1.4- / 1.0-wide rectangle (3.4.5, 15.14.1);
///   - an aliased or alpha-antialiased line (MultisampleEnable false), the
///     aliased diamond rule with every sample hit (15.14.1: these
///     algorithms are "sample-pattern agnostic"; the alpha-based coverage
///     is the implementation's, 3.4.4).
/// Conservative rasterization never sees a line or point: the runtime
/// refuses a pipeline whose rasterized primitive is one, from the input
/// assembler, a geometry shader or a domain shader alike (measured,
/// d3d12_a66_c6_probe "pso lines cr", "pso bary cr gs lines", "pso bary cr
/// ds isolines": E_INVALIDARG), so a line here is target-independent.
[[nodiscard]] graphics::RasterPrimitive raster_primitive_for(
    const GraphicsState& g, graphics::RasterEmulationMode mode, u32 vertices,
    float& line_width) {
    (void)mode;
    line_width = 1.0f;
    if (vertices >= 3) return graphics::RasterPrimitive::Triangle;
    if (vertices == 1) return graphics::RasterPrimitive::Quad;
    if (g.multisample) {
        line_width = g.narrow_quad_line ? 1.0f : 1.4f;
        return graphics::RasterPrimitive::Quad;
    }
    return graphics::RasterPrimitive::AliasedLine;
}

[[nodiscard]] graphics::RasterEmulationOptions raster_emulation_for(
    const GraphicsState& g) {
    graphics::RasterEmulationOptions r{};
    if (g.conservative == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) {
        r.mode = graphics::RasterEmulationMode::Conservative;
        r.samples = g.forced_sample_count != 0 ? g.forced_sample_count
                                               : std::max(g.sample_count, 1u);
        r.sample_mask = g.sample_mask;
    } else if (g.forced_sample_count > 1 ||
               (g.forced_sample_count == 1 && g.sample_count > 1)) {
        r.mode = graphics::RasterEmulationMode::TargetIndependent;
        r.samples = g.forced_sample_count;
    } else if (g.forced_sample_count == 0 && g.sample_count > 1 &&
               no_color_formats(g) &&
               g.dsv_format == DXGI_FORMAT_UNKNOWN) {
        // UAV-only rendering at SampleDesc.Count N rasterizes the N-sample
        // standard pattern (measured: d3d12_a31_raster_probe `tir_uav
        // samples=N forced=0` rows equal the forced=N rows on both oracles;
        // OPTIONS19 SupportedSampleCountsWithNoOutputs 0x1F on WARP).
        r.mode = graphics::RasterEmulationMode::TargetIndependent;
        r.samples = g.sample_count;
    }
    return r;
}

void read_depth_stencil(Frontend& f, GuestAddr at, bool desc1,
                        GraphicsState& state) {
    const auto& l = layout::D3D12_DEPTH_STENCIL_DESC1[f.m_is_64bit];
    static_assert(layout::D3D12_DEPTH_STENCIL_DESC[1].BackFace_StencilFunc ==
                  layout::D3D12_DEPTH_STENCIL_DESC1[1].BackFace_StencilFunc);
    state.depth_enable = f.read_u32(at + l.DepthEnable) != 0;
    state.depth_write_mask = f.read_u32(at + l.DepthWriteMask);
    state.depth_func = f.read_u32(at + l.DepthFunc);
    state.stencil_enable = f.read_u32(at + l.StencilEnable) != 0;
    const auto read_mask = f.m_memory.read_u8(at + l.StencilReadMask);
    const auto write_mask = f.m_memory.read_u8(at + l.StencilWriteMask);
    state.stencil_read_mask = read_mask.is_ok() ? read_mask.value() : 0;
    state.stencil_write_mask = write_mask.is_ok() ? write_mask.value() : 0;
    state.front = {f.read_u32(at + l.FrontFace_StencilFailOp),
                   f.read_u32(at + l.FrontFace_StencilDepthFailOp),
                   f.read_u32(at + l.FrontFace_StencilPassOp),
                   f.read_u32(at + l.FrontFace_StencilFunc)};
    state.back = {f.read_u32(at + l.BackFace_StencilFailOp),
                  f.read_u32(at + l.BackFace_StencilDepthFailOp),
                  f.read_u32(at + l.BackFace_StencilPassOp),
                  f.read_u32(at + l.BackFace_StencilFunc)};
    state.depth_bounds_test =
        desc1 && f.read_u32(at + l.DepthBoundsTestEnable) != 0;
}

}  // namespace

namespace {

// D3D12_LINE_RASTERIZATION_MODE (Windows SDK 10.0.26100.0 um/d3d12.h; not in
// the mingw-w64 header the constants are generated from).
constexpr u32 D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED = 1;
constexpr u32 D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE = 2;
constexpr u32 D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW = 3;

/// The D3DX12 / runtime defaults a pipeline state stream starts from
/// (d3dx12_pipeline_state_stream.h CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER:
/// CD3DX12_BLEND_DESC(D3D12_DEFAULT), SampleMask UINT_MAX,
/// CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT), CD3DX12_DEPTH_STENCIL_DESC1
/// (D3D12_DEFAULT), SampleDesc {1, 0}).
void default_graphics_state(GraphicsState& state) {
    state = GraphicsState{};
    for (RenderTargetBlend& b : state.blend) {
        b.src_blend = D3D12_BLEND_ONE;
        b.dest_blend = D3D12_BLEND_ZERO;
        b.blend_op = D3D12_BLEND_OP_ADD;
        b.src_blend_alpha = D3D12_BLEND_ONE;
        b.dest_blend_alpha = D3D12_BLEND_ZERO;
        b.blend_op_alpha = D3D12_BLEND_OP_ADD;
        b.logic_op = D3D12_LOGIC_OP_NOOP;
        b.write_mask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    state.sample_mask = 0xFFFFFFFFu;
    state.fill_mode = D3D12_FILL_MODE_SOLID;
    state.cull_mode = D3D12_CULL_MODE_BACK;
    state.depth_clip = true;
    state.depth_enable = true;
    state.depth_write_mask = D3D12_DEPTH_WRITE_MASK_ALL;
    state.depth_func = D3D12_COMPARISON_FUNC_LESS;
    state.stencil_read_mask = 0xFF;
    state.stencil_write_mask = 0xFF;
    state.front = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                   D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    state.back = state.front;
    state.sample_count = 1;
}

}  // namespace

bool Frontend::shader_present(GuestAddr bytecode_struct) {
    const auto& l = layout::D3D12_SHADER_BYTECODE[m_is_64bit];
    GuestAddr pointer = 0;
    GuestAddr length = 0;
    (void)read_pointer(bytecode_struct + l.pShaderBytecode, pointer);
    (void)read_pointer(bytecode_struct + l.BytecodeLength, length);
    return pointer != 0 && length != 0;
}

u32 Frontend::read_input_layout(Marshaler& m, GuestAddr elements,
                                u32 element_count, GraphicsState& state) {
    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[m_is_64bit];
    if (element_count != 0 && elements == 0) {
        return refuse(E_INVALIDARG, "input layout: NumElements %u with NULL pInputElementDescs",
                      element_count);
    }
    state.input_layout.clear();
    for (u32 index = 0; index < element_count; ++index) {
        const GuestAddr at = elements + index * il.size;
        InputElement element{};
        GuestAddr name = 0;
        (void)read_pointer(at + il.SemanticName, name);
        const auto semantic = m.read_ansi_string(name, 256);
        if (name == 0 || !semantic.is_ok()) {
            return refuse(E_INVALIDARG, "input layout: element %u has %s SemanticName", index,
                          name == 0 ? "a NULL" : "an unreadable");
        }
        element.semantic = semantic.value();
        element.semantic_index = read_u32(at + il.SemanticIndex);
        element.format = read_u32(at + il.Format);
        element.input_slot = read_u32(at + il.InputSlot);
        element.aligned_offset = read_u32(at + il.AlignedByteOffset);
        element.slot_class = read_u32(at + il.InputSlotClass);
        element.step_rate = read_u32(at + il.InstanceDataStepRate);
        state.input_layout.push_back(std::move(element));
    }
    return S_OK;
}

bool Frontend::root_covers(const RootLayout& root, const CompiledShader& shader,
                           u32 visibility) {
    // Measured (a05 pso "root signature with 2 of the 4 UAVs", "without the
    // b0 the shaders read", "root constants b0 x1 (shader reads 4)"): every
    // register a shader declares is bound by a root parameter (or static
    // sampler) visible to its stage, and root constants hold the whole
    // constant buffer the shader declares.
    const auto visible = [visibility](u32 v) {
        return visibility == D3D12_SHADER_VISIBILITY_ALL ||
               v == D3D12_SHADER_VISIBILITY_ALL || v == visibility;
    };
    for (usize decl = 0; decl < shader.declarations.size(); ++decl) {
        const auto& d = shader.declarations[decl];
        // Vitreo's own raytracing bindings (an acceleration structure's
        // blob view): no root parameter supplies them.
        if (d.space == graphics::kDxilRtSpace) continue;
        // SM 6.6 dynamic resources (graphics::kDxbcResourceHeapSpace): the
        // heap itself, which the root signature makes reachable by its
        // D3D12_ROOT_SIGNATURE_FLAG_*_HEAP_DIRECTLY_INDEXED flags rather
        // than by a range (Learn, D3D12_ROOT_SIGNATURE_FLAGS; HLSL SM 6.6
        // dynamic resources: a shader that indexes a heap needs the flag).
        if (d.space == graphics::kDxbcResourceHeapSpace ||
            d.space == graphics::kDxbcSamplerHeapSpace) {
            const bool sampler =
                d.register_class == graphics::DxbcRegisterClass::Sampler;
            if (sampler != (d.space == graphics::kDxbcSamplerHeapSpace)) {
                d3d12_diag("heap-indexed declaration in the other heap's space");
                return false;
            }
            const u32 flag =
                sampler ? D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
                        : D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
            if ((root.flags & flag) == 0) {
                d3d12_diag("root signature lacks %s for a heap-indexed shader",
                           sampler ? "SAMPLER_HEAP_DIRECTLY_INDEXED"
                                   : "CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED");
                return false;
            }
            continue;
        }
        u32 range_type = 0;
        u32 root_type = ~0u;
        switch (d.register_class) {
        case graphics::DxbcRegisterClass::ConstantBuffer:
            range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            root_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
            break;
        case graphics::DxbcRegisterClass::ShaderResource:
            range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            root_type = D3D12_ROOT_PARAMETER_TYPE_SRV;
            break;
        case graphics::DxbcRegisterClass::UnorderedAccess:
            range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            root_type = D3D12_ROOT_PARAMETER_TYPE_UAV;
            break;
        case graphics::DxbcRegisterClass::Sampler:
            range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            break;
        }
        const u32 last = d.unbounded() ? d.lower : d.upper;
        for (u32 reg = d.lower; reg <= last; ++reg) {
            bool covered = false;
            for (const auto& p : root.parameters) {
                if (!visible(p.visibility)) continue;
                if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    for (const auto& r : p.ranges) {
                        covered = covered ||
                                  (r.type == range_type && r.space == d.space &&
                                   reg >= r.base_register &&
                                   (r.count == 0xFFFFFFFFu ||
                                    reg - r.base_register < r.count));
                    }
                } else if (p.space == d.space && p.shader_register == reg) {
                    if (p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                        d.register_class ==
                            graphics::DxbcRegisterClass::ConstantBuffer) {
                        // The values the constant buffer's variables span
                        // (RDEF): a cbuffer of one uint takes 1, a float4 4.
                        if (decl < shader.cb_values.size() &&
                            p.num_32bit_values < shader.cb_values[decl]) {
                            d3d12_diag("root constants hold %u values, the "
                                       "constant buffer %u",
                                       p.num_32bit_values, shader.cb_values[decl]);
                            return false;
                        }
                        covered = true;
                    } else if (p.type == root_type) {
                        covered = true;
                    }
                }
            }
            if (d.register_class == graphics::DxbcRegisterClass::Sampler) {
                for (const auto& s : root.static_samplers) {
                    covered = covered || (visible(s.visibility) &&
                                          s.space == d.space &&
                                          s.shader_register == reg);
                }
            }
            if (!covered) {
                d3d12_diag("root signature does not bind class %u space %u "
                           "register %u",
                           static_cast<unsigned>(d.register_class), d.space, reg);
                return false;
            }
        }
    }
    return true;
}

u32 Frontend::accept_stream_output(const PipelineSource& src,
                                      PipelineState& pso, const char* api) {
    // Learn (D3D12_STREAM_OUTPUT_DESC / D3D12_SO_DECLARATION_ENTRY) and the
    // packing rules measured for the same declaration
    // (tools/oracle_probe/d3d11_stream_out_probe.c). A gap is a NULL
    // SemanticName; its ComponentCount may exceed 4.
    const auto refuse = [&](const char* why) {
        d3d12_diag("%s: stream output: %s", api, why);
        return static_cast<u32>(E_INVALIDARG);
    };
    const u32 entries = src.stream_output_entries;
    if (entries == 0) return S_OK;
    if (src.so_declaration == 0) return refuse("pSODeclaration is NULL");
    if (entries > D3D12_SO_STREAM_COUNT * D3D12_SO_OUTPUT_COMPONENT_COUNT) {
        return refuse("too many declaration entries");
    }
    if (src.so_num_strides > D3D12_SO_BUFFER_SLOT_COUNT ||
        (src.so_num_strides != 0 && src.so_strides == 0)) {
        return refuse("bad stride array");
    }
    if (src.so_rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM &&
        src.so_rasterized_stream >= D3D12_SO_STREAM_COUNT) {
        return refuse("RasterizedStream out of range");
    }
    // Learn: the root signature must allow stream output. The documented
    // failure is CreateGraphicsPipelineState when a geometry shader streams
    // without D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; the same page
    // requires the flag whenever stream output is used.
    if (pso.root == nullptr ||
        (pso.root->flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT) ==
            0) {
        return refuse("root signature lacks ALLOW_STREAM_OUTPUT");
    }

    const bool geometry = src.gs != 0 && shader_present(src.gs);
    const bool domain = src.ds != 0 && shader_present(src.ds);
    std::vector<u8> producer_bytes;
    graphics::ShaderStage producer = graphics::ShaderStage::Vertex;
    if (geometry) {
        producer = graphics::ShaderStage::Geometry;
        if (u32 error = read_shader_bytes(src.gs, producer_bytes); error != S_OK) {
            return error;
        }
    } else if (domain) {
        producer = graphics::ShaderStage::Domain;
        if (u32 error = read_shader_bytes(src.ds, producer_bytes); error != S_OK) {
            return error;
        }
    } else {
        producer_bytes = pso.vs.bytecode;
    }
    if (producer_bytes.empty()) return refuse("no producing shader");
    const auto container =
        graphics::parse_dxbc(producer_bytes.data(), producer_bytes.size());
    if (!container.valid() || container.stage != producer) {
        return refuse("stream output shader is not that stage");
    }
    u32 declared_streams = 1u;
    if (geometry) {
        declared_streams = 0;
        for (const auto& out : container.output_signature) {
            if (out.stream < D3D12_SO_STREAM_COUNT) {
                declared_streams |= 1u << out.stream;
            }
        }
        if (declared_streams == 0) declared_streams = 1u;
    }

    const auto& layout = layout::D3D12_SO_DECLARATION_ENTRY[m_is_64bit];
    std::vector<StreamOutputEntry> parsed;
    parsed.reserve(entries);
    std::array<u32, D3D12_SO_BUFFER_SLOT_COUNT> slot_dwords{};
    std::array<bool, D3D12_SO_BUFFER_SLOT_COUNT> slot_used{};
    std::array<u32, D3D12_SO_BUFFER_SLOT_COUNT> slot_stream{};
    std::array<u32, D3D12_SO_STREAM_COUNT> stream_slots{};
    std::array<std::array<u8, 64>, D3D12_SO_STREAM_COUNT> written{};
    u32 highest_slot = 0;
    for (u32 index = 0; index < entries; ++index) {
        const GuestAddr base =
            src.so_declaration + static_cast<GuestAddr>(index) * layout.size;
        const u32 stream = read_u32(base + layout.Stream);
        GuestAddr name_address = 0;
        if (!read_pointer(base + layout.SemanticName, name_address)) {
            return refuse("declaration unreadable");
        }
        const u32 semantic_index = read_u32(base + layout.SemanticIndex);
        const auto start = m_memory.read_u8(base + layout.StartComponent);
        const auto count = m_memory.read_u8(base + layout.ComponentCount);
        const auto slot = m_memory.read_u8(base + layout.OutputSlot);
        if (start.is_err() || count.is_err() || slot.is_err()) {
            return refuse("declaration unreadable");
        }
        if (stream >= D3D12_SO_STREAM_COUNT ||
            (declared_streams & (1u << stream)) == 0) {
            return refuse("entry names a stream the shader does not produce");
        }
        StreamOutputEntry entry{};
        entry.stream = stream;
        entry.slot = slot.value();
        entry.start_component = start.value();
        entry.component_count = count.value();
        if (entry.slot >= D3D12_SO_BUFFER_SLOT_COUNT) {
            return refuse("OutputSlot out of range");
        }
        if (slot_used[entry.slot] && slot_stream[entry.slot] != entry.stream) {
            return refuse("one slot takes two streams");
        }
        if (entry.component_count == 0) return refuse("ComponentCount is 0");
        if (name_address == 0) {
            // Learn: a NULL SemanticName is a gap, and ComponentCount may
            // be greater than 4. The skipped dwords keep their old bytes.
            entry.gap = true;
        } else {
            if (entry.start_component > 3 ||
                entry.start_component + entry.component_count > 4) {
                return refuse("component range out of range");
            }
            std::string name;
            for (usize n = 0; n < 256; ++n) {
                const auto ch = m_memory.read_u8(name_address + n);
                if (ch.is_err()) return refuse("semantic unreadable");
                if (ch.value() == 0) break;
                name.push_back(static_cast<char>(ch.value()));
            }
            const graphics::DxbcSignatureElement* element = nullptr;
            for (const auto& out : container.output_signature) {
                if (out.stream == entry.stream &&
                    out.semantic_index == semantic_index &&
                    semantic_equal(out.semantic_name, name)) {
                    element = &out;
                    break;
                }
            }
            if (element == nullptr) {
                return refuse("semantic is not in the stream's output signature");
            }
            const u32 wanted = ((1u << entry.component_count) - 1u)
                               << entry.start_component;
            if ((element->component_mask & wanted) != wanted) {
                return refuse("components past the element's mask");
            }
            if (element->register_num >= written[0].size()) {
                return refuse("output register out of range");
            }
            u8& used = written[entry.stream][element->register_num];
            if ((used & wanted) != 0) return refuse("component streamed twice");
            used = static_cast<u8>(used | wanted);
            entry.output_register = element->register_num;
        }
        entry.dst_dword = slot_dwords[entry.slot];
        slot_dwords[entry.slot] += entry.component_count;
        slot_used[entry.slot] = true;
        slot_stream[entry.slot] = entry.stream;
        stream_slots[entry.stream] |= 1u << entry.slot;
        highest_slot = std::max(highest_slot, entry.slot);
        parsed.push_back(entry);
    }

    std::array<u32, D3D12_SO_BUFFER_SLOT_COUNT> strides{};
    if (src.so_num_strides == 0) {
        for (u32 slot = 0; slot < D3D12_SO_BUFFER_SLOT_COUNT; ++slot) {
            strides[slot] = slot_dwords[slot] * 4u;
        }
    } else {
        if (slot_used[highest_slot] && highest_slot >= src.so_num_strides) {
            return refuse("a used slot has no stride");
        }
        for (u32 slot = 0; slot < src.so_num_strides; ++slot) {
            strides[slot] = read_u32(src.so_strides + slot * 4u);
            if (strides[slot] % 4u != 0 ||
                strides[slot] > D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES ||
                (slot_used[slot] && strides[slot] < slot_dwords[slot] * 4u)) {
                return refuse("stride is not a multiple of 4, exceeds "
                              "D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES, or is "
                              "smaller than the declared vertex");
            }
        }
    }

    // Vertex-shader and domain-shader registers are filled when that stage
    // is lowered (lower_shader_stages). A domain shader's record is its
    // outputs, one per tessellated vertex, which is what the repack reads.
    // A geometry shader's record is its outputs in ascending register order,
    // which is what the expansion buffer stores.
    std::vector<u32> registers;
    if (geometry) {
        for (const auto& out : container.output_signature) {
            if (std::find(registers.begin(), registers.end(),
                          out.register_num) == registers.end()) {
                registers.push_back(out.register_num);
            }
        }
        std::sort(registers.begin(), registers.end());
    }

    StreamOutput& so = pso.stream_output;
    so.active = true;
    so.entries = std::move(parsed);
    so.strides = strides;
    so.stream_slots = stream_slots;
    so.rasterized_stream = src.so_rasterized_stream;
    so.registers = std::move(registers);
    return S_OK;
}

/// D3D12_VIEW_INSTANCING_DESC (DirectX-Specs "View Instancing", Learn
/// D3D12_VIEW_INSTANCING_DESC): ViewInstanceCount 0 (or no subobject)
/// disables view instancing; at most D3D12_MAX_VIEW_INSTANCE_COUNT views,
/// each with a location; Flags are D3D12_VIEW_INSTANCING_FLAGS.
u32 Frontend::read_view_instancing(const PipelineSource& src,
                                   PipelineState& pso, const char* api) {
    pso.view_instance_count = 0;
    if (src.view_instances == 0) return S_OK;
    if (src.view_instances > D3D12_MAX_VIEW_INSTANCE_COUNT) {
        d3d12_diag("%s: ViewInstanceCount %u > %u", api, src.view_instances,
                   D3D12_MAX_VIEW_INSTANCE_COUNT);
        return E_INVALIDARG;
    }
    if ((src.view_flags &
         ~D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING) != 0) {
        d3d12_diag("%s: view instancing Flags 0x%x", api, src.view_flags);
        return E_INVALIDARG;
    }
    if (src.view_locations == 0) {
        d3d12_diag("%s: ViewInstanceCount %u without locations", api,
                   src.view_instances);
        return E_INVALIDARG;
    }
    const auto& l = layout::D3D12_VIEW_INSTANCE_LOCATION[m_is_64bit];
    for (u32 v = 0; v < src.view_instances; ++v) {
        const GuestAddr at = src.view_locations + v * l.size;
        const u32 viewport = read_u32(at + l.ViewportArrayIndex);
        const u32 slice = read_u32(at + l.RenderTargetArrayIndex);
        pso.view_locations[v] = {viewport, slice};
    }
    pso.view_instance_count = src.view_instances;
    pso.view_instancing_flags = src.view_flags;
    return S_OK;
}

namespace {

/// True when the (lowered, SM 5.1 + Vitreo extension) shader @p bytes reads
/// SV_ViewID: its instruction stream holds dxbc_vitreo_ext::kOpViewId.
[[nodiscard]] bool reads_view_id(const std::vector<u8>& bytes) {
    if (bytes.empty()) return false;
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    const std::vector<u8>& code = container.bytecode;
    const usize words = code.size() / 4;
    const auto word = [&](usize i) {
        u32 w = 0;
        std::memcpy(&w, code.data() + i * 4, 4);
        return w;
    };
    if (words < 2) return false;
    const usize end = std::min<usize>(words, word(1));
    for (usize at = 2; at < end;) {
        const u32 token = word(at);
        const u32 opcode = token & 0x7FFu;
        if (opcode == graphics::dxbc_vitreo_ext::kOpViewId) return true;
        // D3D10_SB_OPCODE_CUSTOMDATA (35): the length is the next token.
        u32 length = opcode == 35u ? (at + 1 < end ? word(at + 1) : 0u)
                                   : ((token >> 24) & 0x7Fu);
        if (length == 0) return false;  // malformed: stop scanning
        at += length;
    }
    return false;
}

}  // namespace

/// ViewInstancingTier 3 (DirectX-Specs "View Instancing", tier table: "The
/// implementation of view instancing always occurs at the first shader
/// stage that references SV_ViewID or the rasterizer, whichever is
/// earlier"): which stage of @p src that is. The input assembler never
/// depends on the view ("view instancing doesn't interact with the Input
/// Assembler"). An amplification shader has no SV_ViewID (DirectX-Specs
/// "Mesh Shader": "Amplification shaders do not support SV_ViewID input"),
/// so one that somehow reads it loops the whole dispatch.
u32 Frontend::view_instancing_split(const PipelineSource& src, bool mesh) {
    const auto reads = [&](GuestAddr bytecode) {
        if (bytecode == 0 || !shader_present(bytecode)) return false;
        std::vector<u8> bytes;
        if (read_shader_bytes(bytecode, bytes) != S_OK) return true;  // unknown: loop
        return reads_view_id(bytes);
    };
    if (mesh) {
        if (reads(src.as)) return kViLoop;
        if (reads(src.ms)) return kViMesh;
        return kViMeshRaster;
    }
    if (reads(src.vs)) return kViLoop;
    if (reads(src.hs)) return kViHull;
    if (reads(src.ds)) return kViDomain;
    if (reads(src.gs)) return kViGeometry;
    return kViRaster;
}

/// View instancing by draw-level looping (ViewInstancingTier 2): view 0 is
/// @p pso itself; views 1.. are pipelines of the same description whose
/// shaders were translated with their SV_ViewID. Replay draws each enabled
/// view with its location applied (Replayer::draw).
u32 Frontend::build_view_pipelines(const PipelineSource& src,
                                   PipelineState& pso, const char* api) {
    for (u32 v = 1; v < pso.view_instance_count; ++v) {
        auto view = std::make_unique<PipelineState>();
        view->compute = pso.compute;
        view->graphics = pso.graphics;
        view->flags = pso.flags;
        view->node_mask = pso.node_mask;
        view->view_id = v;
        PipelineSource one = src;
        one.view_instances = 0;
        one.view_instancing_present = false;
        one.cached_blob = 0;
        one.cached_size = 0;
        const u32 saved = build_scratch().view_id;
        build_scratch().view_id = v;
        const u32 error = build_pipeline(one, *view, api);
        build_scratch().view_id = saved;
        if (error != S_OK) return error;
        pso.views[v] = std::move(view);
    }
    return S_OK;
}

Frontend::BuildScratch& Frontend::build_scratch() {
    thread_local BuildScratch scratch;
    return scratch;
}

void Frontend::resolve_pipeline_root(PipelineSource& src) {
    src.root_resolved = true;
    src.root_live = false;
    src.root_layout.reset();
    src.root_blob_hash = 0;
    if (src.root == 0) return;
    if (const RootSignature* root =
            find<RootSignature>(src.root, Kind::RootSignature)) {
        src.root_live = true;
        src.root_layout = root->layout;
        src.root_blob_hash = root->blob_hash;
    }
}

void Frontend::precompile_draw_functions(PipelineState& pso) {
    // A graphics pipeline's Metal render pipeline is formed at its first
    // draw (pipeline_variant: Metal bakes the draw's vertex strides in),
    // under the frontend lock on the queue's thread. Its shader functions
    // do not depend on the strides: compile them here, on the creating
    // thread, so that draw finds them compiled. Only the pipelines
    // pipeline_variant draws with these exact sources -- no lowered stage,
    // no capture, no vertex specialisation (the same test as its loop).
    if (pso.compute || pso.mesh.active || pso.geometry.active ||
        pso.tessellation.active || pso.raster.active ||
        pso.stream_output.active || pso.vertex_cull.active ||
        !pso.per_vertex_msl.empty() || pso.view_instance_count > 1 ||
        pso.graphics.vertex_pulling || pso.vs.msl.empty()) {
        return;
    }
    for (const InputElement& element : pso.graphics.input_layout) {
        for (const auto& input : pso.vs.inputs) {
            if (input.semantic_index != element.semantic_index ||
                !semantic_equal(input.semantic, element.semantic)) {
                continue;
            }
            const auto format = d3d11::vertex_format_from_dxgi(element.format);
            const auto storage = d3d11::vertex_storage_component_type(element.format);
            const auto shader = input.component_type == 1u
                                    ? graphics::BackendVertexComponentType::UInt
                                : input.component_type == 2u
                                    ? graphics::BackendVertexComponentType::SInt
                                    : graphics::BackendVertexComponentType::Float;
            if (format == graphics::BackendVertexFormat::UInt1010102 ||
                storage != shader) {
                return;  // pipeline_variant translates the shader again
            }
            break;
        }
    }
    // The whole render pipeline, strides from the draw: the queue thread
    // then forms nothing at the first draw (Metal's pipeline-state compile
    // ran there, serially under the lock, about a quarter of a cold
    // creation). Only for targets a draw can bind: a description Windows
    // accepts may name a format no render-target or depth-stencil view can
    // have (a05 pso "RTVFormats[0] BC1", "DSVFormat RGBA8"), which is never
    // drawn and which Metal's pipeline validation aborts on.
    // D3D12_FORMAT_SUPPORT1_RENDER_TARGET 0x4000, _DEPTH_STENCIL 0x10000.
    bool drawable = true;
    for (u32 index = 0; index < pso.graphics.num_render_targets && index < 8; ++index) {
        const u32 format = pso.graphics.rtv_formats[index];
        if (format != DXGI_FORMAT_UNKNOWN &&
            (format_support(format, caps()).support1 & 0x4000u) == 0) {
            drawable = false;
        }
    }
    if (pso.graphics.dsv_format != DXGI_FORMAT_UNKNOWN &&
        (format_support(pso.graphics.dsv_format, caps()).support1 & 0x10000u) == 0) {
        drawable = false;
    }
    if (!drawable) return;
    if (auto pipeline = make_render_variant(pso, {}, false, true)) {
        pso.dynamic_variant = std::move(pipeline);
        return;
    }
    graphics::BackendPipelineDesc desc{};
    desc.vertex_shader_msl = pso.vs.msl;
    desc.fragment_shader_msl = pso.ps.present ? pso.ps.msl : std::string();
    desc.vertex_math_mode = pso.vs.math_mode;
    desc.fragment_math_mode = pso.ps.math_mode;
    desc.preserve_invariance = pso.vs.preserve_invariance;
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
    desc.input_primitive_class = primitive_class(pso.graphics.topology_type);
    m_backend.precompile_pipeline_functions(desc);
}

namespace {

/// The description's enumerations and SampleDesc, one refusal reason per
/// field. Checked before any shader is translated: a description Windows
/// refuses as invalid is not an UNIMPLEMENTED translation.
/// @p no_output_counts is OPTIONS19 SupportedSampleCountsWithNoOutputs.
[[nodiscard]] u32 graphics_desc_refusal(const GraphicsState& state, bool mesh,
                                        u32 no_output_counts, const char* api) {
    // One reason per field (measured, d3d12_a72_pso_target_sample_probe on
    // WARP and an RTX 4080 SUPER, x64 and x86, SM 5.0 and 6.0).
    if (state.num_render_targets > 8) {
        return refuse(E_INVALIDARG, "%s: NumRenderTargets %u is above 8", api,
                      state.num_render_targets);
    }
    // Measured (d3d12_a40_mesh_probe "pso"): a mesh pipeline takes any
    // topology type but PATCH, UNDEFINED (the stream default) included.
    // A graphics pipeline takes POINT, LINE, TRIANGLE and (with HS/DS)
    // PATCH; UNDEFINED and 5 are E_INVALIDARG (a72 pso).
    if (state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED && !mesh) {
        return refuse(E_INVALIDARG, "%s: PrimitiveTopologyType UNDEFINED without a mesh shader",
                      api);
    }
    if (state.topology_type > D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        return refuse(E_INVALIDARG, "%s: PrimitiveTopologyType %u is out of range", api,
                      state.topology_type);
    }
    // a72 pso: FillMode 2 (WIREFRAME) and 3 (SOLID) only; CullMode 1..3.
    if (state.fill_mode < D3D12_FILL_MODE_WIREFRAME || state.fill_mode > D3D12_FILL_MODE_SOLID) {
        return refuse(E_INVALIDARG, "%s: FillMode %u is out of range", api, state.fill_mode);
    }
    if (state.cull_mode < D3D12_CULL_MODE_NONE || state.cull_mode > D3D12_CULL_MODE_BACK) {
        return refuse(E_INVALIDARG, "%s: CullMode %u is out of range", api, state.cull_mode);
    }
    // SampleDesc. With a render-target or depth-stencil format, Count 0 is
    // E_INVALIDARG and the count and quality are checked against the first
    // format below. With no output format (NumRenderTargets 0 or every
    // format below it UNKNOWN, and no DSVFormat), the count is 0 or one of
    // OPTIONS19 SupportedSampleCountsWithNoOutputs (2^n for bit n), and the
    // quality is 0, or D3D12_STANDARD_MULTISAMPLE_PATTERN above one sample
    // (measured, a72 pso: {0,0} {1,0} {2,0} {4,0} {8,0} {16,0} and
    // {2..16,0xFFFFFFFF} S_OK; {3,0} {32,0} {33,0}, quality 1,
    // {0|1,0xFFFFFFFF} and every 0xFFFFFFFE E_INVALIDARG). Count 0 then
    // draws as one sample (a72 draw: coverage 0x1, as Count 1).
    {
        const bool outputs =
            !no_color_formats(state) || state.dsv_format != DXGI_FORMAT_UNKNOWN;
        const u32 count = state.sample_count;
        if (outputs && count == 0) {
            return refuse(E_INVALIDARG,
                          "%s: SampleDesc.Count 0 with a render-target or depth-stencil "
                          "format",
                          api);
        }
        if (!outputs) {
            const bool power = count != 0 && (count & (count - 1)) == 0 && count <= 16;
            const u32 bit = power ? static_cast<u32>(__builtin_ctz(count)) : 0u;
            if (count != 0 &&
                (!power || (no_output_counts & (1u << bit)) == 0)) {
                return refuse(E_INVALIDARG,
                              "%s: SampleDesc.Count %u with no output is not one of "
                              "SupportedSampleCountsWithNoOutputs 0x%x",
                              api, count, no_output_counts);
            }
            if (state.sample_quality != 0 &&
                !(state.sample_quality == 0xFFFFFFFFu && count > 1)) {
                return refuse(E_INVALIDARG,
                              "%s: SampleDesc {%u, 0x%x} with no output: the quality is "
                              "neither 0 nor the standard pattern",
                              api, count, state.sample_quality);
            }
        }
    }
    return S_OK;
}

}  // namespace

u32 Frontend::build_pipeline_unlocked(PipelineSource& src, PipelineState& pso,
                                      const char* api) {
    // (a) Under the lock: every frontend object the build reads. The rest of
    // the description stays in guest memory, which the frontend lock never
    // guarded -- the calling thread owns it for the call (Learn: the
    // description is an in-parameter), and other guest threads write guest
    // memory without the lock either way. The capabilities are made once,
    // then only read.
    resolve_pipeline_root(src);
    (void)caps();
    const GuestAddr device = pso.device;
    const u64 device_serial = [&] {
        const Device* owner = find<Device>(device, Kind::Device);
        return owner != nullptr ? owner->serial : 0;
    }();
    u32 built = S_OK;
    {
        // (b) Without it: DXIL lowering, DXBC -> SPIR-V -> MSL and the Metal
        // compiles (each synchronized on its own: lower_dxil's memo, the
        // backend's function cache, unimplemented's log; the per-build
        // translation parameters are per thread, build_scratch). A thread
        // that holds the lock more than once keeps it: its outer frames
        // still hold references into the frontend.
        FrontendUnlock unlocked(*this);
        const u32 running =
            m_builds_in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
        u32 peak = m_builds_in_flight_peak.load(std::memory_order_relaxed);
        while (running > peak &&
               !m_builds_in_flight_peak.compare_exchange_weak(
                   peak, running, std::memory_order_relaxed)) {
        }
        built = build_pipeline(src, pso, api);
        if (built == S_OK) precompile_draw_functions(pso);
        m_builds_in_flight.fetch_sub(1, std::memory_order_relaxed);
    }
    // (c) Under it again. The device the call came through must still be
    // there for the caller to publish a child of it.
    if (device_serial == 0 ||
        find_serial<Device>(device, device_serial, Kind::Device) == nullptr) {
        return built == S_OK
                   ? refuse(E_INVALIDARG, "%s: the device was released during the build", api)
                   : built;
    }
    return built;
}

u32 Frontend::build_pipeline(const PipelineSource& src, PipelineState& pso,
                             const char* api) {
    const bool graphics = src.vs != 0 || src.ps != 0 || src.as != 0 ||
                          src.ms != 0 || src.ds != 0 || src.hs != 0 ||
                          src.gs != 0;
    if (pso.compute == graphics) {
        // A compute pipeline takes only CS; a graphics one no CS.
        return refuse(E_INVALIDARG,
                      pso.compute ? "%s: a compute pipeline with graphics stages"
                                  : "%s: a graphics pipeline with no graphics stage",
                      api);
    }
    // Views 1.. (build_view_pipelines) keep view 0's split.
    struct ViSplitScope {
        u32& slot;
        u32 saved;
        ~ViSplitScope() { slot = saved; }
    } vi_scope{build_scratch().vi_first, build_scratch().vi_first};
    // Only this pipeline's mesh linkage splits its pixel shader's rows.
    struct PrimitiveSplitScope {
        std::array<u8, 32>& slot;
        std::array<u8, 32> saved;
        ~PrimitiveSplitScope() { slot = saved; }
    } split_scope{build_scratch().primitive_split, build_scratch().primitive_split};
    build_scratch().primitive_split = {};
    if (graphics) {
        if (u32 error = read_view_instancing(src, pso, api); error != S_OK) {
            return error;
        }
        if (pso.view_instance_count > 1) {
            const bool mesh_stages = (src.as != 0 && shader_present(src.as)) ||
                                     (src.ms != 0 && shader_present(src.ms));
            build_scratch().vi_first = view_instancing_split(src, mesh_stages);
        }
        pso.vi_first = build_scratch().view_id != 0 || pso.view_instance_count > 1
                           ? build_scratch().vi_first
                           : kViLoop;
    } else if (src.view_instances != 0) {
        // View instancing is a graphics-pipeline feature.
        d3d12_diag("%s: VIEW_INSTANCING on a compute pipeline", api);
        return E_INVALIDARG;
    }
    // D3D12_CACHED_PIPELINE_STATE: a blob GetCachedBlob handed out. Checked
    // once the pipeline is built (its digest is the description's).
    std::vector<u8> cached;
    if (src.cached_blob != 0 && src.cached_size != 0) {
        cached.resize(static_cast<usize>(std::min<u64>(src.cached_size, 64)));
        if (m_memory.read_bytes(src.cached_blob, cached.data(), cached.size())
                .is_err()) {
            return refuse(E_INVALIDARG, "%s: CachedPSO blob is unreadable", api);
        }
        if (src.cached_size != kCachedPsoBytes ||
            std::memcmp(cached.data(), kCachedPsoMagic, 8) != 0) {
            // Measured (a05 pso "CachedPSO garbage blob").
            return refuse(E_INVALIDARG, "%s: CachedPSO blob (%llu bytes) is not one "
                                        "GetCachedBlob made", api,
                          static_cast<unsigned long long>(src.cached_size));
        }
        u32 version = 0;
        std::memcpy(&version, cached.data() + 8, 4);
        if (version != kCachedPsoVersion) {
            // Learn: a blob from another driver version.
            return refuse(D3D12_ERROR_DRIVER_VERSION_MISMATCH,
                          "%s: CachedPSO blob version %u, this driver's %u", api, version,
                          kCachedPsoVersion);
        }
    }
    // Measured (a05 pso "NodeMask 2", "Flags TOOL_DEBUG"): one node; and
    // no flag -- TOOL_DEBUG needs the developer tools (not installed, as on
    // WARP), DYNAMIC_DEPTH_BIAS / DYNAMIC_INDEX_BUFFER_STRIP_CUT the
    // OPTIONS16 / OPTIONS15 features this device reports unsupported.
    if (pso.node_mask > 1 || pso.flags != 0) {
        d3d12_diag("%s: NodeMask %u / Flags 0x%x refused", api, pso.node_mask,
                   pso.flags);
        return E_INVALIDARG;
    }
    // The shaders' bytes, then the root signature -- the one given, else the
    // one embedded in a shader (an RTS0 part; Learn, "Specifying root
    // signatures in HLSL") -- then the translation, which a 5.1 shader's
    // unbounded arrays size from the root signature.
    std::vector<u8> first_bytes;
    std::vector<u8> second_bytes;
    // A mesh pipeline (d3d12_pipeline_mesh.cpp): its first shader is the
    // mesh shader.
    const bool mesh = graphics && ((src.as != 0 && shader_present(src.as)) ||
                                   (src.ms != 0 && shader_present(src.ms)));
    const char* first_stage = graphics ? (mesh ? "MS" : "VS") : "CS";
    if (u32 error = read_shader_bytes(
            graphics ? (mesh ? src.ms : src.vs) : src.cs, first_bytes);
        error != S_OK) {
        return refuse(error, "%s: the %s bytecode was refused", api, first_stage);
    }
    if (graphics && src.ps != 0) {
        if (u32 error = read_shader_bytes(src.ps, second_bytes); error != S_OK) {
            return refuse(error, "%s: the PS bytecode was refused", api);
        }
    }
    // Learn: a graphics pipeline needs a vertex shader, a compute one a CS.
    if (first_bytes.empty()) {
        return refuse(E_INVALIDARG, "%s: no %s bytecode", api, first_stage);
    }
    if (src.root != 0) {
        // Looked up under the frontend lock (resolve_pipeline_root): this
        // may run outside it.
        if (!src.root_resolved || !src.root_live) {
            return refuse(E_INVALIDARG,
                          "%s: pRootSignature 0x%llx is not a live ID3D12RootSignature", api,
                          static_cast<unsigned long long>(src.root));
        }
        pso.root = src.root_layout;
    } else {
#if VITREO_D3D12_ROOT_SIGNATURE_PARSER
        for (const std::vector<u8>* bytes : {&first_bytes, &second_bytes}) {
            if (bytes->empty() || !fourcc_present(*bytes, "RTS0")) continue;
            RootSignatureDesc embedded{};
            if (parse_root_signature_blob_for_device(bytes->data(),
                                                     bytes->size(),
                                                     embedded) == S_OK &&
                validate_root_signature_for_device(embedded) == S_OK) {
                pso.root = make_root_layout(embedded);
                break;
            }
        }
#endif
        if (!pso.root) {
            return refuse(E_INVALIDARG,
                          "%s: pRootSignature is NULL and no shader embeds a valid RTS0 "
                          "root signature",
                          api);
        }
    }
    if (graphics) {
        // Known before the mesh shader is translated: a raster-emulated
        // mesh pipeline captures its primitives for pass C
        // (MeshStageLayout::raster_capture).
        const bool pixel_shader = src.ps != 0 && shader_present(src.ps);
        if (u32 error = graphics_desc_refusal(pso.graphics, mesh,
                                              caps().sample_counts_with_no_outputs, api);
            error != S_OK) {
            return error;
        }
        if (!raster_state_valid(pso.graphics,
                                caps().conservative_rasterization_tier,
                                pixel_shader)) {
            const GraphicsState& g = pso.graphics;
            return refuse(E_INVALIDARG,
                          "%s: rasterizer state refused: ConservativeRaster %u (tier %u), "
                          "FillMode %u, topology type %u, PS %d, ForcedSampleCount %u, "
                          "DSVFormat %u, SampleDesc.Count %u",
                          api, g.conservative, caps().conservative_rasterization_tier,
                          g.fill_mode, g.topology_type, pixel_shader ? 1 : 0,
                          g.forced_sample_count, g.dsv_format, g.sample_count);
        }
        pso.raster.ps = raster_emulation_for(pso.graphics);
        pso.raster.active =
            pso.raster.ps.mode != graphics::RasterEmulationMode::Off;
        if (pso.raster.active && !mesh) {
            // The input assembler's primitive; a geometry or domain shader's
            // is known after the lowering (the pixel shader is translated
            // again then if it differs).
            float width = 1.0f;
            const u32 type = pso.graphics.topology_type;
            pso.raster.ps.primitive = raster_primitive_for(
                pso.graphics, pso.raster.ps.mode,
                type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT  ? 1u
                : type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? 2u
                                                             : 3u,
                width);
            pso.raster.primitive = pso.raster.ps.primitive;
            if (!pixel_shader &&
                pso.raster.primitive != graphics::RasterPrimitive::Triangle) {
                // Input-assembler lines or points with no pixel shader: a
                // forced sample count allows no depth-stencil view, so
                // nothing the rasterizer produces is observable (D3D11.3
                // 15.2: no pixel shader with depth and stencil off disables
                // rasterization; measured, d3d12_a66_c6_probe "crnops tir*":
                // CInvocations 0, PSInvocations 0), and conservative
                // rasterization defines no line or point coverage (the only
                // valid fill mode is SOLID, raster_state_valid; "crnops cr*"
                // count no clipper invocations on WARP either). The ordinary
                // rasterizer draws them: the same counts, and with a
                // depth-stencil view the depth of the plain line or point.
                pso.raster.active = false;
                pso.raster.ps.mode = graphics::RasterEmulationMode::Off;
            }
        }
        if (mesh) {
            if (u32 error = compile_mesh_stages(src, pso, std::move(first_bytes),
                                                second_bytes, api);
                error != S_OK) {
                return error;
            }
            // The pixel shader below reads its split rows apart.
            build_scratch().primitive_split = pso.mesh.ms_layout.ps_primitive_split;
        } else if (u32 error = compile_shader(std::move(first_bytes),
                                              graphics::ShaderStage::Vertex, pso.vs,
                                              api, pso.root.get(),
                                              D3D12_SHADER_VISIBILITY_VERTEX);
                   error != S_OK) {
            return error;
        }
        LogicOpVariant logic{};
        if (u32 error = logic_op_variant(pso.graphics, logic, api);
            error != S_OK) {
            return error;
        }
        // Lines straight from the input assembler (a geometry or domain
        // shader decides its own primitive); a pixel shader behind a mesh
        // shader reads its SV_PrimitiveID from the mesh shader.
        ShaderVariantOptions fragment_variant{};
        fragment_variant.primitive_id_varying = mesh;
        fragment_variant.color_output_mask = color_output_mask(pso.graphics);
        fragment_variant.line_barycentrics =
            pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
            !(src.gs != 0 && shader_present(src.gs)) && !mesh;
        if (u32 error = compile_shader(
                std::move(second_bytes), graphics::ShaderStage::Fragment,
                pso.ps, api, pso.root.get(), D3D12_SHADER_VISIBILITY_PIXEL,
                &logic, &fragment_variant,
                pso.raster.active ? &pso.raster.ps : nullptr);
            error != S_OK) {
            return error;
        }
        // The pixel shader's inputs left no room for pass C's edge data
        // (raster_fragment_components): pass C writes it to the edge buffer.
        pso.raster.ps.edges_in_buffer = pso.ps.raster_edges_in_buffer;
        for (const auto& [shader, visibility, label] :
             {std::tuple{&pso.vs, D3D12_SHADER_VISIBILITY_VERTEX, "VS"},
              std::tuple{&pso.ps, D3D12_SHADER_VISIBILITY_PIXEL, "PS"},
              std::tuple{&pso.mesh.ms, D3D12_SHADER_VISIBILITY_MESH, "MS"},
              std::tuple{&pso.mesh.as, D3D12_SHADER_VISIBILITY_AMPLIFICATION, "AS"}}) {
            if (!root_covers(*pso.root, *shader, visibility)) {
                return refuse(E_INVALIDARG,
                              "%s: the root signature does not cover the %s's bindings", api,
                              label);
            }
        }
        // Stream output is known before the geometry lowering so a
        // D3D12_SO_NO_RASTERIZED_STREAM pipeline is not refused for lacking
        // an SV_Position the rasterizer will never read.
        if (src.stream_output_entries != 0 && !mesh) {
            if (u32 error = accept_stream_output(src, pso, api); error != S_OK) {
                return error;
            }
        }
        if (mesh && pso.ps.per_vertex_inputs != 0) {
            // GetAttributeAtVertex behind a mesh shader: the registers are
            // known once the pixel shader is translated; the mesh shader's
            // epilogue then writes each primitive's per-vertex copies
            // (MeshStageLayout::per_vertex_registers).
            pso.mesh.ms_layout.per_vertex_registers = pso.ps.per_vertex_inputs;
            CompiledShader ms;
            if (u32 error = compile_shader(pso.mesh.ms.bytecode, graphics::ShaderStage::Mesh,
                                           ms, api, pso.root.get(),
                                           D3D12_SHADER_VISIBILITY_MESH, nullptr, nullptr,
                                           nullptr, &pso.mesh.ms_layout,
                                           pso.mesh.payload_words);
                error != S_OK) {
                return error;
            }
            pso.mesh.ms = std::move(ms);
        }
        if (!mesh) {
            if (u32 error = lower_shader_stages(src, pso, api); error != S_OK) {
                return error;
            }
        }
        // Lines from a geometry shader (line strip output) or a domain
        // shader (isoline) are known only after the lowering: SV_Barycentrics'
        // third weight is then "guaranteed to be exactly 0.0" too (DXC wiki
        // SV_Barycentrics), so the pixel shader is translated again with it.
        const bool lowered_lines =
            (pso.geometry.active && pso.geometry.output_topology == 3u) ||
            (!pso.geometry.active && pso.tessellation.active &&
             pso.tessellation.output_primitive == 2u);
        const bool bary_lines = lowered_lines && !fragment_variant.line_barycentrics &&
                                pso.ps.present && reads_barycentrics(pso.ps.bytecode);
        // Raster emulation of the primitive the lowering found (a geometry
        // shader's lines, a domain shader's points): the pixel shader's
        // coverage test is that primitive's.
        const bool raster_primitive =
            pso.raster.active && pso.raster.primitive != pso.raster.ps.primitive;
        pso.raster.ps.primitive = pso.raster.primitive;
        if ((bary_lines || raster_primitive) && pso.ps.present) {
            if (bary_lines) fragment_variant.line_barycentrics = true;
            CompiledShader ps;
            if (u32 error = compile_shader(
                    pso.ps.bytecode, graphics::ShaderStage::Fragment, ps, api,
                    pso.root.get(), D3D12_SHADER_VISIBILITY_PIXEL, &logic,
                    &fragment_variant, pso.raster.active ? &pso.raster.ps : nullptr);
                error != S_OK) {
                return error;
            }
            pso.ps = std::move(ps);
        }
    } else {
        if (u32 error = compile_shader(std::move(first_bytes),
                                       graphics::ShaderStage::Compute, pso.cs,
                                       api, pso.root.get(),
                                       D3D12_SHADER_VISIBILITY_ALL);
            error != S_OK) {
            return error;
        }
        if (!root_covers(*pso.root, pso.cs, D3D12_SHADER_VISIBILITY_ALL)) {
            return refuse(E_INVALIDARG,
                          "%s: the root signature does not cover the CS's bindings", api);
        }
    }
    const u32 built = graphics ? finish_graphics_pipeline(pso, api)
                               : finish_compute_pipeline(pso, api);
    if (built != S_OK) return built;
    if (u32 error = build_view_pipelines(src, pso, api); error != S_OK) {
        return error;
    }
    pso.cache_digest = pipeline_digest(pso, src);
    if (!cached.empty()) {
        u64 digest = 0;
        std::memcpy(&digest, cached.data() + 16, 8);
        // Measured (a05 pso "CachedPSO from GetCachedBlob, different desc").
        if (digest != pso.cache_digest) {
            return refuse(E_INVALIDARG,
                          "%s: CachedPSO blob was made for another description", api);
        }
    }
    return S_OK;
}

u64 Frontend::pipeline_digest(const PipelineState& pso, const PipelineSource& src) {
    u64 h = fnv1a(&pso.compute, sizeof(pso.compute));
    for (const CompiledShader* shader :
         {&pso.vs, &pso.ps, &pso.cs, &pso.geometry.kernel,
          &pso.tessellation.hull, &pso.tessellation.domain_stage,
          &pso.mesh.as, &pso.mesh.ms}) {
        const u64 size = shader->bytecode.size();
        h = fnv1a(&size, sizeof(size), h);
        h = fnv1a(shader->bytecode.data(), shader->bytecode.size(), h);
    }
    if (src.root_live) {
        h = fnv1a(&src.root_blob_hash, sizeof(src.root_blob_hash), h);
    }
    h = fnv1a(&pso.node_mask, sizeof(pso.node_mask), h);
    h = fnv1a(&pso.flags, sizeof(pso.flags), h);
    if (pso.compute) return h;
    const GraphicsState& g = pso.graphics;
    const auto mix = [&h](const auto& value) { h = fnv1a(&value, sizeof(value), h); };
    mix(g.alpha_to_coverage);
    mix(g.independent_blend);
    for (const auto& b : g.blend) {
        mix(b.blend_enable); mix(b.logic_op_enable); mix(b.src_blend);
        mix(b.dest_blend); mix(b.blend_op); mix(b.src_blend_alpha);
        mix(b.dest_blend_alpha); mix(b.blend_op_alpha); mix(b.logic_op);
        mix(b.write_mask);
    }
    mix(g.sample_mask); mix(g.fill_mode); mix(g.cull_mode); mix(g.front_ccw);
    mix(g.depth_bias); mix(g.depth_bias_clamp); mix(g.slope_scaled_depth_bias);
    mix(g.depth_clip); mix(g.multisample); mix(g.antialiased_line); mix(g.narrow_quad_line);
    mix(g.forced_sample_count); mix(g.conservative); mix(g.depth_enable);
    mix(g.depth_write_mask); mix(g.depth_func); mix(g.stencil_enable);
    mix(g.stencil_read_mask); mix(g.stencil_write_mask); mix(g.front);
    mix(g.back); mix(g.depth_bounds_test);
    for (const auto& e : g.input_layout) {
        h = fnv1a(e.semantic.data(), e.semantic.size(), h);
        mix(e.semantic_index); mix(e.format); mix(e.input_slot);
        mix(e.aligned_offset); mix(e.slot_class); mix(e.step_rate);
    }
    mix(g.strip_cut); mix(g.topology_type); mix(g.num_render_targets);
    mix(g.rtv_formats); mix(g.dsv_format); mix(g.sample_count);
    mix(g.sample_quality);
    mix(pso.stream_output.active);
    mix(pso.stream_output.rasterized_stream);
    mix(pso.stream_output.strides);
    mix(pso.stream_output.stream_slots);
    for (const auto& entry : pso.stream_output.entries) {
        mix(entry.stream);
        mix(entry.slot);
        mix(entry.start_component);
        mix(entry.component_count);
        mix(entry.dst_dword);
        mix(entry.output_register);
        mix(entry.gap);
    }
    return h;
}

void Frontend::create_graphics_pipeline_state(
    Marshaler& m, Device& device, usize first,
    std::unique_ptr<PipelineState>* capture) {
    // CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
    //     REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device::CreateGraphicsPipelineState";
    const GuestAddr desc = m.arg_ptr(first);
    const GuestAddr out = m.arg_ptr(first + 2);
    PipelineSource src{};
    PipelineRefusalScope report(*this, m, api);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(refuse(E_INVALIDARG, "%s: pDesc is NULL", api));
        return;
    }
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[m_is_64bit];
    report.desc = desc;
    report.desc_size = l.size;
    report.src = &src;
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    GraphicsState& state = pso->graphics;
    (void)read_pointer(desc + l.pRootSignature, src.root);
    src.vs = desc + l.VS;
    src.ps = desc + l.PS;
    src.ds = desc + l.DS;
    src.hs = desc + l.HS;
    src.gs = desc + l.GS;
    (void)read_pointer(desc + l.StreamOutput_pSODeclaration, src.so_declaration);
    src.stream_output_entries = read_u32(desc + l.StreamOutput_NumEntries);
    (void)read_pointer(desc + l.StreamOutput_pBufferStrides, src.so_strides);
    src.so_num_strides = read_u32(desc + l.StreamOutput_NumStrides);
    src.so_rasterized_stream = read_u32(desc + l.StreamOutput_RasterizedStream);
    read_blend(*this, desc + l.BlendState, state);
    state.sample_mask = read_u32(desc + l.SampleMask);
    read_rasterizer(*this, desc + l.RasterizerState, state);
    read_depth_stencil(*this, desc + l.DepthStencilState, false, state);
    GuestAddr elements = 0;
    (void)read_pointer(desc + l.InputLayout_pInputElementDescs, elements);
    report.input_elements = elements;
    report.input_element_count = read_u32(desc + l.InputLayout_NumElements);
    if (u32 error = read_input_layout(
            m, elements, read_u32(desc + l.InputLayout_NumElements), state);
        error != S_OK) {
        m.set_return(error);
        return;
    }
    state.strip_cut = read_u32(desc + l.IBStripCutValue);
    state.topology_type = read_u32(desc + l.PrimitiveTopologyType);
    state.num_render_targets = read_u32(desc + l.NumRenderTargets);
    for (u32 index = 0; index < 8; ++index) {
        state.rtv_formats[index] = read_u32(desc + l.RTVFormats + index * 4);
    }
    state.dsv_format = read_u32(desc + l.DSVFormat);
    state.sample_count = read_u32(desc + l.SampleDesc_Count);
    state.sample_quality = read_u32(desc + l.SampleDesc_Quality);
    pso->node_mask = read_u32(desc + l.NodeMask);
    (void)read_pointer(desc + l.CachedPSO_pCachedBlob, src.cached_blob);
    (void)read_pointer(desc + l.CachedPSO_CachedBlobSizeInBytes,
                       src.cached_size);
    pso->flags = read_u32(desc + l.Flags);
    if (u32 error = build_pipeline_unlocked(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (capture != nullptr) {
        // ID3D12PipelineLibrary's Load*: the library decides what to hand
        // out.
        *capture = std::move(pso);
        m.set_return(S_OK);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(first + 1), out));
}

namespace {

/// Each component's bits of a renderable UINT format (the D3D11 frontend's
/// uint_component_masks); false for any other format.
bool uint_component_masks(graphics::PixelFormat format,
                          std::array<u32, 4>& masks) {
    using graphics::PixelFormat;
    const auto set = [&masks](u32 r, u32 g, u32 b, u32 a) {
        masks = {r, g, b, a};
        return true;
    };
    switch (format) {
    case PixelFormat::R8Uint: return set(0xFFu, 0, 0, 0);
    case PixelFormat::RG8Uint: return set(0xFFu, 0xFFu, 0, 0);
    case PixelFormat::RGBA8Uint: return set(0xFFu, 0xFFu, 0xFFu, 0xFFu);
    case PixelFormat::R16Uint: return set(0xFFFFu, 0, 0, 0);
    case PixelFormat::RG16Uint: return set(0xFFFFu, 0xFFFFu, 0, 0);
    case PixelFormat::RGBA16Uint: return set(0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu);
    case PixelFormat::R32Uint: return set(~0u, 0, 0, 0);
    case PixelFormat::RG32Uint: return set(~0u, ~0u, 0, 0);
    case PixelFormat::RGBA32Uint: return set(~0u, ~0u, ~0u, ~0u);
    case PixelFormat::RGB10A2Uint: return set(0x3FFu, 0x3FFu, 0x3FFu, 0x3u);
    default: masks = {}; return false;
    }
}

}  // namespace

u32 Frontend::logic_op_variant(const GraphicsState& state, LogicOpVariant& out,
                               const char* api) {
    // D3D12_RENDER_TARGET_BLEND_DESC::LogicOp is that target's operation.
    // Learn (D3D12_BLEND_DESC::IndependentBlendEnable): false uses only
    // RenderTarget[0], so every bound UINT target shares that one op. True
    // uses each target's description, so each bound UINT target keeps the
    // LogicOp in its own description. A logic op excludes blending on that
    // target (measured, a05 pso "LogicOp and BlendEnable both") and applies
    // only to a renderable UINT format (D3D11.3 17.7.1). Metal has no logic
    // ops: the pixel shader performs that target's op through framebuffer
    // fetch.
    out = LogicOpVariant{};
    bool any = false;
    for (u32 index = 0; index < 8; ++index) {
        const RenderTargetBlend& b =
            state.independent_blend ? state.blend[index] : state.blend[0];
        if (!b.logic_op_enable) continue;
        if (b.blend_enable || b.logic_op > D3D12_LOGIC_OP_OR_INVERTED) {
            return refuse(E_INVALIDARG,
                          "%s: RenderTarget[%u] LogicOpEnable with %s", api,
                          state.independent_blend ? index : 0u,
                          b.blend_enable ? "BlendEnable" : "an invalid LogicOp");
        }
        if (index >= state.num_render_targets ||
            state.rtv_formats[index] == DXGI_FORMAT_UNKNOWN) {
            continue;
        }
        if (!uint_component_masks(
                graphics::pixel_format_from_dxgi(state.rtv_formats[index]),
                out.component_masks[index])) {
            return refuse(E_INVALIDARG,
                          "%s: a logic op on RTVFormats[%u] %u, which is not a renderable "
                          "UINT format",
                          api, index, state.rtv_formats[index]);
        }
        if (!caps().logic_ops) {
            unimplemented(api, "logic ops without framebuffer fetch");
            return E_INVALIDARG;
        }
        if (any && b.logic_op != out.op) out.per_target = true;
        if (!any) out.op = b.logic_op;
        any = true;
        out.ops[index] = b.logic_op;
        out.targets |= 1u << index;
    }
    return S_OK;
}

namespace {

void apply_color_state(const GraphicsState& state,
                       graphics::BackendPipelineDesc& desc) {
    for (u32 index = 0; index < 8; ++index) {
        desc.color_formats[index] =
            index < state.num_render_targets
                ? graphics::pixel_format_from_dxgi(state.rtv_formats[index])
                : graphics::PixelFormat::Unknown;
    }
    desc.color_format = desc.color_formats[0];
    desc.depth_format =
        state.dsv_format != DXGI_FORMAT_UNKNOWN
            ? texture_format_for(state.dsv_format,
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            : graphics::PixelFormat::Unknown;
    desc.depth_attachment = state.dsv_format != DXGI_FORMAT_UNKNOWN;
    desc.sample_count = std::max(1u, state.sample_count);
    desc.alpha_to_coverage = state.alpha_to_coverage;
    desc.attachmentless = no_color_formats(state) &&
                          state.dsv_format == DXGI_FORMAT_UNKNOWN;
    desc.independent_blend_enabled = state.independent_blend;
    for (u32 index = 0; index < 8; ++index) {
        const RenderTargetBlend& b =
            state.blend[state.independent_blend ? index : 0];
        desc.blend_enabled_targets[index] = b.blend_enable;
        desc.source_blends[index] = blend_factor(b.src_blend);
        desc.destination_blends[index] = blend_factor(b.dest_blend);
        desc.blend_operations[index] = blend_op(b.blend_op);
        desc.source_blends_alpha[index] = blend_factor(b.src_blend_alpha);
        desc.destination_blends_alpha[index] = blend_factor(b.dest_blend_alpha);
        desc.blend_operations_alpha[index] = blend_op(b.blend_op_alpha);
        desc.color_write_masks[index] = b.write_mask & 0xFu;
    }
    desc.blend_enabled = desc.blend_enabled_targets[0];
    desc.source_blend = desc.source_blends[0];
    desc.destination_blend = desc.destination_blends[0];
    desc.blend_operation = desc.blend_operations[0];
    desc.source_blend_alpha = desc.source_blends_alpha[0];
    desc.destination_blend_alpha = desc.destination_blends_alpha[0];
    desc.blend_operation_alpha = desc.blend_operations_alpha[0];
    desc.color_write_mask = desc.color_write_masks[0];
}

struct FlatCompiled {
    u32 hr{S_OK};
    /// Set when hr is E_NOTIMPL: the UNIMPLEMENTED case.
    /// The translator's own reason when the emulation cannot lower the
    /// shader (empty for none): logged as the UNIMPLEMENTED case.
    std::string unimplemented_case;
    graphics::DxbcToSpirvResult spirv;
};

void guard_flat_stage(const graphics::DxbcContainer& container,
                      graphics::DxbcTranslationOptions& options) {
    if (table_translation(container)) return;
    options.guard_constant_buffers = true;
    options.guard_buffer_bounds = true;
    options.typed_buffer_bias = true;
}

FlatCompiled compile_flat(std::vector<u8> bytecode, graphics::ShaderStage stage,
                          graphics::DxbcTranslationOptions options,
                          CompiledShader& out, const char* api,
                          const RootLayout* root) {
    FlatCompiled built;
    out = CompiledShader{};
    out.bytecode = std::move(bytecode);
    if (fourcc_present(out.bytecode, "DXIL")) {
        unimplemented(api, "DXIL");
        built.hr = E_INVALIDARG;
        return built;
    }
    u32 total_size = 0;
    if (out.bytecode.size() >= 32) {
        std::memcpy(&total_size, out.bytecode.data() + 24, 4);
    }
    if (out.bytecode.size() < 32 ||
        std::memcmp(out.bytecode.data(), "DXBC", 4) != 0 ||
        total_size > out.bytecode.size()) {
        d3d12_diag("%s: not a whole DXBC container", api);
        built.hr = E_INVALIDARG;
        return built;
    }
    if (!graphics::dxbc_container_verified(out.bytecode.data(),
                                           out.bytecode.size())) {
        d3d12_diag("%s: DXBC container hash mismatch", api);
        built.hr = E_FAIL;
        return built;
    }
    const auto container =
        graphics::parse_dxbc(out.bytecode.data(), out.bytecode.size());
    if (!container.valid() || !container.is_sm4_plus() ||
        container.stage != stage) {
        d3d12_diag("%s: bytecode is not a DXBC shader of the stage", api);
        built.hr = E_INVALIDARG;
        return built;
    }
    out.declarations = container.binding_declarations;
    const bool tables = needs_descriptor_tables(container);
    if (tables) {
        if (u32 error = configure_descriptor_bindings(container, root, out,
                                                      options, api);
            error != S_OK) {
            built.hr = error;
            return built;
        }
    }
    options.uavs_in_every_stage = true;  // D3D12: UAVs in all stages
    options.typed_uav_uint_views = true;  // Replayer::bind_view
    built.spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!built.spirv.valid()) {
        // A program the spec calls invalid is the caller's error; anything
        // else is a gap of the emulation, named by the translator.
        if (built.spirv.invalid_program) {
            d3d12_diag("%s: invalid shader program: %s", api,
                       built.spirv.failure.c_str());
            built.hr = E_INVALIDARG;
            return built;
        }
        built.hr = E_NOTIMPL;
        built.unimplemented_case = built.spirv.failure.empty()
                                       ? std::string("the shader translation failed")
                                       : built.spirv.failure;
        return built;
    }
    out.uav_counter_registers = built.spirv.uav_counter_registers;
    out.uav_counter_spaces = built.spirv.uav_counter_spaces;
    out.typed_uav_atomics = built.spirv.uses_typed_uav_atomics;
    if (!note_uint_view_uavs(out, built.spirv, options, api)) {
        built.hr = E_NOTIMPL;
        built.unimplemented_case =
            "an atomic on a float typed UAV in a descriptor range the shader "
            "reads from the heap in place";
        return built;
    }
    out.cb_values.assign(out.declarations.size(), 0u);
    for (usize index = 0; index < out.declarations.size(); ++index) {
        const auto& declaration = out.declarations[index];
        if (declaration.register_class !=
                graphics::DxbcRegisterClass::ConstantBuffer ||
            declaration.name.empty()) {
            continue;
        }
        for (const auto& cb : container.constant_buffers) {
            if (cb.name != declaration.name) continue;
            u32 end = 0;
            for (const auto& variable : cb.variables) {
                end = std::max(end, variable.offset + variable.size);
            }
            out.cb_values[index] = (end + 3u) / 4u;
        }
    }
    const graphics::ShaderStage msl_stage =
        built.spirv.stage == graphics::ShaderStage::Compute
            ? graphics::ShaderStage::Compute
            : stage;
    graphics::MslCompileOptions msl{};
    if (tables) {
        finish_register_counts(out, built.spirv, options);
        argument_buffer_options(out, msl);
    }
    if (built.spirv.uses_typed_uav_atomics) msl.msl_version = 30100u;
    msl.precise_without_helpers = true;
    msl.fp_contract_off = false;
    const auto compiled = graphics::compile_spirv_to_msl(
        built.spirv.spirv.data(), built.spirv.spirv.size(), msl_stage, msl);
    if (!compiled.valid()) {
        built.hr = E_NOTIMPL;
        built.unimplemented_case =
            std::string("SPIRV-Cross could not emit MSL for the lowered ") +
            (stage == graphics::ShaderStage::Hull     ? "hull shader"
             : stage == graphics::ShaderStage::Domain ? "domain shader"
             : stage == graphics::ShaderStage::Geometry
                 ? "geometry shader"
                 : "vertex capture") +
            (compiled.failure.empty() ? std::string() : ": " + compiled.failure);
        return built;
    }
    out.present = true;
    out.msl = compiled.msl_text();
    if (tables) {
        retarget_lowered_buffers(out.msl,
                                 options.capture_vertex_outputs_to_buffer);
    }
    out.threads_x = std::max(1u, compiled.workgroup_size_x);
    out.threads_y = std::max(1u, compiled.workgroup_size_y);
    out.threads_z = std::max(1u, compiled.workgroup_size_z);
    const auto contract = graphics::dxbc_math_contract(container);
    out.math_mode = !contract.refactoring_allowed || contract.uses_precise
                        ? graphics::BackendMathMode::Safe
                        : graphics::BackendMathMode::Relaxed;
    if (!tables) {
        out.registers.cbv_slots =
            register_high_water(built.spirv.cbuffer_bindings);
        out.registers.srv_slots =
            register_high_water(built.spirv.texture_bindings);
        out.registers.sampler_slots =
            register_high_water(built.spirv.sampler_bindings);
    }
    return built;
}

/// What a raster-emulation pass C needs besides the record layout.
struct RasterPassRequest {
    graphics::RasterEmulationSource source{};
    std::array<graphics::RasterVarying, 32> modes{};
    /// source.edges_in_buffer: where each pass-C vertex MSL's edge kernel
    /// goes (PipelineState::RasterProgram::edge_kernel_msl).
    std::unordered_map<std::string, std::string>* edge_kernels{nullptr};
};


/// The pixel shader's interpolation of each register, as pass C places it.
[[nodiscard]] std::array<graphics::RasterVarying, 32> raster_varyings(
    const CompiledShader& ps) {
    std::array<graphics::RasterVarying, 32> modes{};
    for (usize reg = 0; reg < modes.size(); ++reg) {
        const u8 m = ps.input_interpolation[reg];
        modes[reg] = m == 1u ? graphics::RasterVarying::Constant
                     : (m == 4u || m == 5u || m == 7u)
                         ? graphics::RasterVarying::NoPerspective
                         : graphics::RasterVarying::Perspective;
    }
    return modes;
}

/// Pass-through vertex MSL for a geometry or domain output signature.
/// Empty when the rasterized stream has no SV_Position. @p raster_stream
/// is 0 for a domain shader. With @p raster, the raster-emulation pass C
/// over the same records instead.
///
/// SV_CullDistance needs each primitive's vertices together: a geometry
/// expansion has them; a domain shader's dense records are then drawn
/// NON-indexed, the pass-through reading the tessellator's index list at t1
/// (@p through_index_list reports it; @p primitive_vertices is the
/// tessellator's output primitive).
[[nodiscard]] std::string passthrough_for(
    const graphics::DxbcContainer& container, u32 raster_stream,
    u32 vertices_per_invocation, bool dense, bool& writes_viewport,
    const char*& refusal, u32 primitive_vertices = 0,
    bool* through_index_list = nullptr,
    const RasterPassRequest* raster = nullptr, u32 per_vertex = 0,
    bool force_index_list = false) {
    writes_viewport = false;
    refusal = nullptr;
    bool culls = false;
    std::vector<graphics::GsPassthroughOutput> outputs;
    for (const auto& element : container.output_signature) {
        const u32 sv = element.system_value_type;
        const bool delivered = element.stream == raster_stream;
        culls |= delivered && sv == D3D_NAME_CULL_DISTANCE;
        auto same = std::find_if(outputs.begin(), outputs.end(),
                                 [&](const graphics::GsPassthroughOutput& o) {
                                     return o.register_num == element.register_num;
                                 });
        if (same == outputs.end()) {
            graphics::GsPassthroughOutput output{};
            output.register_num = element.register_num;
            output.component_type = element.component_type;
            output.delivered = false;
            outputs.push_back(output);
            same = std::prev(outputs.end());
        }
        if (!delivered) continue;
        if (!same->delivered) same->component_type = element.component_type;
        same->delivered = true;
        writes_viewport |= sv == D3D_NAME_VIEWPORT_ARRAY_INDEX;
        if (sv == D3D_NAME_POSITION) same->is_position = true;
        if (sv == D3D_NAME_CLIP_DISTANCE || sv == D3D_NAME_CULL_DISTANCE ||
            sv == D3D_NAME_RENDER_TARGET_ARRAY_INDEX ||
            sv == D3D_NAME_VIEWPORT_ARRAY_INDEX) {
            for (u32 component = 0; component < 4; ++component) {
                if ((element.component_mask & (1u << component)) != 0u) {
                    same->component_system_values[component] =
                        static_cast<u8>(sv);
                }
            }
        }
    }
    if (through_index_list != nullptr) *through_index_list = false;
    if (raster != nullptr) {
        // Pass C sees each triangle whole but does not evaluate
        // SV_CullDistance; refuse rather than rasterize a culled primitive.
        if (culls) {
            refusal = "SV_CullDistance with the raster-emulation pass "
                      "(conservative or target-independent rasterization)";
            return {};
        }
        // Pass C reads each primitive's records together: GetAttributeAtVertex
        // copies come from them in D3D vertex order.
        graphics::RasterEmulationSource source = raster->source;
        source.per_vertex_registers = per_vertex;
        std::string msl = graphics::generate_raster_emulation_vertex_msl(
            outputs, source, raster->modes);
        if (!msl.empty() && source.edges_in_buffer && raster->edge_kernels != nullptr) {
            (*raster->edge_kernels)[msl] =
                graphics::generate_raster_emulation_edge_kernel_msl(outputs, source,
                                                                    raster->modes);
        }
        return msl;
    }
    const bool index_list = dense && (culls || per_vertex != 0 || force_index_list);
    if (through_index_list != nullptr) *through_index_list = index_list;
    return graphics::generate_gs_passthrough_vertex_msl(
        outputs, vertices_per_invocation, /*fixed_function_push=*/true, dense,
        primitive_vertices, index_list, per_vertex);
}


}  // namespace

/// D3D11.3 (Primitive Topologies) / Learn ("Primitive Topologies"): the
/// adjacent vertices of an adjacency topology are visible only to a
/// geometry shader; with none the primitive draws without them. Every
/// vertex still runs the vertex shader (measured, d3d12_a66_c6_probe
/// "adj *": VSInvocations and vs_adj's own count 12 of 12). The draw
/// captures the vertex shader and draws this pass-through over the
/// non-adjacent vertices' record ids (Replayer::draw_lowered).
u32 Frontend::ensure_vertex_replay(PipelineState& pso) {
    constexpr const char* api = "ExecuteCommandLists";
    if (!pso.adjacency_msl.empty()) return S_OK;
    if (pso.adjacency_tried) return E_NOTIMPL;
    pso.adjacency_tried = true;
    const u32 vertices =
        pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE       ? 2u
        : pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ? 3u
                                                                               : 0u;
    if (vertices == 0) return E_INVALIDARG;
    struct ViewIdScope {
        u32& value;
        u32 saved;
        ~ViewIdScope() { value = saved; }
    } scope{build_scratch().view_id, build_scratch().view_id};
    build_scratch().view_id = pso.view_id;
    const auto container = graphics::parse_dxbc(pso.vs.bytecode.data(), pso.vs.bytecode.size());
    if (pso.capture_msl.empty()) {
        graphics::DxbcTranslationOptions options{};
        options.view_id = pso.view_id;
        guard_flat_stage(container, options);
        options.capture_vertex_outputs_to_buffer = true;
        CompiledShader captured;
        FlatCompiled built = compile_flat(pso.vs.bytecode, graphics::ShaderStage::Vertex,
                                          options, captured, api, pso.root.get());
        if (built.hr != S_OK) {
            if (!built.unimplemented_case.empty()) {
                unimplemented(api, built.unimplemented_case.c_str());
            }
            return built.hr;
        }
        if (!built.spirv.captures_vertex_outputs || built.spirv.capture_registers.empty()) {
            unimplemented(api, "an adjacency draw whose vertex shader cannot be captured");
            return E_NOTIMPL;
        }
        pso.capture_msl = std::move(captured.msl);
        pso.capture_registers = built.spirv.capture_registers;
        pso.capture_math = captured.math_mode;
    }
    bool viewport = false;
    bool through = false;
    const char* refusal = nullptr;
    std::string msl = passthrough_for(container, 0, 0, /*dense=*/true, viewport, refusal,
                                      vertices, &through, nullptr, 0,
                                      /*force_index_list=*/true);
    if (refusal != nullptr) {
        unimplemented(api, refusal);
        return E_NOTIMPL;
    }
    if (msl.empty() || !through) {
        unimplemented(api, "an adjacency draw whose vertex outputs cannot be passed through");
        return E_NOTIMPL;
    }
    pso.adjacency_msl = std::move(msl);
    if (pso.per_vertex_primitive == 0) pso.per_vertex_primitive = vertices;
    if (!pso.lowered_clip[0].valid) {
        pso.lowered_clip[0] =
            graphics::stats_clip_layout_of(container.output_signature, pso.capture_registers, 0);
    }
    return S_OK;
}

u32 Frontend::lower_shader_stages(const PipelineSource& src, PipelineState& pso,
                                  const char* api) {
    const bool hull = src.hs != 0 && shader_present(src.hs);
    const bool domain = src.ds != 0 && shader_present(src.ds);
    const bool geometry = src.gs != 0 && shader_present(src.gs);
    // Learn (D3D12_GRAPHICS_PIPELINE_STATE_DESC): the hull and domain shaders
    // are both set or both null.
    if (hull != domain) {
        return refuse(E_INVALIDARG, "%s: a %s shader without a %s shader", api,
                      hull ? "hull" : "domain", hull ? "domain" : "hull");
    }
    // Target-independent / conservative rasterization of the primitive that
    // reaches the rasterizer (@p vertices: 1 point, 2 line, 3 triangle):
    // what pass C draws for it (raster_primitive_for).
    RasterPassRequest raster_request{};
    raster_request.modes = raster_varyings(pso.ps);
    raster_request.source.barycentrics = pso.ps.raster_barycentrics;
    raster_request.source.edges_in_buffer = pso.raster.ps.edges_in_buffer;
    raster_request.edge_kernels = &pso.raster.edge_kernel_msl;
    const auto raster_shape = [&](u32 vertices, RasterPassRequest& request) -> u32 {
        if (!pso.raster.active) return S_OK;
        if (vertices < 1 || vertices > 3) {
            return refuse(E_INVALIDARG,
                          "%s: raster emulation of a primitive of %u vertices (topology "
                          "type %u)",
                          api, vertices, pso.graphics.topology_type);
        }
        if (vertices < 3 && pso.raster.ps.mode == graphics::RasterEmulationMode::Conservative) {
            // Measured: the runtime refuses conservative rasterization of
            // a geometry shader's or a domain shader's lines (and of IA
            // lines and points, raster_state_valid).
            d3d12_diag("%s: conservative rasterization of lines or points", api);
            return E_INVALIDARG;
        }
        float width = 1.0f;
        request.source.primitive =
            raster_primitive_for(pso.graphics, pso.raster.ps.mode, vertices, width);
        request.source.primitive_vertices = vertices;
        request.source.line_width = width;
        pso.raster.primitive = request.source.primitive;
        return S_OK;
    };
    if (!hull && !geometry) {
        // Vertex-shader stream output captures that shader's outputs. There
        // is no geometry or hull stage to lower. A raster-emulated pipeline
        // captures them for its pass C, and so does a vertex shader that
        // writes SV_CullDistance (PipelineState::vertex_cull).
        {
            const auto container = graphics::parse_dxbc(
                pso.vs.bytecode.data(), pso.vs.bytecode.size());
            pso.vertex_cull.active = std::any_of(
                container.output_signature.begin(),
                container.output_signature.end(), [](const auto& element) {
                    return element.system_value_type == D3D_NAME_CULL_DISTANCE;
                });
        }
        const u32 per_vertex = pso.ps.per_vertex_inputs;
        // ViewInstancingTier 3 with a vertex shader that does not read
        // SV_ViewID: it runs once (a capture), and each view draws the
        // captured vertices through the pass-through (per_vertex_msl, with
        // no per-vertex copies).
        const bool vi_replay = pso.vi_first == kViRaster &&
                               pso.graphics.topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        if (!pso.stream_output.active && !pso.raster.active && per_vertex == 0 &&
            !vi_replay && !pso.vertex_cull.active) {
            return S_OK;
        }
        if (pso.vertex_cull.active && (per_vertex != 0 || vi_replay) && !pso.raster.active) {
            // Both draw the capture through one pass-through of their own.
            unimplemented(api, "a vertex shader writing SV_CullDistance with "
                               "GetAttributeAtVertex or view instancing tier 3");
            return E_NOTIMPL;
        }
        // With vertex-shader stream output, the draw records the streams
        // first and then rasterizes stream 0 through the pass-through (or
        // pass C) below, from a capture of its own.
        const u32 per_vertex_primitive =
            pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT  ? 1u
            : pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? 2u
            : pso.graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                ? 3u
                : 0u;
        if (per_vertex != 0 && !pso.raster.active && per_vertex_primitive == 0) {
            // PATCH without a hull shader
            return refuse(E_INVALIDARG,
                          "%s: GetAttributeAtVertex with topology type %u and no hull shader",
                          api, pso.graphics.topology_type);
        }
        if (u32 error = raster_shape(per_vertex_primitive, raster_request);
            error != S_OK) {
            return error;
        }
        graphics::DxbcTranslationOptions options{};
        options.view_id = pso.view_id;
        {
            const auto parsed = graphics::parse_dxbc(pso.vs.bytecode.data(),
                                                     pso.vs.bytecode.size());
            guard_flat_stage(parsed, options);
        }
        options.capture_vertex_outputs_to_buffer = true;
        CompiledShader captured;
        FlatCompiled built =
            compile_flat(pso.vs.bytecode, graphics::ShaderStage::Vertex, options,
                         captured, api, pso.root.get());
        if (built.hr != S_OK) {
            if (!built.unimplemented_case.empty()) {
                unimplemented(api, built.unimplemented_case.c_str());
            }
            return built.hr;
        }
        if (!built.spirv.captures_vertex_outputs ||
            built.spirv.capture_registers.empty()) {
            unimplemented(api, "the vertex shader cannot be captured");
            return E_NOTIMPL;
        }
        pso.capture_msl = std::move(captured.msl);
        pso.capture_registers = built.spirv.capture_registers;
        pso.capture_math = captured.math_mode;
        pso.stream_output.registers = pso.capture_registers;
        if (pso.raster.active) {
            // Pass C reads the captured records through the draw's
            // primitive-assembled record ids.
            const auto container = graphics::parse_dxbc(
                pso.vs.bytecode.data(), pso.vs.bytecode.size());
            std::vector<u32> registers;
            for (const auto& element : container.output_signature) {
                if (std::find(registers.begin(), registers.end(),
                              element.register_num) == registers.end()) {
                    registers.push_back(element.register_num);
                }
            }
            std::sort(registers.begin(), registers.end());
            bool viewport = false;
            const char* refusal = nullptr;
            pso.raster.clip = graphics::stats_clip_layout_of(
                container.output_signature, pso.capture_registers, 0);
            if (registers == pso.capture_registers) {
                pso.raster.vertex_msl =
                    passthrough_for(container, 0, 0, /*dense=*/false, viewport,
                                    refusal, 0, nullptr, &raster_request,
                                    per_vertex);
            }
            if (refusal != nullptr) {
                unimplemented(api, refusal);
                return E_NOTIMPL;
            }
            if (pso.raster.vertex_msl.empty()) {
                unimplemented(api, "the vertex shader's outputs cannot be "
                                   "drawn by the raster emulation");
                return E_NOTIMPL;
            }
        } else if (pso.vertex_cull.active) {
            // Pass C reads the records through the draw's list of assembled
            // record ids, a whole primitive of the PSO's topology type at a
            // time, and culls it (generate_gs_passthrough_vertex_msl,
            // records_through_index_list).
            const u32 type = pso.graphics.topology_type;
            const u32 vertices =
                type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT  ? 1u
                : type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? 2u
                : type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ? 3u
                                                                 : 0u;
            if (vertices == 0) {
                return refuse(E_INVALIDARG,
                              "%s: SV_CullDistance with topology type %u and no hull shader",
                              api, type);
            }
            const auto container = graphics::parse_dxbc(
                pso.vs.bytecode.data(), pso.vs.bytecode.size());
            std::vector<u32> registers;
            for (const auto& element : container.output_signature) {
                if (std::find(registers.begin(), registers.end(),
                              element.register_num) == registers.end()) {
                    registers.push_back(element.register_num);
                }
            }
            std::sort(registers.begin(), registers.end());
            bool viewport = false;
            const char* refusal = nullptr;
            bool through_index_list = false;
            if (registers == pso.capture_registers) {
                pso.vertex_cull.vertex_msl = passthrough_for(
                    container, 0, 0, /*dense=*/true, viewport, refusal,
                    vertices, &through_index_list);
            }
            if (refusal != nullptr) {
                unimplemented(api, refusal);
                return E_NOTIMPL;
            }
            if (pso.vertex_cull.vertex_msl.empty() || !through_index_list) {
                unimplemented(api, "the cull distances of a vertex shader "
                                   "whose outputs pass C cannot draw");
                return E_NOTIMPL;
            }
        } else if (per_vertex != 0 || vi_replay) {
            // GetAttributeAtVertex: the captured vertices are drawn
            // NON-indexed through the assembled primitives' record ids (D3D's
            // vertex order, an odd strip triangle n, n + 2, n + 1), each
            // vertex carrying its primitive's copies of the registers.
            const auto container = graphics::parse_dxbc(
                pso.vs.bytecode.data(), pso.vs.bytecode.size());
            bool viewport = false;
            bool through = false;
            const char* refusal = nullptr;
            pso.per_vertex_msl =
                passthrough_for(container, 0, 0, /*dense=*/true, viewport,
                                refusal, per_vertex_primitive, &through,
                                nullptr, per_vertex, /*force_index_list=*/true);
            pso.per_vertex_primitive = per_vertex_primitive;
            // The clipper counts over the captured records (count_clip).
            pso.lowered_clip[0] = graphics::stats_clip_layout_of(
                container.output_signature, pso.capture_registers, 0);
            if (refusal != nullptr) {
                unimplemented(api, refusal);
                return E_NOTIMPL;
            }
            if (pso.per_vertex_msl.empty() || !through) {
                unimplemented(api, "GetAttributeAtVertex: the vertex shader's "
                                   "outputs cannot be passed through");
                return E_NOTIMPL;
            }
        }
        return S_OK;
    }
    // A tessellated pipeline's primitive topology type is PATCH.
    if (hull &&
        pso.graphics.topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        return refuse(E_INVALIDARG, "%s: hull and domain shaders with topology type %u, not PATCH",
                      api, pso.graphics.topology_type);
    }

    const auto fail_lower = [&](const FlatCompiled& built) {
        if (!built.unimplemented_case.empty()) {
            unimplemented(api, built.unimplemented_case.c_str());
        }
        return built.hr;
    };
    const auto read_stage = [&](GuestAddr bytecode, std::vector<u8>& bytes) {
        if (u32 error = read_shader_bytes(bytecode, bytes); error != S_OK) {
            return error;
        }
        if (bytes.empty()) {
            return refuse(E_INVALIDARG, "%s: a lowered stage has no bytecode", api);
        }
        return static_cast<u32>(S_OK);
    };

    CompiledShader captured;
    graphics::DxbcTranslationOptions capture_options{};
    capture_options.view_id = pso.view_id;
    {
        const auto container = graphics::parse_dxbc(pso.vs.bytecode.data(),
                                                     pso.vs.bytecode.size());
        guard_flat_stage(container, capture_options);
    }
    capture_options.capture_vertex_outputs_to_buffer = true;
    FlatCompiled capture =
        compile_flat(pso.vs.bytecode, graphics::ShaderStage::Vertex,
                     capture_options, captured, api, pso.root.get());
    if (capture.hr != S_OK) return fail_lower(capture);
    if (!capture.spirv.captures_vertex_outputs ||
        capture.spirv.capture_registers.empty()) {
        unimplemented(api, "the vertex shader cannot be captured");
        return E_NOTIMPL;
    }
    pso.capture_msl = std::move(captured.msl);
    pso.capture_registers = capture.spirv.capture_registers;
    pso.capture_math = captured.math_mode;

    if (hull) {
        std::vector<u8> hull_bytes;
        std::vector<u8> domain_bytes;
        if (u32 error = read_stage(src.hs, hull_bytes); error != S_OK) {
            return error;
        }
        if (u32 error = read_stage(src.ds, domain_bytes); error != S_OK) {
            return error;
        }
        graphics::DxbcTranslationOptions hull_options{};
        hull_options.view_id = pso.view_id;
        {
            const auto container =
                graphics::parse_dxbc(hull_bytes.data(), hull_bytes.size());
            guard_flat_stage(container, hull_options);
        }
        hull_options.emulate_tessellation = true;
        hull_options.domain_to_compute = true;
        hull_options.gs_input_record_registers = pso.capture_registers;
        FlatCompiled lowered =
            compile_flat(std::move(hull_bytes), graphics::ShaderStage::Hull,
                         hull_options, pso.tessellation.hull, api,
                         pso.root.get());
        if (lowered.hr != S_OK) return fail_lower(lowered);
        if (!lowered.spirv.hs_lowered_to_compute) {
            unimplemented(api, "the hull shader translated without its "
                               "compute lowering");
            return E_NOTIMPL;
        }
        graphics::DxbcTranslationOptions domain_options{};
        domain_options.view_id = pso.view_id;
        {
            const auto container =
                graphics::parse_dxbc(domain_bytes.data(), domain_bytes.size());
            guard_flat_stage(container, domain_options);
        }
        domain_options.emulate_tessellation = true;
        domain_options.domain_to_compute = true;
        FlatCompiled domain_lowered = compile_flat(
            std::move(domain_bytes), graphics::ShaderStage::Domain,
            domain_options, pso.tessellation.domain_stage, api,
            pso.root.get());
        if (domain_lowered.hr != S_OK) return fail_lower(domain_lowered);
        // A domain shader may output nothing (a geometry shader after it
        // then reads 0); its records are then empty.
        if (!domain_lowered.spirv.ds_lowered_to_compute) {
            unimplemented(api, "the domain shader translated without its "
                               "compute lowering");
            return E_NOTIMPL;
        }
        if (domain_lowered.spirv.tess_domain != lowered.spirv.tess_domain ||
            domain_lowered.spirv.hs_output_control_points !=
                lowered.spirv.hs_output_control_points) {
            return refuse(E_INVALIDARG,
                          "%s: the domain shader's domain %u / %u control points differ "
                          "from the hull shader's %u / %u",
                          api, domain_lowered.spirv.tess_domain,
                          domain_lowered.spirv.hs_output_control_points,
                          lowered.spirv.tess_domain, lowered.spirv.hs_output_control_points);
        }
        auto& program = pso.tessellation;
        program.active = true;
        program.tess_domain = lowered.spirv.tess_domain;
        program.partitioning = lowered.spirv.tess_partitioning;
        program.output_primitive = lowered.spirv.tess_output_primitive;
        program.input_control_points = lowered.spirv.hs_input_control_points;
        program.output_control_points = lowered.spirv.hs_output_control_points;
        program.max_tess_factor = lowered.spirv.hs_max_tess_factor;
        program.cp_record_vec4s = std::max(lowered.spirv.hs_cp_record_vec4s, 1u);
        program.pc_record_vec4s = std::max(lowered.spirv.hs_pc_record_vec4s, 1u);
        program.domain_records = domain_lowered.spirv.capture_registers;
        {
            const auto container = graphics::parse_dxbc(
                program.domain_stage.bytecode.data(),
                program.domain_stage.bytecode.size());
            pso.lowered_clip[0] = graphics::stats_clip_layout_of(
                container.output_signature, program.domain_records, 0);
        }
        if (!geometry) {
            // Learn (CreateGeometryShaderWithStreamOutput): with no geometry
            // shader, stream output captures the prior stage. A domain
            // shader's outputs are those vertices. RasterizedStream 0 draws
            // them through the passthrough already built for this pipeline.
            // D3D12_SO_NO_RASTERIZED_STREAM and every other stream index
            // leave the rasterizer unbound, so the domain shader then has
            // no SV_Position requirement.
            const bool raster_stream0 =
                !pso.stream_output.active ||
                pso.stream_output.rasterized_stream == 0;
            if (raster_stream0) {
                const auto container = graphics::parse_dxbc(
                    program.domain_stage.bytecode.data(),
                    program.domain_stage.bytecode.size());
                bool viewport = false;
                const char* refusal = nullptr;
                // The raster pass C reads the domain records through the
                // tessellator's index list, as many triangles as it wrote.
                RasterPassRequest tess_raster = raster_request;
                tess_raster.source.gpu_primitive_limit = true;
                if (u32 error = raster_shape(program.output_primitive == 1u   ? 1u
                                             : program.output_primitive == 2u ? 2u
                                                                              : 3u,
                                             tess_raster);
                    error != S_OK) {
                    return error;
                }
                program.passthrough_msl = passthrough_for(
                    container, 0, 0, /*dense=*/true, viewport, refusal,
                    program.output_primitive == 1u   ? 1u
                    : program.output_primitive == 2u ? 2u
                                                     : 3u,
                    &program.passthrough_through_index_list,
                    pso.raster.active ? &tess_raster : nullptr,
                    pso.ps.per_vertex_inputs);
                if (refusal != nullptr) {
                    unimplemented(api, refusal);
                    return E_NOTIMPL;
                }
                if (program.passthrough_msl.empty()) {
                    // As for a geometry shader: the stage feeding the
                    // rasterizer must output SV_Position.
                    d3d12_diag("%s: the rasterized domain shader has no "
                               "SV_Position -> E_INVALIDARG", api);
                    return E_INVALIDARG;
                }
            }
            if (pso.stream_output.active) {
                pso.stream_output.registers = program.domain_records;
            }
        }
    }

    if (geometry) {
        std::vector<u8> bytes;
        if (u32 error = read_stage(src.gs, bytes); error != S_OK) return error;
        graphics::DxbcTranslationOptions options{};
        options.view_id = pso.view_id;
        {
            const auto container =
                graphics::parse_dxbc(bytes.data(), bytes.size());
            guard_flat_stage(container, options);
        }
        options.emulate_geometry_with_compute = true;
        options.gs_input_record_registers =
            hull ? pso.tessellation.domain_records : pso.capture_registers;
        FlatCompiled lowered =
            compile_flat(std::move(bytes), graphics::ShaderStage::Geometry,
                         options, pso.geometry.kernel, api, pso.root.get());
        if (lowered.hr != S_OK) return fail_lower(lowered);
        if (!lowered.spirv.gs_lowered_to_compute) {
            unimplemented(api, "the geometry shader translated without its "
                               "compute lowering");
            return E_NOTIMPL;
        }
        const u32 vertices = graphics::gs_vertices_per_invocation(
            lowered.spirv.gs_output_topology,
            lowered.spirv.gs_max_output_vertices);
        if (vertices == 0) {
            unimplemented(api,
                          "maxvertexcount cannot complete one output primitive");
            return E_NOTIMPL;
        }
        const auto container = graphics::parse_dxbc(
            pso.geometry.kernel.bytecode.data(),
            pso.geometry.kernel.bytecode.size());
        bool viewport = false;
        const char* refusal = nullptr;
        const bool no_raster =
            pso.stream_output.active &&
            pso.stream_output.rasterized_stream == D3D12_SO_NO_RASTERIZED_STREAM;
        const u32 raster_stream =
            pso.stream_output.active && !no_raster
                ? pso.stream_output.rasterized_stream
                : 0u;
        RasterPassRequest gs_raster = raster_request;
        gs_raster.source.vertices_per_invocation = vertices;
        if (u32 error = raster_shape(lowered.spirv.gs_output_topology == 1u   ? 1u
                                     : lowered.spirv.gs_output_topology == 3u ? 2u
                                                                              : 3u,
                                     gs_raster);
            error != S_OK) {
            return error;
        }
        std::string passthrough =
            passthrough_for(container, raster_stream, vertices, /*dense=*/false,
                            viewport, refusal,
                            lowered.spirv.gs_output_topology == 1u   ? 1u
                            : lowered.spirv.gs_output_topology == 3u ? 2u
                                                                     : 3u,
                            nullptr, pso.raster.active ? &gs_raster : nullptr,
                            pso.ps.per_vertex_inputs);
        if (refusal != nullptr) {
            unimplemented(api, refusal);
            return E_NOTIMPL;
        }
        const u32 stream_mask = lowered.spirv.gs_stream_mask == 0
                                    ? 1u
                                    : lowered.spirv.gs_stream_mask;
        // A stream that is rasterized needs an SV_Position. NO_RASTERIZED
        // does not. A multi-stream shader rasterizing a stream other than
        // 0 is E_INVALIDARG when that stream has outputs and no position.
        if (!no_raster && passthrough.empty() &&
            lowered.spirv.gs_output_registers != 0 &&
            (stream_mask & (1u << raster_stream)) != 0 &&
            (raster_stream != 0 || stream_mask == 1u)) {
            // The rasterizer reads SV_Position from the last stage before
            // it; D3D12 rejects a pipeline that rasterizes a stream without
            // one (E_INVALIDARG), which is not a gap of this emulation.
            d3d12_diag("%s: the rasterized geometry stream has no "
                       "SV_Position -> E_INVALIDARG", api);
            return E_INVALIDARG;
        }
        if (no_raster) passthrough.clear();
        const u32 prim = lowered.spirv.gs_input_primitive;
        const u32 type = pso.graphics.topology_type;
        if (hull) {
            const u32 produced = pso.tessellation.output_primitive == 1u   ? 1u
                                 : pso.tessellation.output_primitive == 2u ? 2u
                                                                           : 3u;
            const bool ok = prim == produced;
            if (!ok) {
                return refuse(E_INVALIDARG,
                              "%s: the GS input primitive %u is not the tessellator's %u",
                              api, prim, produced);
            }
        } else if ((type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT && prim != 1u) ||
                   (type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE && prim != 2u &&
                    prim != 4u) ||
                   (type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && prim != 3u &&
                    prim != 6u) ||
                   type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
            return refuse(E_INVALIDARG,
                          "%s: the GS input primitive %u does not match topology type %u",
                          api, prim, type);
        }
        if (pso.root &&
            !root_covers(*pso.root, pso.geometry.kernel,
                         D3D12_SHADER_VISIBILITY_GEOMETRY)) {
            return refuse(E_INVALIDARG,
                          "%s: the root signature does not cover the GS's bindings", api);
        }
        auto& program = pso.geometry;
        program.active = true;
        program.passthrough_msl = std::move(passthrough);
        program.input_primitive = prim;
        program.output_topology = lowered.spirv.gs_output_topology;
        program.max_output_vertices = lowered.spirv.gs_max_output_vertices;
        program.instance_count = std::max(1u, lowered.spirv.gs_instance_count);
        program.stream_mask = stream_mask;
        program.output_record_vec4s = lowered.spirv.gs_output_registers;
        program.vertices_per_invocation = vertices;
        // PIPELINE_STATISTICS clips each stream's expanded records, which
        // hold the output registers in ascending order (gs_record_order).
        std::vector<u32> record_registers;
        for (const auto& element : container.output_signature) {
            if (std::find(record_registers.begin(), record_registers.end(),
                          element.register_num) == record_registers.end()) {
                record_registers.push_back(element.register_num);
            }
        }
        std::sort(record_registers.begin(), record_registers.end());
        for (u32 stream = 0; stream < 4; ++stream) {
            pso.lowered_clip[stream] = graphics::stats_clip_layout_of(
                container.output_signature, record_registers, stream);
        }
        (void)viewport;
    }
    if (hull && pso.root &&
        (!root_covers(*pso.root, pso.tessellation.hull,
                      D3D12_SHADER_VISIBILITY_HULL) ||
         !root_covers(*pso.root, pso.tessellation.domain_stage,
                      D3D12_SHADER_VISIBILITY_DOMAIN))) {
        return refuse(E_INVALIDARG,
                      "%s: the root signature does not cover the HS's or DS's bindings", api);
    }
    return S_OK;
}

u32 Frontend::finish_graphics_pipeline(PipelineState& pso, const char* api) {
    GraphicsState& state = pso.graphics;
    // Measured (a05 "topology type PATCH without HS/DS"): PATCH with no
    // hull and domain shader is E_INVALIDARG. With both, the topology is
    // the one the tessellator consumes.
    if (state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
        !pso.tessellation.active) {
        return refuse(E_INVALIDARG, "%s: topology type PATCH without a hull and domain shader",
                      api);
    }
    // Measured (a05 pso): the strip-cut value is one of the three; no
    // format past NumRenderTargets; the depth / stencil functions and ops
    // are valid when the test is on; the sample quality is one the formats
    // have.
    if (state.strip_cut > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) {
        return refuse(E_INVALIDARG, "%s: IBStripCutValue %u", api, state.strip_cut);
    }
    for (u32 index = state.num_render_targets; index < 8; ++index) {
        if (state.rtv_formats[index] != DXGI_FORMAT_UNKNOWN) {
            return refuse(E_INVALIDARG,
                          "%s: RTVFormats[%u] %u is set at or past NumRenderTargets %u", api,
                          index, state.rtv_formats[index], state.num_render_targets);
        }
    }
    // A pixel shader may write SV_Target outputs past NumRenderTargets or
    // into a slot with no format (measured, a72 pso: SV_Target0 with
    // NumRenderTargets 0, with or without a DSV; SV_Target0..1 with 1;
    // SV_Target3 with 2; all S_OK). Those writes are discarded: the PS is
    // compiled without them (color_output_mask).
    const auto func_valid = [](u32 f) {
        return f >= D3D12_COMPARISON_FUNC_NEVER && f <= D3D12_COMPARISON_FUNC_ALWAYS;
    };
    const auto op_valid = [](u32 o) {
        return o >= D3D12_STENCIL_OP_KEEP && o <= D3D12_STENCIL_OP_DECR;
    };
    if (state.depth_enable &&
        (!func_valid(state.depth_func) ||
         state.depth_write_mask > D3D12_DEPTH_WRITE_MASK_ALL)) {
        return refuse(E_INVALIDARG, "%s: depth enabled with DepthFunc %u / DepthWriteMask %u",
                      api, state.depth_func, state.depth_write_mask);
    }
    if (state.stencil_enable) {
        for (const auto* face : {&state.front, &state.back}) {
            if (!func_valid(face->func) || !op_valid(face->fail) ||
                !op_valid(face->depth_fail) || !op_valid(face->pass)) {
                return refuse(E_INVALIDARG,
                              "%s: stencil enabled with %s face func %u ops %u/%u/%u", api,
                              face == &state.front ? "front" : "back", face->func,
                              face->fail, face->depth_fail, face->pass);
            }
        }
    }
    {
        u32 format = state.dsv_format;
        for (u32 index = 0; index < state.num_render_targets; ++index) {
            if (state.rtv_formats[index] != DXGI_FORMAT_UNKNOWN) {
                format = state.rtv_formats[index];
                break;
            }
        }
        if (format != DXGI_FORMAT_UNKNOWN) {
            const u32 levels = layout().quality_levels(format, state.sample_count);
            if (levels == 0 || (state.sample_quality >= levels &&
                                state.sample_quality != 0xFFFFFFFFu &&
                                state.sample_quality != 0xFFFFFFFEu)) {
                return refuse(E_INVALIDARG,
                              "%s: SampleDesc {%u, %u}: format %u has %u quality levels at "
                              "that count",
                              api, state.sample_count, state.sample_quality, format, levels);
            }
        }
    }
    // The input layout: every non-system-value input of the vertex shader
    // is fed (a missing one, or no layout at all, is E_INVALIDARG), the
    // root signature allows an input assembler layout when there is one, and
    // one input slot is either per-vertex or per-instance (measured, a05 pso
    // "input layout missing COLOR", "no input layout", "root signature
    // without ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT", "COLOR per-instance in the
    // per-vertex slot 0").
    if (!state.input_layout.empty() && pso.root &&
        (pso.root->flags &
         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) == 0) {
        return refuse(E_INVALIDARG,
                      "%s: an input layout with a root signature without "
                      "ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT",
                      api);
    }
    for (const auto& input : pso.vs.inputs) {
        if (input.system_value != 0) continue;
        const bool fed = std::any_of(
            state.input_layout.begin(), state.input_layout.end(),
            [&](const InputElement& e) {
                return e.semantic_index == input.semantic_index &&
                       semantic_equal(e.semantic, input.semantic);
            });
        if (!fed) {
            return refuse(E_INVALIDARG,
                          "%s: the input layout does not feed VS input %s%u", api,
                          input.semantic.c_str(), input.semantic_index);
        }
    }
    for (const InputElement& a : state.input_layout) {
        for (const InputElement& b : state.input_layout) {
            if (a.input_slot == b.input_slot && a.slot_class != b.slot_class) {
                return refuse(E_INVALIDARG,
                              "%s: input slot %u mixes per-vertex and per-instance elements",
                              api, a.input_slot);
            }
        }
    }
    // Learn (D3D12_BLEND_DESC::IndependentBlendEnable): FALSE uses only
    // RenderTarget[0]; RenderTarget[1..7] are ignored, and so not validated.
    // TRUE validates all eight, those at or past NumRenderTargets too
    // (measured, d3d12_a71_pso_unused_state_probe on WARP and an RTX 4080
    // SUPER: RT[1] SrcBlend 0 under NumRenderTargets 1 is S_OK without
    // independent blending, E_INVALIDARG with it; RT[3] SrcBlendAlpha
    // SRC_COLOR under NumRenderTargets 0 with it is E_INVALIDARG).
    const u32 blend_descs = state.independent_blend ? 8u : 1u;
    for (u32 index = 0; index < blend_descs; ++index) {
        const RenderTargetBlend& b = state.blend[index];
        // Learn (D3D12_RENDER_TARGET_BLEND_DESC): the alpha factors take no
        // colour factor (measured, a05 pso "SrcBlendAlpha SRC_COLOR").
        const auto colour_factor = [](u32 f) {
            return f == D3D12_BLEND_SRC_COLOR || f == D3D12_BLEND_INV_SRC_COLOR ||
                   f == D3D12_BLEND_DEST_COLOR || f == D3D12_BLEND_INV_DEST_COLOR ||
                   f == D3D12_BLEND_SRC1_COLOR || f == D3D12_BLEND_INV_SRC1_COLOR;
        };
        if (b.blend_enable &&
            (colour_factor(b.src_blend_alpha) || colour_factor(b.dest_blend_alpha))) {
            return refuse(E_INVALIDARG,
                          "%s: RenderTarget[%u] alpha blend factors %u/%u name a colour", api,
                          index, b.src_blend_alpha, b.dest_blend_alpha);
        }
        if (b.blend_enable &&
            (!blend_factor_valid(b.src_blend) ||
             !blend_factor_valid(b.dest_blend) ||
             !blend_factor_valid(b.src_blend_alpha) ||
             !blend_factor_valid(b.dest_blend_alpha) ||
             b.blend_op < D3D12_BLEND_OP_ADD || b.blend_op > D3D12_BLEND_OP_MAX ||
             b.blend_op_alpha < D3D12_BLEND_OP_ADD ||
             b.blend_op_alpha > D3D12_BLEND_OP_MAX)) {
            return refuse(E_INVALIDARG,
                          "%s: RenderTarget[%u] blend factors %u/%u/%u/%u ops %u/%u", api,
                          index, b.src_blend, b.dest_blend, b.src_blend_alpha,
                          b.dest_blend_alpha, b.blend_op, b.blend_op_alpha);
        }
    }
    for (u32 index = 0; index < 8; ++index) {
        const u32 format = state.rtv_formats[index];
        if (index >= state.num_render_targets) continue;
        if (format != DXGI_FORMAT_UNKNOWN &&
            graphics::pixel_format_from_dxgi(format) ==
                graphics::PixelFormat::Unknown) {
            // Windows takes any format here, BC1 and TYPELESS included
            // (measured, a05 pso).
            char why[96];
            std::snprintf(why, sizeof(why), "RTVFormats[%u] DXGI format %u has no Metal format",
                          index, format);
            unimplemented(api, why);
            return E_INVALIDARG;
        }
    }
    if (state.dsv_format != DXGI_FORMAT_UNKNOWN &&
        texture_format_for(state.dsv_format,
                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ==
            graphics::PixelFormat::Unknown) {
        // Windows takes a colour DSVFormat too (measured, a05 pso).
        char why[96];
        std::snprintf(why, sizeof(why), "DSVFormat DXGI format %u has no Metal format",
                      state.dsv_format);
        unimplemented(api, why);
        return E_INVALIDARG;
    }
    // UAV-only rendering at SampleDesc.Count N rasterizes through the
    // emulation's one-sample pass (raster_emulation_for): no Metal sample
    // count is involved. Count 0 (accepted only with no output) is one
    // sample.
    const bool uav_only = no_color_formats(state) &&
                          state.dsv_format == DXGI_FORMAT_UNKNOWN;
    if (!(uav_only && pso.raster.active) &&
        !m_backend.supports_texture_sample_count(std::max(1u, state.sample_count))) {
        return refuse(E_INVALIDARG, "%s: SampleDesc.Count %u is not a supported sample count",
                      api, state.sample_count);
    }
    // Map each input slot onto a backend vertex stream, and resolve
    // APPEND_ALIGNED_ELEMENT offsets. A slot whose own stream has a Metal
    // buffer index keeps that stream. A higher slot takes the lowest
    // bindable stream this layout has not claimed. A layout that needs
    // another stream fails: the slot is not dropped and it is not fetched
    // from a colliding index.
    static_assert(graphics::BACKEND_MAX_VERTEX_BUFFERS ==
                  D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
    state.slot_stream.fill(0xFF);
    state.stream_slot.fill(0xFF);
    std::array<bool, graphics::BACKEND_MAX_VERTEX_BUFFERS> claimed{};
    d3d11::VertexLayoutPackingState packing{};
    // B5G6R5, B5G5R5A1, B4G4R4A4 and B8G8R8X8 have IA_VERTEX_BUFFER on
    // Windows (d3d12_a41_format_sweep) and no Metal vertex format: a layout
    // that reads one is fetched by the vertex shader (vertex pulling).
    bool pull_only_format = false;
    for (InputElement& element : state.input_layout) {
        const u32 pulled_bytes =
            d3d11::pulled_only_vertex_format_bytes(element.format);
        if (element.input_slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
            (d3d11::vertex_format_info(element.format) == nullptr &&
             pulled_bytes == 0) ||
            element.slot_class > D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            return refuse(E_INVALIDARG,
                          "%s: input element %s%u: InputSlot %u, Format %u (no vertex "
                          "format) or InputSlotClass %u",
                          api, element.semantic.c_str(), element.semantic_index,
                          element.input_slot, element.format, element.slot_class);
        }
        if (pulled_bytes != 0) pull_only_format = true;
        u32 resolved = 0;
        if (!d3d11::resolve_vertex_element_offset(
                element.input_slot, element.aligned_offset,
                pulled_bytes != 0
                    ? pulled_bytes
                    : d3d11::vertex_format_stride_from_dxgi(element.format),
                packing, resolved)) {
            return refuse(E_INVALIDARG,
                          "%s: input element %s%u: AlignedByteOffset 0x%x does not resolve "
                          "in slot %u",
                          api, element.semantic.c_str(), element.semantic_index,
                          element.aligned_offset, element.input_slot);
        }
        element.aligned_offset = resolved;
        const u32 slot = element.input_slot;
        if (state.slot_stream[slot] != 0xFF) continue;
        if (!graphics::vertex_stream_bindable(slot)) continue;
        state.slot_stream[slot] = static_cast<u8>(slot);
        state.stream_slot[slot] = static_cast<u8>(slot);
        claimed[slot] = true;
        state.stream_count = std::max(state.stream_count, slot + 1);
    }
    for (const InputElement& element : state.input_layout) {
        const u32 slot = element.input_slot;
        if (state.slot_stream[slot] != 0xFF) continue;
        bool placed = false;
        for (u32 stream = 0; stream < graphics::BACKEND_MAX_VERTEX_BUFFERS;
             ++stream) {
            if (!graphics::vertex_stream_bindable(stream)) break;
            if (claimed[stream]) continue;
            state.slot_stream[slot] = static_cast<u8>(stream);
            state.stream_slot[stream] = static_cast<u8>(slot);
            claimed[stream] = true;
            state.stream_count = std::max(state.stream_count, stream + 1);
            placed = true;
            break;
        }
        if (!placed) {
            // D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT is 32; Metal's
            // vertex-stage buffer table leaves BACKEND_BINDABLE_VERTEX_STREAMS
            // (9) above the binding windows. The vertex shader then fetches
            // every input itself (vertex pulling): stream s is slot s.
            state.vertex_pulling = true;
            break;
        }
    }
    if (pull_only_format) state.vertex_pulling = true;
    if (state.vertex_pulling) {
        for (u32 slot = 0; slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
             ++slot) {
            state.slot_stream[slot] = static_cast<u8>(slot);
            state.stream_slot[slot] = static_cast<u8>(slot);
        }
        state.stream_count = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        return S_OK;
    }
    for (u32 stream = 0; stream < state.stream_count; ++stream) {
        if (!claimed[stream]) {
            state.stream_slot[stream] = static_cast<u8>(stream);
        }
    }
    if (pso.mesh.active) return finish_mesh_pipeline(pso, api);
    return S_OK;
}

namespace {

/// The vertex-pull form of one input element feeding v#@p reg
/// (DxbcTranslationOptions::vertex_pull).
bool vertex_pull_element_for(const InputElement& element, u32 reg,
                             graphics::DxbcTranslationOptions& options) {
    if (reg >= options.vertex_pull.size()) return false;
    graphics::VertexPullElement pulled{};
    if (!d3d11::vertex_pull_format(element.format, pulled)) return false;
    pulled.slot = static_cast<u8>(element.input_slot);
    pulled.offset = element.aligned_offset;
    pulled.per_instance =
        element.slot_class == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    pulled.step_rate = pulled.per_instance ? element.step_rate : 0u;
    options.vertex_pulling = true;
    options.vertex_pull[reg] = pulled;
    return true;
}

}  // namespace
u32 Frontend::finish_mesh_pipeline(PipelineState& pso, const char* api) {
    if (pso.mesh.ms_layout.raster_capture) {
        // Pass C over the captured records: each uint4 of the record is a
        // register of a synthetic output signature (mesh_stage.hpp
        // CaptureRecord), in the same order.
        std::vector<graphics::GsPassthroughOutput> outputs;
        for (const auto& record : pso.mesh.ms_layout.capture) {
            graphics::GsPassthroughOutput output{};
            output.register_num = record.register_num;
            output.component_type = record.position ? 3u : record.component_type;
            output.is_position = record.position;
            output.component_system_values = record.system_values;
            outputs.push_back(output);
        }
        graphics::RasterEmulationSource source{};
        source.gpu_primitive_limit = true;
        source.consecutive_records = true;
        pso.raster.vertex_msl = graphics::generate_raster_emulation_vertex_msl(
            outputs, source, raster_varyings(pso.ps));
        if (pso.raster.vertex_msl.empty()) {
            unimplemented(api, "the mesh shader's outputs cannot be drawn by "
                               "the raster emulation");
            return E_INVALIDARG;
        }
    }
    graphics::last_pipeline_failure().clear();
    auto pipeline = make_mesh_pipeline(pso, pso.mesh.ms.msl, pso.mesh.as.msl,
                                       pso.ps.present ? pso.ps.msl : std::string(),
                                       pso.serial);
    if (!pipeline) {
        const std::string& why = graphics::last_pipeline_failure();
        unimplemented(api, ("a mesh pipeline Metal refuses: " +
                            (why.empty() ? std::string("no backend reason") : why))
                               .c_str());
        return E_INVALIDARG;
    }
    pso.mesh.pipeline = std::move(pipeline);
    return S_OK;
}

std::unique_ptr<graphics::BackendPipeline> Frontend::make_mesh_pipeline(
    const PipelineState& pso, const std::string& ms_msl, const std::string& as_msl,
    const std::string& fragment_msl, u64 cache_key, int object, u32 payload_bytes) {
    graphics::BackendMeshPipelineDesc desc{};
    graphics::BackendPipelineDesc& render = desc.render;
    render.vertex_shader_msl = ms_msl;
    render.vertex_math_mode = pso.mesh.ms.math_mode;
    render.fragment_shader_msl = fragment_msl;
    render.fragment_math_mode = pso.ps.math_mode;
    render.vertex_entry_point = graphics::kMeshStageEntryPoint;
    render.fragment_entry_point = "main0";
    apply_color_state(pso.graphics, render);
    if (pso.mesh.ms_layout.raster_capture) {
        // The capture draws nothing: no fragment function, and it runs in
        // a pass of its own on the 1x1 capture target (capture_target).
        graphics::BackendPipelineDesc plain{};
        plain.vertex_shader_msl = std::move(render.vertex_shader_msl);
        plain.vertex_math_mode = render.vertex_math_mode;
        plain.vertex_entry_point = render.vertex_entry_point;
        plain.color_formats[0] = graphics::PixelFormat::RGBA8Unorm;
        plain.color_format = plain.color_formats[0];
        plain.sample_count = 1;
        render = std::move(plain);
    }
    render.cache_key = cache_key;
    desc.mesh_entry_point = graphics::kMeshStageEntryPoint;
    desc.threads_per_mesh_threadgroup = pso.mesh.ms_layout.thread_count();
    if (object < 0 ? pso.mesh.as.present : object != 0) {
        desc.object_entry_point = graphics::kMeshStageEntryPoint;
        desc.object_shader_msl = as_msl;
        desc.object_math_mode = pso.mesh.as.math_mode;
        desc.threads_per_object_threadgroup = pso.mesh.as_layout.thread_count();
        desc.payload_bytes = payload_bytes != 0
                                 ? payload_bytes
                                 : std::max(pso.mesh.as_layout.state.payload_words, 1u) * 4u;
    } else {
        desc.object_entry_point.clear();
    }
    auto pipeline = m_backend.create_mesh_pipeline(desc);
    if (!pipeline || !pipeline->valid()) return nullptr;
    return pipeline;
}

graphics::BackendPipeline* Frontend::pipeline_variant(
    PipelineState& pso,
    const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
    bool statistics) {
    if (!statistics && pso.dynamic_variant) {
        bool fits = true;
        for (u32 stream = 0; stream < strides.size(); ++stream) {
            if ((pso.dynamic_stream_mask & (1u << stream)) != 0 &&
                (strides[stream] == 0 || (strides[stream] & 3u) != 0)) {
                fits = false;
            }
        }
        if (fits) return pso.dynamic_variant.get();
    }
    u64 key = 1469598103934665603ull;
    for (u32 stride : strides) key = (key ^ stride) * 1099511628211ull;
    auto& variants = statistics ? pso.statistics_variants : pso.variants;
    if (auto found = variants.find(key); found != variants.end()) {
        return found->second.get();
    }
    graphics::frame_profile::Scope profile_compile(
        graphics::frame_profile::Time::VariantCompile);
    if (graphics::frame_profile::enabled()) {
        graphics::frame_profile::add(graphics::frame_profile::Counter::VariantCompiles);
    }
    auto pipeline = make_render_variant(pso, strides, statistics, false);
    if (!pipeline) return nullptr;
    graphics::BackendPipeline* raw = pipeline.get();
    variants.emplace(key, std::move(pipeline));
    return raw;
}

std::unique_ptr<graphics::BackendPipeline> Frontend::make_render_variant(
    PipelineState& pso,
    const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
    bool statistics, bool dynamic) {
    u64 key = 1469598103934665603ull;
    for (u32 stride : strides) key = (key ^ stride) * 1099511628211ull;
    const GraphicsState& state = pso.graphics;
    // The translations below run at replay: with this pipeline's SV_ViewID
    // (compile_shader reads build_scratch().view_id).
    struct ViewIdScope {
        u32& slot;
        u32 saved;
        ~ViewIdScope() { slot = saved; }
    } view_scope{build_scratch().view_id, build_scratch().view_id};
    build_scratch().view_id = pso.view_id;
    graphics::BackendPipelineDesc desc{};
    // The vertex attributes: each input element matched to the vertex
    // shader's input register by semantic (D3D12_INPUT_ELEMENT_DESC), and
    // the translated shader specialised to the element's storage type.
    graphics::DxbcTranslationOptions vs_options{};
    vs_options.view_id = pso.view_id;
    vs_options.guard_buffer_bounds = true;
    vs_options.uavs_in_every_stage = true;  // D3D12: UAVs in all stages
    vs_options.vertex_cull_distances_culled_by_caller = true;
    bool specialize = false;
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS> inferred{};
    for (const InputElement& element : state.input_layout) {
        const CompiledShader::Input* input = nullptr;
        for (const auto& candidate : pso.vs.inputs) {
            if (candidate.semantic_index == element.semantic_index &&
                semantic_equal(candidate.semantic, element.semantic)) {
                input = &candidate;
                break;
            }
        }
        // An element the shader does not read is fetched by nothing.
        if (input == nullptr) continue;
        if (state.vertex_pulling) {
            // The shader fetches it itself (vertex_pull_element_for).
            if (!vertex_pull_element_for(element, input->register_num,
                                         vs_options)) {
                return nullptr;
            }
            specialize = true;
            continue;
        }
        graphics::BackendVertexAttribute attribute{};
        attribute.shader_location = input->register_num;
        attribute.offset = element.aligned_offset;
        attribute.buffer_index = state.slot_stream[element.input_slot];
        attribute.format = d3d11::vertex_format_from_dxgi(element.format);
        attribute.storage_component_type =
            d3d11::vertex_storage_component_type(element.format);
        attribute.shader_component_type =
            input->component_type == 1u
                ? graphics::BackendVertexComponentType::UInt
                : input->component_type == 2u
                      ? graphics::BackendVertexComponentType::SInt
                      : graphics::BackendVertexComponentType::Float;
        if (attribute.format == graphics::BackendVertexFormat::UInt1010102 &&
            attribute.shader_location < 32) {
            vs_options.vertex_input_unpack_uint1010102 |=
                1u << attribute.shader_location;
            specialize = true;
        } else if (attribute.storage_component_type !=
                       attribute.shader_component_type &&
                   attribute.shader_location <
                       vs_options.vertex_input_component_types.size()) {
            specialize = true;
            vs_options.vertex_input_component_types[attribute.shader_location] =
                attribute.storage_component_type ==
                        graphics::BackendVertexComponentType::UInt
                    ? 1u
                    : attribute.storage_component_type ==
                              graphics::BackendVertexComponentType::SInt
                          ? 2u
                          : 3u;
        }
        const unsigned stream = attribute.buffer_index;
        if (element.slot_class ==
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            desc.vertex_buffer_per_instance[stream] = true;
            desc.vertex_buffer_step_rates[stream] = element.step_rate;
        }
        inferred[stream] = std::max(
            inferred[stream],
            attribute.offset + d3d11::vertex_format_stride(attribute.format));
        desc.vertex_attributes.push_back(attribute);
    }
    for (unsigned stream = 0; stream < strides.size() && !dynamic; ++stream) {
        desc.vertex_buffer_strides[stream] = strides[stream];
        // A stream the layout reads with stride 0 is a constant stream
        // (every vertex and instance reads the element at the offset, as the
        // D3D11 frontend measured for IASetVertexBuffers).
        if (strides[stream] == 0 && inferred[stream] != 0) {
            desc.vertex_buffer_constant[stream] = true;
            desc.vertex_buffer_strides[stream] = inferred[stream];
        }
    }
    if (dynamic) {
        if (specialize || statistics) return nullptr;
        desc.dynamic_vertex_strides = true;
    }
    desc.vertex_stride = desc.vertex_buffer_strides[0];
    std::string vertex_msl = pso.vs.msl;
    std::string fragment_msl = pso.ps.present ? pso.ps.msl : std::string();
    if (statistics) {
        // PIPELINE_STATISTICS: the same translations as the pipeline's,
        // plus the vertex capture the clipper counts from and the pixel
        // shader's invocation count.
        ShaderVariantOptions vertex{};
        vertex.capture_rasterized = true;
        vertex.vertex_input_unpack_uint1010102 =
            vs_options.vertex_input_unpack_uint1010102;
        vertex.vertex_input_component_types =
            vs_options.vertex_input_component_types;
        vertex.vertex_pulling = vs_options.vertex_pulling;
        vertex.vertex_pull = vs_options.vertex_pull;
        std::vector<u32> registers;
        vertex.capture_registers = &registers;
        CompiledShader built;
        // A vertex shader the capture cannot express (an early return, no
        // outputs) draws as before: its CPrimitives are then unmeasured
        // (statistics_clip invalid), never invented.
        pso.statistics_capture_registers.clear();
        pso.statistics_clip = graphics::StatsClipLayout{};
        if (compile_shader(pso.vs.bytecode, graphics::ShaderStage::Vertex, built,
                           "ExecuteCommandLists", pso.root.get(),
                           D3D12_SHADER_VISIBILITY_VERTEX, nullptr,
                           &vertex) == S_OK &&
            !registers.empty()) {
            vertex_msl = std::move(built.msl);
            specialize = false;
            pso.statistics_capture_registers = registers;
            const auto container = graphics::parse_dxbc(
                pso.vs.bytecode.data(), pso.vs.bytecode.size());
            pso.statistics_clip = graphics::stats_clip_layout_of(
                container.output_signature, registers, 0);
        }
        if (pso.ps.present) fragment_msl = statistics_fragment(pso);
    }
    if (specialize) {
        const auto container = graphics::parse_dxbc(pso.vs.bytecode.data(),
                                                     pso.vs.bytecode.size());
        const auto spirv = graphics::translate_dxbc_to_spirv(container,
                                                             vs_options);
        graphics::MslCompileOptions msl{};
        msl.argument_buffers = true;
        const auto index = [](u32 set) {
            return graphics::BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
        };
        msl.argument_buffer_sets[0] = {pso.vs.registers.cbv_slots, index(0),
                                       graphics::MslDescriptorClass::Buffer};
        msl.argument_buffer_sets[1] = {pso.vs.registers.srv_slots, index(1),
                                       graphics::MslDescriptorClass::Texture};
        msl.argument_buffer_sets[2] = {pso.vs.registers.sampler_slots,
                                       index(2),
                                       graphics::MslDescriptorClass::Sampler};
        msl.argument_buffer_sets[3] = {pso.vs.registers.uav_slots, index(3),
                                       graphics::MslDescriptorClass::Texture};
        if (spirv.uses_typed_uav_atomics) msl.msl_version = 30100u;
        const auto compiled =
            spirv.valid() ? graphics::compile_spirv_to_msl(
                                spirv.spirv.data(), spirv.spirv.size(),
                                graphics::ShaderStage::Vertex, msl)
                          : graphics::CompiledShader{};
        if (!compiled.valid()) {
            d3d12_diag("vertex input specialisation failed to compile");
            return nullptr;
        }
        vertex_msl = compiled.msl_text();
    }
    desc.vertex_shader_msl = std::move(vertex_msl);
    desc.fragment_shader_msl = std::move(fragment_msl);
    desc.vertex_math_mode = pso.vs.math_mode;
    desc.fragment_math_mode = pso.ps.math_mode;
    desc.preserve_invariance = pso.vs.preserve_invariance;
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
    apply_color_state(state, desc);
    desc.input_primitive_class = primitive_class(state.topology_type);
    desc.cache_key = key ^ pso.serial ^ (statistics ? 0x57A7C0ull : 0ull) ^
                     (dynamic ? 0xD1A5D1A5ull : 0ull);
    auto pipeline = m_backend.create_pipeline(desc);
    if (!pipeline || !pipeline->valid()) {
        // The dynamic form is an optimisation: its draws fall back to the
        // stride variants, which name their own failure.
        if (!dynamic) {
            d3d12_diag("backend refused the render pipeline of PSO 0x%llx",
                       static_cast<unsigned long long>(pso.address));
        }
        return nullptr;
    }
    if (!pipeline->dynamic_vertex_strides() && dynamic) return nullptr;
    if (dynamic) {
        pso.dynamic_stream_mask = 0;
        for (const auto& attribute : desc.vertex_attributes) {
            if (attribute.buffer_index < 32) {
                pso.dynamic_stream_mask |= 1u << attribute.buffer_index;
            }
        }
    }
    return pipeline;
}

const std::string& Frontend::statistics_fragment(PipelineState& pso) {
    if (!pso.statistics_fragment_tried) {
        pso.statistics_fragment_tried = true;
        pso.statistics_pixels = false;
        LogicOpVariant logic{};
        ShaderVariantOptions pixel{};
        pixel.count_pixel_invocations = true;
        pixel.primitive_id_varying = pso.mesh.active;
        pixel.color_output_mask = color_output_mask(pso.graphics);
        CompiledShader counted;
        // Translated at replay: with this pipeline's SV_ViewID (a view of a
        // view-instanced pipeline), not the build's (0 by now), and its mesh
        // linkage.
        const u32 saved_view = build_scratch().view_id;
        const std::array<u8, 32> saved_split = build_scratch().primitive_split;
        build_scratch().view_id = pso.view_id;
        build_scratch().primitive_split = pso.mesh.active ? pso.mesh.ms_layout.ps_primitive_split
                                                  : std::array<u8, 32>{};
        if (pso.ps.present &&
            logic_op_variant(pso.graphics, logic, "ExecuteCommandLists") ==
                S_OK &&
            compile_shader(pso.ps.bytecode, graphics::ShaderStage::Fragment,
                           counted, "ExecuteCommandLists", pso.root.get(),
                           D3D12_SHADER_VISIBILITY_PIXEL, &logic, &pixel,
                           // A raster-emulated pixel shader keeps its
                           // coverage test in the counting translation.
                           pso.raster.active ? &pso.raster.ps : nullptr) ==
                S_OK) {
            pso.statistics_fragment_msl = std::move(counted.msl);
            pso.statistics_pixels = true;
        }
        build_scratch().view_id = saved_view;
        build_scratch().primitive_split = saved_split;
    }
    // Uncounted, the pixel shader draws as before and PSInvocations is
    // reported unmeasured.
    return pso.statistics_pixels ? pso.statistics_fragment_msl : pso.ps.msl;
}

graphics::BackendComputePipeline* Frontend::statistics_kernel(
    const char* entry_point) {
    auto& slot = m_stats_kernels[entry_point];
    if (!slot) {
        static const std::string msl = graphics::pipeline_stats_msl();
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = msl;
        desc.compute_entry_point = entry_point;
        desc.cache_key = std::hash<std::string>{}(msl) ^
                         std::hash<std::string>{}(entry_point);
        slot = m_backend.create_compute_pipeline(desc);
    }
    return slot && slot->valid() ? slot.get() : nullptr;
}

graphics::BackendComputePipeline* Frontend::indirect_count_kernel() {
    if (!m_indirect_count) {
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_entry_point = graphics::kIndirectCountEntryPoint;
        desc.compute_shader_msl = graphics::indirect_count_msl();
        desc.cache_key = 0x1D1C0047ull;
        m_indirect_count = m_backend.create_compute_pipeline(desc);
    }
    return m_indirect_count && m_indirect_count->valid()
               ? m_indirect_count.get()
               : nullptr;
}

void Frontend::poll_lowered_feedback() {
    auto& list = m_lowered_feedback;
    for (usize i = 0; i < list.size();) {
        const auto& entry = list[i];
        if (entry.generation != 0 &&
            !m_backend.generation_complete(entry.generation)) {
            ++i;
            continue;
        }
        std::array<u32, graphics::IA_RESOLVED_WORDS> words{};
        if (entry.resolved &&
            entry.resolved->read(0, words.data(), sizeof(words)) &&
            words[graphics::IA_RESOLVED_OVERFLOW] != 0) {
            if (m_lowered_record_capacity >= (1u << 24)) {
                unimplemented("ExecuteCommandLists",
                              "an indirect geometry / tessellation draw needs "
                              "more captured vertices than the size limit "
                              "allows; it is dropped");
            } else {
                m_lowered_record_capacity *= 4u;
                std::fprintf(stderr,
                             "[vitreo] d3d12: an indirect geometry / "
                             "tessellation draw needed more than the captured "
                             "vertices it was sized for; that draw was "
                             "dropped, capacity now %u\n",
                             m_lowered_record_capacity);
            }
        }
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

graphics::BackendComputePipeline* Frontend::ia_resolve_kernel() {
    if (!m_ia_resolve) {
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_entry_point = graphics::kIaResolveEntryPoint;
        desc.compute_shader_msl = graphics::ia_resolve_msl();
        desc.cache_key = 0x1A7E5020ull;  // cut_mode
        m_ia_resolve = m_backend.create_compute_pipeline(desc);
    }
    return m_ia_resolve && m_ia_resolve->valid() ? m_ia_resolve.get() : nullptr;
}

graphics::BackendPipeline* Frontend::capture_variant(
    PipelineState& pso,
    const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
    bool relative) {
    u64 key = 1469598103934665603ull;
    for (u32 stride : strides) key = (key ^ stride) * 1099511628211ull;
    if (relative) key = (key ^ 0x5E1A71u) * 1099511628211ull;
    if (auto found = pso.capture_variants.find(key);
        found != pso.capture_variants.end()) {
        return found->second.get();
    }
    graphics::frame_profile::Scope profile_compile(
        graphics::frame_profile::Time::VariantCompile);
    if (graphics::frame_profile::enabled()) {
        graphics::frame_profile::add(graphics::frame_profile::Counter::VariantCompiles);
    }
    const GraphicsState& state = pso.graphics;
    graphics::BackendPipelineDesc desc{};
    graphics::DxbcTranslationOptions options{};
    options.view_id = pso.view_id;
    {
        const auto container = graphics::parse_dxbc(pso.vs.bytecode.data(),
                                                     pso.vs.bytecode.size());
        guard_flat_stage(container, options);
    }
    options.capture_vertex_outputs_to_buffer = true;
    options.capture_draw_relative = relative;
    // pso.capture_msl keys records by the raw vertex id.
    bool specialize = relative;
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS> inferred{};
    for (const InputElement& element : state.input_layout) {
        const CompiledShader::Input* input = nullptr;
        for (const auto& candidate : pso.vs.inputs) {
            if (candidate.semantic_index == element.semantic_index &&
                semantic_equal(candidate.semantic, element.semantic)) {
                input = &candidate;
                break;
            }
        }
        if (input == nullptr) continue;
        if (state.vertex_pulling) {
            if (!vertex_pull_element_for(element, input->register_num,
                                         options)) {
                return nullptr;
            }
            specialize = true;
            continue;
        }
        graphics::BackendVertexAttribute attribute{};
        attribute.shader_location = input->register_num;
        attribute.offset = element.aligned_offset;
        attribute.buffer_index = state.slot_stream[element.input_slot];
        attribute.format = d3d11::vertex_format_from_dxgi(element.format);
        attribute.storage_component_type =
            d3d11::vertex_storage_component_type(element.format);
        attribute.shader_component_type =
            input->component_type == 1u
                ? graphics::BackendVertexComponentType::UInt
                : input->component_type == 2u
                      ? graphics::BackendVertexComponentType::SInt
                      : graphics::BackendVertexComponentType::Float;
        if (attribute.format == graphics::BackendVertexFormat::UInt1010102 &&
            attribute.shader_location < 32) {
            options.vertex_input_unpack_uint1010102 |=
                1u << attribute.shader_location;
            specialize = true;
        } else if (attribute.storage_component_type !=
                       attribute.shader_component_type &&
                   attribute.shader_location <
                       options.vertex_input_component_types.size()) {
            specialize = true;
            options.vertex_input_component_types[attribute.shader_location] =
                attribute.storage_component_type ==
                        graphics::BackendVertexComponentType::UInt
                    ? 1u
                    : attribute.storage_component_type ==
                              graphics::BackendVertexComponentType::SInt
                          ? 2u
                          : 3u;
        }
        const unsigned stream = attribute.buffer_index;
        if (element.slot_class ==
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            desc.vertex_buffer_per_instance[stream] = true;
            desc.vertex_buffer_step_rates[stream] = element.step_rate;
        }
        inferred[stream] = std::max(
            inferred[stream],
            attribute.offset + d3d11::vertex_format_stride(attribute.format));
        desc.vertex_attributes.push_back(attribute);
    }
    for (unsigned stream = 0; stream < strides.size(); ++stream) {
        desc.vertex_buffer_strides[stream] = strides[stream];
        if (strides[stream] == 0 && inferred[stream] != 0) {
            desc.vertex_buffer_constant[stream] = true;
            desc.vertex_buffer_strides[stream] = inferred[stream];
        }
    }
    desc.vertex_stride = desc.vertex_buffer_strides[0];
    std::string vertex_msl = pso.capture_msl;
    if (specialize) {
        CompiledShader specialized;
        const FlatCompiled built =
            compile_flat(pso.vs.bytecode, graphics::ShaderStage::Vertex,
                         options, specialized, "ExecuteCommandLists",
                         pso.root.get());
        if (built.hr != S_OK) return nullptr;
        vertex_msl = std::move(specialized.msl);
    }
    desc.vertex_shader_msl = std::move(vertex_msl);
    desc.vertex_math_mode = pso.capture_math;
    desc.vertex_entry_point = "main0";
    desc.rasterization_enabled = false;
    desc.color_formats[0] = graphics::PixelFormat::RGBA8Unorm;
    desc.color_format = desc.color_formats[0];
    desc.sample_count = 1;
    desc.cache_key = key ^ pso.serial ^ 0xCA97u;
    auto pipeline = m_backend.create_pipeline(desc);
    if (!pipeline || !pipeline->valid()) {
        d3d12_diag("backend refused the vertex-capture pipeline");
        return nullptr;
    }
    graphics::BackendPipeline* raw = pipeline.get();
    pso.capture_variants.emplace(key, std::move(pipeline));
    return raw;
}

graphics::BackendPipeline* Frontend::passthrough_variant(
    PipelineState& pso, const std::string& vertex_msl,
    std::unique_ptr<graphics::BackendPipeline>& slot,
    graphics::BackendPrimitiveTopology topology, bool statistics) {
    if (slot) return slot.get();
    if (vertex_msl.empty()) return nullptr;
    graphics::frame_profile::Scope profile_compile(
        graphics::frame_profile::Time::VariantCompile);
    if (graphics::frame_profile::enabled()) {
        graphics::frame_profile::add(graphics::frame_profile::Counter::VariantCompiles);
    }
    graphics::BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.fragment_shader_msl =
        !pso.ps.present ? std::string()
        : statistics    ? statistics_fragment(pso)
                        : pso.ps.msl;
    // With no pixel shader, a raster-emulated pass C still needs its
    // coverage test (a conservative depth-only pass).
    if (!pso.ps.present && pso.raster.active) {
        desc.fragment_shader_msl =
            graphics::generate_raster_emulation_fragment_msl(pso.raster.ps);
    }
    desc.fragment_math_mode = pso.ps.math_mode;
    desc.vertex_entry_point = graphics::kGsPassthroughEntryPoint;
    desc.fragment_entry_point = "main0";
    desc.input_primitive_class = graphics::primitive_class_of(topology);
    apply_color_state(pso.graphics, desc);
    // A raster-emulated pass C with no target rasterizes one sample.
    if (pso.raster.active && desc.attachmentless) desc.sample_count = 1;
    desc.cache_key = std::hash<std::string>{}(vertex_msl) ^ pso.serial ^
                     (statistics ? 0x57A7C0ull : 0ull);
    slot = m_backend.create_pipeline(desc);
    if (!slot || !slot->valid()) {
        d3d12_diag("backend refused the geometry pass-through pipeline");
        slot.reset();
        return nullptr;
    }
    return slot.get();
}

graphics::BackendTexture2D* Frontend::capture_target() {
    if (!m_capture_target) {
        graphics::BackendTexture2DDesc desc{};
        desc.width = 1;
        desc.height = 1;
        desc.mip_levels = 1;
        desc.array_size = 1;
        desc.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        desc.render_target = true;
        m_capture_target = m_backend.create_texture_2d(desc, nullptr, 0);
    }
    return m_capture_target ? m_capture_target.get() : nullptr;
}

bool Frontend::tessellator_kernels(graphics::BackendComputePipeline*& patch,
                                   graphics::BackendComputePipeline*& scan) {
    if (!m_tess_patch || !m_tess_scan) {
        const std::string msl = graphics::tess::tessellator_msl();
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = msl;
        desc.compute_entry_point = graphics::tess::kTessPatchEntryPoint;
        desc.cache_key = 0x7E550A7Cull;
        m_tess_patch = m_backend.create_compute_pipeline(desc);
        desc.compute_entry_point = graphics::tess::kTessScanEntryPoint;
        desc.cache_key = 0x7E550A7Dull;
        m_tess_scan = m_backend.create_compute_pipeline(desc);
    }
    patch = m_tess_patch.get();
    scan = m_tess_scan.get();
    return patch != nullptr && patch->valid() && scan != nullptr &&
           scan->valid();
}

bool Frontend::read_buffer_va(u64 va, void* out, usize bytes) {
    if (out == nullptr || bytes == 0) return false;
    auto range = m_va_ranges.upper_bound(va);
    if (range == m_va_ranges.begin()) return false;
    --range;
    const Memory* memory = range->second;
    if (memory == nullptr || memory->backend == nullptr || va < memory->va ||
        va - memory->va > memory->size ||
        bytes > memory->size - (va - memory->va)) {
        return false;
    }
    const u64 offset = va - memory->va;
    if (memory->cpu_visible && memory->backend->host_pointer() != nullptr) {
        std::memcpy(out,
                    static_cast<const u8*>(memory->backend->host_pointer()) +
                        offset,
                    bytes);
        return true;
    }
    graphics::BackendBuffer* buffer = memory->backend->buffer();
    return buffer != nullptr &&
           buffer->read(static_cast<usize>(offset), out, bytes);
}

bool Frontend::read_upload_va(u64 va, void* out, usize bytes) {
    if (out == nullptr || bytes == 0) return false;
    auto range = m_va_ranges.upper_bound(va);
    if (range == m_va_ranges.begin()) return false;
    --range;
    const Memory* memory = range->second;
    // D3D12_HEAP_TYPE_UPLOAD: the GPU only reads it (GENERIC_READ).
    if (memory == nullptr || memory->backend == nullptr ||
        !memory->upload || !memory->cpu_visible ||
        memory->backend->host_pointer() == nullptr || va < memory->va ||
        va - memory->va > memory->size ||
        bytes > memory->size - (va - memory->va)) {
        return false;
    }
    std::memcpy(out,
                static_cast<const u8*>(memory->backend->host_pointer()) +
                    (va - memory->va),
                bytes);
    return true;
}

graphics::BackendComputePipeline* Frontend::ia_cut_kernel() {
    if (!m_ia_cut) {
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_entry_point = graphics::kIaCutEntryPoint;
        desc.compute_shader_msl = graphics::ia_cut_rewrite_msl();
        desc.cache_key = 0x1AC07001ull;
        m_ia_cut = m_backend.create_compute_pipeline(desc);
    }
    return m_ia_cut && m_ia_cut->valid() ? m_ia_cut.get() : nullptr;
}

void Frontend::create_compute_pipeline_state(
    Marshaler& m, Device& device, usize first,
    std::unique_ptr<PipelineState>* capture) {
    // CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
    //     REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device::CreateComputePipelineState";
    const GuestAddr desc = m.arg_ptr(first);
    const GuestAddr out = m.arg_ptr(first + 2);
    PipelineSource src{};
    PipelineRefusalScope report(*this, m, api);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(refuse(E_INVALIDARG, "%s: pDesc is NULL", api));
        return;
    }
    const auto& l = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[m_is_64bit];
    report.desc = desc;
    report.desc_size = l.size;
    report.src = &src;
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    pso->compute = true;
    (void)read_pointer(desc + l.pRootSignature, src.root);
    src.cs = desc + l.CS;
    pso->node_mask = read_u32(desc + l.NodeMask);
    pso->flags = read_u32(desc + l.Flags);
    (void)read_pointer(desc + l.CachedPSO_pCachedBlob, src.cached_blob);
    (void)read_pointer(desc + l.CachedPSO_CachedBlobSizeInBytes,
                       src.cached_size);
    if (u32 error = build_pipeline_unlocked(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (capture != nullptr) {
        // ID3D12PipelineLibrary's Load*: the library decides what to hand
        // out.
        *capture = std::move(pso);
        m.set_return(S_OK);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(first + 1), out));
}

u32 Frontend::finish_compute_pipeline(PipelineState& pso, const char* api) {
    graphics::BackendComputePipelineDesc desc{};
    desc.compute_shader_msl = pso.cs.msl;
    desc.compute_entry_point = "main0";
    desc.math_mode = pso.cs.math_mode;
    graphics::last_pipeline_failure().clear();
    pso.compute_pipeline = m_backend.create_compute_pipeline(desc);
    if (!pso.compute_pipeline || !pso.compute_pipeline->valid()) {
        // MSL the translator produced and Metal will not build.
        const std::string& why = graphics::last_pipeline_failure();
        unimplemented(api, ("a compute pipeline Metal refuses: " +
                            (why.empty() ? std::string("no backend reason") : why))
                               .c_str());
        return E_INVALIDARG;
    }
    return S_OK;
}

void Frontend::create_pipeline_state(
    Marshaler& m, Device& device, usize first,
    std::unique_ptr<PipelineState>* capture) {
    // ID3D12Device2::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC
    //     *pDesc, REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device2::CreatePipelineState";
    const GuestAddr desc = m.arg_ptr(first);
    const GuestAddr out = m.arg_ptr(first + 2);
    PipelineSource src{};
    PipelineRefusalScope report(*this, m, api);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(refuse(E_INVALIDARG, "%s: pDesc is NULL", api));
        return;
    }
    const auto& sl = layout::D3D12_PIPELINE_STATE_STREAM_DESC[m_is_64bit];
    GuestAddr stream_size = 0;
    GuestAddr stream = 0;
    (void)read_pointer(desc + sl.SizeInBytes, stream_size);
    (void)read_pointer(desc + sl.pPipelineStateSubobjectStream, stream);
    report.desc = desc;
    report.desc_size = sl.size;
    report.stream = stream;
    report.stream_size = stream_size;
    report.src = &src;
    if (stream_size == 0 || stream == 0) {
        m.set_return(refuse(E_INVALIDARG, "%s: SizeInBytes %llu / stream 0x%llx", api,
                            static_cast<unsigned long long>(stream_size),
                            static_cast<unsigned long long>(stream)));
        return;
    }
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    pso->from_stream = true;
    GraphicsState& state = pso->graphics;
    default_graphics_state(state);
    // Each subobject is D3DX12's `alignas(void*) { TYPE; Inner; }`: the
    // inner struct at its natural alignment after the 4-byte type, the whole
    // rounded up to a pointer (d3dx12_pipeline_state_stream.h).
    const u64 pointer = m_is_64bit ? 8u : 4u;
    const auto align = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    u64 seen = 0;
    u64 cursor = 0;
    GuestAddr elements = 0;
    u32 element_count = 0;
    while (cursor < stream_size) {
        if (stream_size - cursor < 4) {
            m.set_return(refuse(E_INVALIDARG,
                                "%s: stream ends inside a subobject type at byte %llu of %llu",
                                api, static_cast<unsigned long long>(cursor),
                                static_cast<unsigned long long>(stream_size)));
            return;
        }
        const u32 type = read_u32(stream + cursor);
        u64 inner_align = 4;
        u64 inner_size = 4;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            inner_align = inner_size = pointer;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            inner_align = pointer;
            inner_size = layout::D3D12_SHADER_BYTECODE[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            inner_align = pointer;
            inner_size = layout::D3D12_STREAM_OUTPUT_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            inner_size = layout::D3D12_BLEND_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            inner_size = layout::D3D12_RASTERIZER_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
            inner_size = layout::D3D12_RASTERIZER_DESC1[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
            inner_size = layout::D3D12_RASTERIZER_DESC2[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            inner_size = layout::D3D12_DEPTH_STENCIL_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            inner_size = layout::D3D12_DEPTH_STENCIL_DESC1[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
            inner_size = layout::D3D12_DEPTH_STENCIL_DESC2[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            inner_align = pointer;
            inner_size = layout::D3D12_INPUT_LAYOUT_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            inner_size = layout::D3D12_RT_FORMAT_ARRAY[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            inner_size = layout::DXGI_SAMPLE_DESC[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            inner_align = pointer;
            inner_size = layout::D3D12_CACHED_PIPELINE_STATE[m_is_64bit].size;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            inner_align = pointer;
            inner_size = layout::D3D12_VIEW_INSTANCING_DESC[m_is_64bit].size;
            break;
        default:
            m.set_return(refuse(E_INVALIDARG, "%s: unknown subobject type %u at byte %llu",
                                api, type, static_cast<unsigned long long>(cursor)));
            return;
        }
        // Learn (D3D12_PIPELINE_STATE_STREAM_DESC): each subobject type at
        // most once.
        if ((seen & (1ull << type)) != 0) {
            m.set_return(refuse(E_INVALIDARG, "%s: subobject type %u appears twice", api,
                                type));
            return;
        }
        seen |= 1ull << type;
        const u64 inner_offset = align(cursor + 4, inner_align);
        const u64 next = align(inner_offset + inner_size, pointer);
        if (inner_offset + inner_size > stream_size) {
            m.set_return(refuse(E_INVALIDARG,
                                "%s: subobject type %u (%llu bytes at %llu) runs past "
                                "SizeInBytes %llu",
                                api, type, static_cast<unsigned long long>(inner_size),
                                static_cast<unsigned long long>(inner_offset),
                                static_cast<unsigned long long>(stream_size)));
            return;
        }
        const GuestAddr at = stream + inner_offset;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            (void)read_pointer(at, src.root);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: src.vs = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: src.ps = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: src.ds = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: src.hs = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: src.gs = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: src.as = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: src.ms = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            // A CS subobject with no bytecode does not make the stream a
            // compute pipeline.
            if (shader_present(at)) src.cs = at;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
            const auto& so = layout::D3D12_STREAM_OUTPUT_DESC[m_is_64bit];
            (void)read_pointer(at + so.pSODeclaration, src.so_declaration);
            src.stream_output_entries = read_u32(at + so.NumEntries);
            (void)read_pointer(at + so.pBufferStrides, src.so_strides);
            src.so_num_strides = read_u32(at + so.NumStrides);
            src.so_rasterized_stream = read_u32(at + so.RasterizedStream);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            read_blend(*this, at, state);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            state.sample_mask = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            read_rasterizer(*this, at, state);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: {
            // DESC1: DepthBias is a FLOAT. DESC2: LineRasterizationMode
            // replaces MultisampleEnable / AntialiasedLineEnable.
            const auto& r1 = layout::D3D12_RASTERIZER_DESC1[m_is_64bit];
            const auto& r2 = layout::D3D12_RASTERIZER_DESC2[m_is_64bit];
            const bool desc2 =
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2;
            state.fill_mode = read_u32(at + r1.FillMode);
            state.cull_mode = read_u32(at + r1.CullMode);
            state.front_ccw = read_u32(at + r1.FrontCounterClockwise) != 0;
            state.depth_bias = read_f32(at + r1.DepthBias);
            state.depth_bias_clamp = read_f32(at + r1.DepthBiasClamp);
            state.slope_scaled_depth_bias =
                read_f32(at + r1.SlopeScaledDepthBias);
            state.depth_clip = read_u32(at + r1.DepthClipEnable) != 0;
            if (desc2) {
                const u32 mode = read_u32(at + r2.LineRasterizationMode);
                state.multisample =
                    mode >= D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE;
                state.narrow_quad_line =
                    mode == D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW;
                state.antialiased_line =
                    mode == D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED;
                state.forced_sample_count = read_u32(at + r2.ForcedSampleCount);
                state.conservative = read_u32(at + r2.ConservativeRaster);
            } else {
                state.multisample = read_u32(at + r1.MultisampleEnable) != 0;
                state.antialiased_line =
                    read_u32(at + r1.AntialiasedLineEnable) != 0;
                state.forced_sample_count = read_u32(at + r1.ForcedSampleCount);
                state.conservative = read_u32(at + r1.ConservativeRaster);
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            read_depth_stencil(*this, at, false, state);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            read_depth_stencil(*this, at, true, state);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: {
            // Per-face stencil masks: one pair unless OPTIONS14
            // IndependentFrontAndBackStencilRefMaskSupported, which the caps
            // pass reports FALSE (Learn, D3D12_DEPTH_STENCIL_DESC2).
            const auto& d2 = layout::D3D12_DEPTH_STENCIL_DESC2[m_is_64bit];
            const auto byte = [&](GuestAddr a) -> u8 {
                const auto v = m_memory.read_u8(a);
                return v.is_ok() ? v.value() : 0;
            };
            state.depth_enable = read_u32(at + d2.DepthEnable) != 0;
            state.depth_write_mask = read_u32(at + d2.DepthWriteMask);
            state.depth_func = read_u32(at + d2.DepthFunc);
            state.stencil_enable = read_u32(at + d2.StencilEnable) != 0;
            state.front = {read_u32(at + d2.FrontFace_StencilFailOp),
                           read_u32(at + d2.FrontFace_StencilDepthFailOp),
                           read_u32(at + d2.FrontFace_StencilPassOp),
                           read_u32(at + d2.FrontFace_StencilFunc)};
            state.back = {read_u32(at + d2.BackFace_StencilFailOp),
                          read_u32(at + d2.BackFace_StencilDepthFailOp),
                          read_u32(at + d2.BackFace_StencilPassOp),
                          read_u32(at + d2.BackFace_StencilFunc)};
            state.stencil_read_mask = byte(at + d2.FrontFace_StencilReadMask);
            state.stencil_write_mask = byte(at + d2.FrontFace_StencilWriteMask);
            state.depth_bounds_test =
                read_u32(at + d2.DepthBoundsTestEnable) != 0;
            if (state.stencil_enable &&
                (byte(at + d2.BackFace_StencilReadMask) !=
                     state.stencil_read_mask ||
                 byte(at + d2.BackFace_StencilWriteMask) !=
                     state.stencil_write_mask)) {
                m.set_return(refuse(E_INVALIDARG,
                                    "%s: DEPTH_STENCIL2 back-face stencil masks differ "
                                    "from the front (IndependentFrontAndBackStencil"
                                    "RefMaskSupported is FALSE)",
                                    api));
                return;
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            const auto& il = layout::D3D12_INPUT_LAYOUT_DESC[m_is_64bit];
            (void)read_pointer(at + il.pInputElementDescs, elements);
            element_count = read_u32(at + il.NumElements);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            state.strip_cut = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            state.topology_type = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            const auto& rt = layout::D3D12_RT_FORMAT_ARRAY[m_is_64bit];
            for (u32 index = 0; index < 8; ++index) {
                state.rtv_formats[index] =
                    read_u32(at + rt.RTFormats + index * 4);
            }
            state.num_render_targets = read_u32(at + rt.NumRenderTargets);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            state.dsv_format = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            state.sample_count = read_u32(at);
            state.sample_quality = read_u32(at + 4);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            pso->node_mask = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
            const auto& c = layout::D3D12_CACHED_PIPELINE_STATE[m_is_64bit];
            (void)read_pointer(at + c.pCachedBlob, src.cached_blob);
            (void)read_pointer(at + c.CachedBlobSizeInBytes, src.cached_size);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            pso->flags = read_u32(at);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            const auto& vi = layout::D3D12_VIEW_INSTANCING_DESC[m_is_64bit];
            src.view_instancing_present = true;
            src.view_instances = read_u32(at + vi.ViewInstanceCount);
            (void)read_pointer(at + vi.pViewInstanceLocations,
                               src.view_locations);
            src.view_flags = read_u32(at + vi.Flags);
            break;
        }
        default:
            break;
        }
        cursor = next;
    }
    pso->compute = src.cs != 0;
    report.input_elements = elements;
    report.input_element_count = element_count;
    if (pso->compute) {
        // A compute stream ignores the graphics defaults.
        state = GraphicsState{};
    } else if (u32 error = read_input_layout(m, elements, element_count, state);
               error != S_OK) {
        m.set_return(error);
        return;
    }
    if (u32 error = build_pipeline_unlocked(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (capture != nullptr) {
        // ID3D12PipelineLibrary's Load*: the library decides what to hand
        // out.
        *capture = std::move(pso);
        m.set_return(S_OK);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(first + 1), out));
}

void Frontend::pipeline_state_method(Marshaler& m, PipelineState& pso,
                                     usize slot) {
    switch (slot) {
    case PipelineStateSlot::GetCachedBlob: {
        // GetCachedBlob(ID3DBlob **ppBlob): the blob a later creation passes
        // back as its CachedPSO (measured, a05 pso: nonempty, accepted for
        // the same description, E_INVALIDARG for another). It names the
        // description; the translation is redone (a cache may be used, the
        // contract does not promise a faster creation).
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        (void)write_pointer(out, 0);
        std::array<u8, kCachedPsoBytes> blob{};
        std::memcpy(blob.data(), kCachedPsoMagic, 8);
        std::memcpy(blob.data() + 8, &kCachedPsoVersion, 4);
        std::memcpy(blob.data() + 16, &pso.cache_digest, 8);
        const GuestAddr address =
            m_create_blob ? m_create_blob(blob.data(), blob.size()) : 0;
        if (address == 0) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        (void)write_pointer(out, address);
        m.set_return(S_OK);
        return;
    }
    default:
        unimplemented("ID3D12PipelineState", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

}  // namespace vitreo::winapi::d3d12
