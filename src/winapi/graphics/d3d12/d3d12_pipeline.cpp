// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_pipeline.cpp
/// @brief ID3D12RootSignature and ID3D12PipelineState: root signature
/// creation (through the root-signature pass's parser), graphics and compute
/// pipeline state creation, shader translation, and the backend pipelines a
/// draw needs.
///
/// PLAN decision 1: shaders are DXBC (SM 4.0-5.1) through the existing
/// translator; DXIL fails with E_INVALIDARG and a one-time UNIMPLEMENTED line.
/// Every translated shader reads its registers from descriptor tables (Metal
/// Tier-2 argument buffers), one per register class: set 0 = b#, 1 = t#,
/// 2 = s#, 3 = u#, slot = register (PLAN decision 6).

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "core/memory/guest_memory.hpp"

#if __has_include("winapi/graphics/d3d12/d3d12_root_signature.hpp")
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#define VITREO_D3D12_ROOT_SIGNATURE_PARSER 1
#endif

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vitreo::winapi::d3d12 {
namespace {

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
        m.set_return(E_INVALIDARG);
        return;
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    if (m_memory.read_bytes(blob, bytes.data(), bytes.size()).is_err()) {
        m.set_return(E_INVALIDARG);
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
        return E_INVALIDARG;
    }
    if (pointer == 0 || length == 0) return S_OK;  // no shader at this stage
    if (length > 0x4000000ull) return E_INVALIDARG;
    out.resize(static_cast<usize>(length));
    if (m_memory.read_bytes(pointer, out.data(), out.size()).is_err()) {
        out.clear();
        return E_INVALIDARG;
    }
    return S_OK;
}

namespace {

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

/// Descriptors an unbounded SM 5.1 array is given when its root-signature
/// range is unbounded too: the table the pipeline is compiled against is
/// fixed, and one past this reads a null descriptor.
constexpr u32 UNBOUNDED_ARRAY_SLOTS = 1024;

}  // namespace

u32 Frontend::compile_shader(std::vector<u8> bytecode,
                             graphics::ShaderStage stage, CompiledShader& out,
                             const char* api, const RootLayout* root,
                             u32 visibility_stage,
                             const LogicOpVariant* logic) {
    out = CompiledShader{};
    if (bytecode.empty()) return S_OK;  // no shader at this stage
    out.bytecode = std::move(bytecode);
    if (fourcc_present(out.bytecode, "DXIL")) {
        // PLAN decision 1: SM 6 has no translator; the caps pass reports
        // D3D_SHADER_MODEL_5_1 so a guest that asks never sends DXIL.
        unimplemented(api, "DXIL");
        return E_INVALIDARG;
    }
    graphics::DxbcTranslationOptions options{};
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
        d3d12_diag("%s: not a whole DXBC container", api);
        return E_INVALIDARG;
    }
    if (!graphics::dxbc_container_verified(out.bytecode.data(),
                                           out.bytecode.size())) {
        d3d12_diag("%s: DXBC container hash mismatch", api);
        return E_FAIL;
    }
    const auto container =
        graphics::parse_dxbc(out.bytecode.data(), out.bytecode.size());
    if (!container.valid() || !container.is_sm4_plus() ||
        container.stage != stage) {
        d3d12_diag("%s: bytecode is not a DXBC shader of the stage", api);
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
        options.logic_op_targets = logic->targets;
        options.logic_op_component_masks = logic->component_masks;
    }
    std::array<u32, 4> next_slot{};
    if (container.is_sm51_plus()) {
        // Shader model 5.1 (register spaces, descriptor arrays): the
        // descriptor-table translation. Each declared range takes the next
        // slots of its class's table -- bounded ranges first, then at most one
        // unbounded array per class, which runs to the table's end
        // (dxbc_to_spirv.hpp DxbcRegisterRemap).
        out.table_mode = true;
        std::vector<const graphics::DxbcBindingDeclaration*> order;
        for (const auto& d : out.declarations) order.push_back(&d);
        std::stable_sort(order.begin(), order.end(),
                         [](const auto* a, const auto* b) {
                             return !a->unbounded() && b->unbounded();
                         });
        std::array<bool, 4> unbounded_seen{};
        for (const auto* d : order) {
            const u32 set = set_of_class(d->register_class);
            u32 count = d->count();
            if (d->unbounded()) {
                if (unbounded_seen[set]) {
                    unimplemented(api, "two unbounded arrays of one register "
                                       "class in one shader");
                    return E_INVALIDARG;
                }
                unbounded_seen[set] = true;
                count = UNBOUNDED_ARRAY_SLOTS;
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
            }
            graphics::DxbcRegisterRemap entry{};
            entry.register_class = d->register_class;
            entry.space = d->space;
            entry.first_register = d->lower;
            entry.register_count = d->unbounded() ? graphics::kDxbcUnboundedRange
                                                  : count;
            entry.descriptor_set = set;
            entry.first_slot = next_slot[set];
            options.register_remap.push_back(entry);
            out.runs[set].push_back(
                CompiledShader::Run{d->space, d->lower, count, next_slot[set]});
            next_slot[set] += count;
        }
        if (next_slot[2] != 0) {
            // MipLODBias and POINT-mip rounding of descriptor samplers: one
            // record per sampler slot, in the CBV table's last slot.
            options.sampler_state_buffer.enabled = true;
            options.sampler_state_buffer.descriptor_set = 0;
            options.sampler_state_buffer.binding = next_slot[0];
            out.sampler_state_slot = next_slot[0];
            ++next_slot[0];
        }
    } else {
        // Raw and structured views carry their size in the push block's size
        // table (member 2), which the replay fills from each bound
        // descriptor: bufinfo reads it, and a load or store past the view
        // returns 0 / is dropped (Learn, ByteAddressBuffer /
        // StructuredBuffer).
        options.guard_buffer_bounds = true;
    }
    auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!spirv.valid()) {
        std::fprintf(stderr, "[d3d12] %s: DXBC->SPIR-V failed: %s\n", api,
                     spirv.failure.c_str());
        return E_INVALIDARG;
    }
    if (out.table_mode) {
        out.registers.cbv_slots = next_slot[0];
        out.registers.srv_slots = next_slot[1];
        out.registers.sampler_slots = next_slot[2];
        out.registers.uav_slots = next_slot[3];
    } else {
        out.registers.cbv_slots = register_high_water(spirv.cbuffer_bindings);
        out.registers.srv_slots = register_high_water(spirv.texture_bindings);
        out.registers.sampler_slots = register_high_water(spirv.sampler_bindings);
        out.registers.uav_slots = UAV_TABLE_SLOTS;
    }
    if (!spirv.uav_counter_registers.empty()) {
        // Append/consume counters bind outside the u# table; the D3D12
        // counter resource (CounterResource + CounterOffsetInBytes) has no
        // binding path yet.
        unimplemented(api, "UAV counters (Append/Consume buffers)");
        return E_INVALIDARG;
    }
    graphics::MslCompileOptions msl{};
    msl.argument_buffers = true;
    const auto index = [](u32 set) {
        return graphics::BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    msl.argument_buffer_sets[0] = {out.registers.cbv_slots, index(0),
                                   graphics::MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {out.registers.srv_slots, index(1),
                                   graphics::MslDescriptorClass::Texture};
    msl.argument_buffer_sets[2] = {out.registers.sampler_slots, index(2),
                                   graphics::MslDescriptorClass::Sampler};
    msl.argument_buffer_sets[3] = {out.registers.uav_slots, index(3),
                                   graphics::MslDescriptorClass::Texture};
    // The logic-op variant reads each target through framebuffer fetch.
    msl.framebuffer_fetch = options.logic_op_targets != 0;
    if (spirv.uses_typed_uav_atomics && msl.msl_version < 30100u) {
        // SPIRV-Cross lowers typed UAV atomics natively only from MSL 3.1
        // (see D3D11's raise_msl_version_for_typed_uav_atomics).
        msl.msl_version = 30100u;
    }
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), stage, msl);
    if (!compiled.valid()) {
        std::fprintf(stderr, "[d3d12] %s: SPIR-V->MSL failed\n", api);
        return E_INVALIDARG;
    }
    out.present = true;
    out.msl = compiled.msl_text();
    out.threads_x = compiled.workgroup_size_x;
    out.threads_y = compiled.workgroup_size_y;
    out.threads_z = compiled.workgroup_size_z;
    // D3D11.3 3.1.3.1 / 22.3.2 / 22.19.2, as the D3D11 frontend applies them:
    // without refactoringAllowed, or with precise, the Safe math mode.
    const auto contract = graphics::dxbc_math_contract(container);
    out.math_mode = !contract.refactoring_allowed || contract.uses_precise
                        ? graphics::BackendMathMode::Safe
                        : graphics::BackendMathMode::Relaxed;
    out.preserve_invariance = contract.uses_precise;
    for (const auto& input : container.input_signature) {
        CompiledShader::Input in{};
        in.semantic = input.semantic_name;
        in.semantic_index = input.semantic_index;
        in.register_num = input.register_num;
        in.component_type = input.component_type;
        in.system_value = input.system_value_type;
        out.inputs.push_back(std::move(in));
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
    if (element_count != 0 && elements == 0) return E_INVALIDARG;
    state.input_layout.clear();
    for (u32 index = 0; index < element_count; ++index) {
        const GuestAddr at = elements + index * il.size;
        InputElement element{};
        GuestAddr name = 0;
        (void)read_pointer(at + il.SemanticName, name);
        const auto semantic = m.read_ansi_string(name, 256);
        if (name == 0 || !semantic.is_ok()) return E_INVALIDARG;
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

u32 Frontend::build_pipeline(const PipelineSource& src, PipelineState& pso,
                             const char* api) {
    const bool graphics = src.vs != 0 || src.ps != 0 || src.as != 0 ||
                          src.ms != 0 || src.ds != 0 || src.hs != 0 ||
                          src.gs != 0;
    if (pso.compute == graphics) {
        // A compute pipeline takes only CS; a graphics one no CS.
        return E_INVALIDARG;
    }
    if (graphics) {
        // Geometry, hull and domain stages: Metal has neither; the D3D11
        // frontend's compute lowering of them is not wired to D3D12.
        for (const GuestAddr stage : {src.ds, src.hs, src.gs}) {
            if (stage != 0 && shader_present(stage)) {
                unimplemented(api, "geometry / hull / domain shaders");
                return E_NOTIMPL;
            }
        }
        // D3D12_OPTIONS7 MeshShaderTier NOT_SUPPORTED (caps pass).
        for (const GuestAddr stage : {src.as, src.ms}) {
            if (stage != 0 && shader_present(stage)) {
                unimplemented(api, "amplification / mesh shaders");
                return E_INVALIDARG;
            }
        }
        if (src.stream_output_entries != 0) {
            unimplemented(api, "stream output");
            return E_NOTIMPL;
        }
        if (src.view_instances > 1) {
            // D3D12_OPTIONS3 ViewInstancingTier NOT_SUPPORTED (caps pass).
            unimplemented(api, "view instancing");
            return E_INVALIDARG;
        }
    }
    // D3D12_CACHED_PIPELINE_STATE: a blob GetCachedBlob handed out. Checked
    // once the pipeline is built (its digest is the description's).
    std::vector<u8> cached;
    if (src.cached_blob != 0 && src.cached_size != 0) {
        cached.resize(static_cast<usize>(std::min<u64>(src.cached_size, 64)));
        if (m_memory.read_bytes(src.cached_blob, cached.data(), cached.size())
                .is_err()) {
            return E_INVALIDARG;
        }
        if (src.cached_size != kCachedPsoBytes ||
            std::memcmp(cached.data(), kCachedPsoMagic, 8) != 0) {
            // Measured (a05 pso "CachedPSO garbage blob").
            return E_INVALIDARG;
        }
        u32 version = 0;
        std::memcpy(&version, cached.data() + 8, 4);
        if (version != kCachedPsoVersion) {
            // Learn: a blob from another driver version.
            return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
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
    if (u32 error = read_shader_bytes(graphics ? src.vs : src.cs, first_bytes);
        error != S_OK) {
        return error;
    }
    if (graphics && src.ps != 0) {
        if (u32 error = read_shader_bytes(src.ps, second_bytes); error != S_OK) {
            return error;
        }
    }
    // Learn: a graphics pipeline needs a vertex shader, a compute one a CS.
    if (first_bytes.empty()) return E_INVALIDARG;
    if (src.root != 0) {
        RootSignature* root = find<RootSignature>(src.root, Kind::RootSignature);
        if (root == nullptr) return E_INVALIDARG;
        pso.root = root->layout;
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
        if (!pso.root) return E_INVALIDARG;
    }
    if (graphics) {
        if (u32 error = compile_shader(std::move(first_bytes),
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
        if (u32 error = compile_shader(std::move(second_bytes),
                                       graphics::ShaderStage::Fragment, pso.ps,
                                       api, pso.root.get(),
                                       D3D12_SHADER_VISIBILITY_PIXEL, &logic);
            error != S_OK) {
            return error;
        }
        if (!root_covers(*pso.root, pso.vs, D3D12_SHADER_VISIBILITY_VERTEX) ||
            !root_covers(*pso.root, pso.ps, D3D12_SHADER_VISIBILITY_PIXEL)) {
            return E_INVALIDARG;
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
            return E_INVALIDARG;
        }
    }
    const u32 built = graphics ? finish_graphics_pipeline(pso, api)
                               : finish_compute_pipeline(pso, api);
    if (built != S_OK) return built;
    pso.cache_digest = pipeline_digest(pso, src.root);
    if (!cached.empty()) {
        u64 digest = 0;
        std::memcpy(&digest, cached.data() + 16, 8);
        // Measured (a05 pso "CachedPSO from GetCachedBlob, different desc").
        if (digest != pso.cache_digest) return E_INVALIDARG;
    }
    return S_OK;
}

u64 Frontend::pipeline_digest(const PipelineState& pso, GuestAddr root_address) {
    u64 h = fnv1a(&pso.compute, sizeof(pso.compute));
    for (const CompiledShader* shader : {&pso.vs, &pso.ps, &pso.cs}) {
        const u64 size = shader->bytecode.size();
        h = fnv1a(&size, sizeof(size), h);
        h = fnv1a(shader->bytecode.data(), shader->bytecode.size(), h);
    }
    if (const RootSignature* root =
            find<RootSignature>(root_address, Kind::RootSignature)) {
        h = fnv1a(&root->blob_hash, sizeof(root->blob_hash), h);
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
    mix(g.depth_clip); mix(g.multisample); mix(g.antialiased_line);
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
    return h;
}

void Frontend::create_graphics_pipeline_state(Marshaler& m, Device& device) {
    // CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
    //     REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device::CreateGraphicsPipelineState";
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[m_is_64bit];
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    GraphicsState& state = pso->graphics;
    PipelineSource src{};
    (void)read_pointer(desc + l.pRootSignature, src.root);
    src.vs = desc + l.VS;
    src.ps = desc + l.PS;
    src.ds = desc + l.DS;
    src.hs = desc + l.HS;
    src.gs = desc + l.GS;
    src.stream_output_entries = read_u32(desc + l.StreamOutput_NumEntries);
    read_blend(*this, desc + l.BlendState, state);
    state.sample_mask = read_u32(desc + l.SampleMask);
    read_rasterizer(*this, desc + l.RasterizerState, state);
    read_depth_stencil(*this, desc + l.DepthStencilState, false, state);
    GuestAddr elements = 0;
    (void)read_pointer(desc + l.InputLayout_pInputElementDescs, elements);
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
    if (u32 error = build_pipeline(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(2), out));
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
    // D3D12_RENDER_TARGET_BLEND_DESC: a logic op is for a UINT target and
    // excludes blending (measured, a05 pso "LogicOp XOR on RGBA8_UNORM",
    // "LogicOp and BlendEnable both"); without IndependentBlendEnable the
    // first target's description is every target's. Metal has no logic ops:
    // the pixel shader performs it on each such target through framebuffer
    // fetch (DxbcTranslationOptions::logic_op, as D3D11's variant does).
    out = LogicOpVariant{};
    bool any = false;
    for (u32 index = 0; index < 8; ++index) {
        const RenderTargetBlend& b =
            state.independent_blend ? state.blend[index] : state.blend[0];
        if (!b.logic_op_enable) continue;
        if (b.blend_enable || b.logic_op > D3D12_LOGIC_OP_OR_INVERTED) {
            return E_INVALIDARG;
        }
        if (index >= state.num_render_targets ||
            state.rtv_formats[index] == DXGI_FORMAT_UNKNOWN) {
            continue;
        }
        if (!uint_component_masks(
                graphics::pixel_format_from_dxgi(state.rtv_formats[index]),
                out.component_masks[index])) {
            return E_INVALIDARG;
        }
        if (any && b.logic_op != out.op) {
            unimplemented(api, "different logic ops on different targets");
            return E_INVALIDARG;
        }
        if (!caps().logic_ops) {
            unimplemented(api, "logic ops without framebuffer fetch");
            return E_INVALIDARG;
        }
        any = true;
        out.op = b.logic_op;
        out.targets |= 1u << index;
    }
    return S_OK;
}

u32 Frontend::finish_graphics_pipeline(PipelineState& pso, const char* api) {
    GraphicsState& state = pso.graphics;
    if (state.num_render_targets > 8 || state.sample_count == 0 ||
        state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
        state.topology_type > D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH ||
        state.fill_mode < D3D12_FILL_MODE_WIREFRAME ||
        state.fill_mode > D3D12_FILL_MODE_SOLID ||
        state.cull_mode < D3D12_CULL_MODE_NONE ||
        state.cull_mode > D3D12_CULL_MODE_BACK) {
        return E_INVALIDARG;
    }
    // PATCH needs a hull and a domain shader, which a pipeline here never
    // has (they fail loudly above): measured E_INVALIDARG without them.
    if (state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        return E_INVALIDARG;
    }
    if (state.conservative != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF) {
        // Reported unsupported by the caps pass.
        unimplemented(api, "conservative rasterization");
        return E_INVALIDARG;
    }
    // Measured (a05 pso): the strip-cut value is one of the three; no
    // format past NumRenderTargets; the pixel shader writes no SV_Target at
    // or past it; the depth / stencil functions and ops are valid when the
    // test is on; the sample quality is one the formats have.
    if (state.strip_cut > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) {
        return E_INVALIDARG;
    }
    for (u32 index = state.num_render_targets; index < 8; ++index) {
        if (state.rtv_formats[index] != DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
    }
    if (pso.ps.present &&
        (pso.ps.target_mask >> std::min(state.num_render_targets, 8u)) != 0) {
        return E_INVALIDARG;
    }
    const auto func_valid = [](u32 f) {
        return f >= D3D12_COMPARISON_FUNC_NEVER && f <= D3D12_COMPARISON_FUNC_ALWAYS;
    };
    const auto op_valid = [](u32 o) {
        return o >= D3D12_STENCIL_OP_KEEP && o <= D3D12_STENCIL_OP_DECR;
    };
    if (state.depth_enable &&
        (!func_valid(state.depth_func) ||
         state.depth_write_mask > D3D12_DEPTH_WRITE_MASK_ALL)) {
        return E_INVALIDARG;
    }
    if (state.stencil_enable) {
        for (const auto* face : {&state.front, &state.back}) {
            if (!func_valid(face->func) || !op_valid(face->fail) ||
                !op_valid(face->depth_fail) || !op_valid(face->pass)) {
                return E_INVALIDARG;
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
                return E_INVALIDARG;
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
        return E_INVALIDARG;
    }
    for (const auto& input : pso.vs.inputs) {
        if (input.system_value != 0) continue;
        const bool fed = std::any_of(
            state.input_layout.begin(), state.input_layout.end(),
            [&](const InputElement& e) {
                return e.semantic_index == input.semantic_index &&
                       semantic_equal(e.semantic, input.semantic);
            });
        if (!fed) return E_INVALIDARG;
    }
    for (const InputElement& a : state.input_layout) {
        for (const InputElement& b : state.input_layout) {
            if (a.input_slot == b.input_slot && a.slot_class != b.slot_class) {
                return E_INVALIDARG;
            }
        }
    }
    for (u32 index = 0; index < 8; ++index) {
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
            return E_INVALIDARG;
        }
        if (b.blend_enable &&
            (!blend_factor_valid(b.src_blend) ||
             !blend_factor_valid(b.dest_blend) ||
             !blend_factor_valid(b.src_blend_alpha) ||
             !blend_factor_valid(b.dest_blend_alpha) ||
             b.blend_op < D3D12_BLEND_OP_ADD || b.blend_op > D3D12_BLEND_OP_MAX ||
             b.blend_op_alpha < D3D12_BLEND_OP_ADD ||
             b.blend_op_alpha > D3D12_BLEND_OP_MAX)) {
            return E_INVALIDARG;
        }
    }
    for (u32 index = 0; index < 8; ++index) {
        const u32 format = state.rtv_formats[index];
        if (index >= state.num_render_targets) continue;
        if (format != DXGI_FORMAT_UNKNOWN &&
            graphics::pixel_format_from_dxgi(format) ==
                graphics::PixelFormat::Unknown) {
            return E_INVALIDARG;
        }
    }
    if (state.dsv_format != DXGI_FORMAT_UNKNOWN &&
        texture_format_for(state.dsv_format,
                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ==
            graphics::PixelFormat::Unknown) {
        return E_INVALIDARG;
    }
    if (!m_backend.supports_texture_sample_count(state.sample_count)) {
        return E_INVALIDARG;
    }
    // Map the input slots the layout reads onto the backend's vertex
    // streams, and resolve APPEND_ALIGNED_ELEMENT offsets.
    state.slot_stream.fill(0xFF);
    state.stream_slot.fill(0xFF);
    d3d11::VertexLayoutPackingState packing{};
    for (InputElement& element : state.input_layout) {
        if (element.input_slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
            d3d11::vertex_format_info(element.format) == nullptr ||
            element.slot_class > D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            return E_INVALIDARG;
        }
        u32 resolved = 0;
        if (!d3d11::resolve_vertex_element_offset(
                element.input_slot, element.aligned_offset,
                d3d11::vertex_format_stride_from_dxgi(element.format), packing,
                resolved)) {
            return E_INVALIDARG;
        }
        element.aligned_offset = resolved;
        if (state.slot_stream[element.input_slot] == 0xFF) {
            if (state.stream_count >= graphics::BACKEND_MAX_VERTEX_BUFFERS) {
                unimplemented(api, "more distinct input slots than backend "
                                   "vertex streams");
                return E_NOTIMPL;
            }
            state.slot_stream[element.input_slot] =
                static_cast<u8>(state.stream_count);
            state.stream_slot[state.stream_count] =
                static_cast<u8>(element.input_slot);
            ++state.stream_count;
        }
    }
    return S_OK;
}

graphics::BackendPipeline* Frontend::pipeline_variant(
    PipelineState& pso,
    const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides) {
    u64 key = 1469598103934665603ull;
    for (u32 stride : strides) key = (key ^ stride) * 1099511628211ull;
    if (auto found = pso.variants.find(key); found != pso.variants.end()) {
        return found->second.get();
    }
    const GraphicsState& state = pso.graphics;
    graphics::BackendPipelineDesc desc{};
    // The vertex attributes: each input element matched to the vertex
    // shader's input register by semantic (D3D12_INPUT_ELEMENT_DESC), and
    // the translated shader specialised to the element's storage type.
    graphics::DxbcTranslationOptions vs_options{};
    vs_options.guard_buffer_bounds = true;
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
    for (unsigned stream = 0; stream < strides.size(); ++stream) {
        desc.vertex_buffer_strides[stream] = strides[stream];
        // A stream the layout reads with stride 0 is a constant stream
        // (every vertex and instance reads the element at the offset, as the
        // D3D11 frontend measured for IASetVertexBuffers).
        if (strides[stream] == 0 && inferred[stream] != 0) {
            desc.vertex_buffer_constant[stream] = true;
            desc.vertex_buffer_strides[stream] = inferred[stream];
        }
    }
    desc.vertex_stride = desc.vertex_buffer_strides[0];
    std::string vertex_msl = pso.vs.msl;
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
    desc.fragment_shader_msl = pso.ps.present ? pso.ps.msl : std::string();
    desc.vertex_math_mode = pso.vs.math_mode;
    desc.fragment_math_mode = pso.ps.math_mode;
    desc.preserve_invariance = pso.vs.preserve_invariance;
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
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
    desc.input_primitive_class = primitive_class(state.topology_type);
    desc.attachmentless = state.num_render_targets == 0 &&
                          state.dsv_format == DXGI_FORMAT_UNKNOWN;
    // Blend: RenderTarget[0] applies to every target unless independent
    // blending is on (Learn, D3D12_BLEND_DESC).
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
    desc.cache_key = key ^ pso.serial;
    auto pipeline = m_backend.create_pipeline(desc);
    if (!pipeline || !pipeline->valid()) {
        d3d12_diag("backend refused the render pipeline of PSO 0x%llx",
                   static_cast<unsigned long long>(pso.address));
        return nullptr;
    }
    graphics::BackendPipeline* raw = pipeline.get();
    pso.variants.emplace(key, std::move(pipeline));
    return raw;
}

void Frontend::create_compute_pipeline_state(Marshaler& m, Device& device) {
    // CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
    //     REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device::CreateComputePipelineState";
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& l = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[m_is_64bit];
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    pso->compute = true;
    PipelineSource src{};
    (void)read_pointer(desc + l.pRootSignature, src.root);
    src.cs = desc + l.CS;
    pso->node_mask = read_u32(desc + l.NodeMask);
    pso->flags = read_u32(desc + l.Flags);
    (void)read_pointer(desc + l.CachedPSO_pCachedBlob, src.cached_blob);
    (void)read_pointer(desc + l.CachedPSO_CachedBlobSizeInBytes,
                       src.cached_size);
    if (u32 error = build_pipeline(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(2), out));
}

u32 Frontend::finish_compute_pipeline(PipelineState& pso, const char* api) {
    graphics::BackendComputePipelineDesc desc{};
    desc.compute_shader_msl = pso.cs.msl;
    desc.compute_entry_point = "main0";
    desc.math_mode = pso.cs.math_mode;
    pso.compute_pipeline = m_backend.create_compute_pipeline(desc);
    if (!pso.compute_pipeline || !pso.compute_pipeline->valid()) {
        d3d12_diag("%s: backend refused the compute pipeline", api);
        return E_INVALIDARG;
    }
    return S_OK;
}

void Frontend::create_pipeline_state(Marshaler& m, Device& device) {
    // ID3D12Device2::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC
    //     *pDesc, REFIID riid, void **ppPipelineState).
    constexpr const char* api = "ID3D12Device2::CreatePipelineState";
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& sl = layout::D3D12_PIPELINE_STATE_STREAM_DESC[m_is_64bit];
    GuestAddr stream_size = 0;
    GuestAddr stream = 0;
    (void)read_pointer(desc + sl.SizeInBytes, stream_size);
    (void)read_pointer(desc + sl.pPipelineStateSubobjectStream, stream);
    if (stream_size == 0 || stream == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto pso = std::make_unique<PipelineState>();
    pso->device = device.address;
    GraphicsState& state = pso->graphics;
    default_graphics_state(state);
    PipelineSource src{};
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
            m.set_return(E_INVALIDARG);
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
            d3d12_diag("%s: unknown subobject type %u", api, type);
            m.set_return(E_INVALIDARG);
            return;
        }
        // Learn (D3D12_PIPELINE_STATE_STREAM_DESC): each subobject type at
        // most once.
        if ((seen & (1ull << type)) != 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        seen |= 1ull << type;
        const u64 inner_offset = align(cursor + 4, inner_align);
        const u64 next = align(inner_offset + inner_size, pointer);
        if (inner_offset + inner_size > stream_size) {
            m.set_return(E_INVALIDARG);
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
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            src.stream_output_entries = read_u32(
                at + layout::D3D12_STREAM_OUTPUT_DESC[m_is_64bit].NumEntries);
            break;
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
                m.set_return(E_INVALIDARG);
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
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            src.view_instances = read_u32(
                at +
                layout::D3D12_VIEW_INSTANCING_DESC[m_is_64bit].ViewInstanceCount);
            break;
        default:
            break;
        }
        cursor = next;
    }
    pso->compute = src.cs != 0;
    if (pso->compute) {
        // A compute stream ignores the graphics defaults.
        state = GraphicsState{};
    } else if (u32 error = read_input_layout(m, elements, element_count, state);
               error != S_OK) {
        m.set_return(error);
        return;
    }
    if (u32 error = build_pipeline(src, *pso, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(pso), m.arg_ptr(2), out));
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
