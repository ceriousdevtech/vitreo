// SPDX-License-Identifier: LGPL-2.1-only

/// @file spirv_to_msl.cpp
/// @brief SPIR-V → MSL compiler implementation using SPIRV-Cross.

#include "spirv_to_msl.hpp"

#include <algorithm>
#include <map>

// Suppress warnings from SPIRV-Cross headers
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include <spirv_msl.hpp>
#pragma clang diagnostic pop
#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"

namespace vitreo::graphics {

static_assert(SPIRV_MAX_UAV_COUNTERS == BACKEND_MAX_UAV_COUNTER_BINDINGS,
              "the translator's counter limit must be the backend's");

// =============================================================================
// Helpers
// =============================================================================

bool msl_memory_fence_available() noexcept {
    // VITREO_MSL_NO_FENCE=1: refuse divergent fences as before (bisection
    // knob while a GPU hang is being isolated).
    static const bool disabled = std::getenv("VITREO_MSL_NO_FENCE") != nullptr;
    if (disabled) return false;
    // MSL 3.2 (macOS 15) added atomic_thread_fence with a thread_scope, the
    // only Metal fence that does not also rendezvous the threadgroup.
    // Measured: MSL 3.0/3.1 reject memory_order_seq_cst / thread_scope_device;
    // 3.2 compiles it inside divergent control flow. The Metal backend
    // compiles with default options, which select the newest version.
    if (__builtin_available(macOS 15.0, *)) return true;
    return false;
}

namespace {

/// SPIRV-Cross 1.3.283 lowers OpMemoryBarrier to threadgroup_barrier
/// (CompilerMSL::emit_barrier), which is also an EXECUTION barrier: every
/// thread of the group must reach it. D3D's sync without _t (and SPIR-V's
/// OpMemoryBarrier) is a fence only, legal in divergent control flow, where a
/// threadgroup_barrier is undefined and can hang the group. Emit the real
/// fence instead.
class FencingCompilerMSL final : public spirv_cross::CompilerMSL {
public:
    using CompilerMSL::CompilerMSL;

    /// The precise multiply emit_instruction lowers NoContraction OpFMul to.
    /// Declared in the header (before `using namespace metal`), optnone like
    /// SPIRV-Cross's own spvFMul so the Metal compiler cannot fuse it.
    static constexpr const char* kPreciseMultiply = "vitreoFMul";
    void declare_precise_multiply() {
        add_header_line(
            "template<typename T> [[clang::optnone]] T vitreoFMul(T l, T r) "
            "{ return metal::fma(l, r, T(-0.0)); }");
    }

    /// MslCompileOptions::precise_without_helpers. Without it every precise
    /// add, subtract and multiply is a call to an optnone function, which
    /// the Metal compiler can neither inline into its neighbours nor
    /// schedule across. Pixel shaders here are ~140 of them each.
    /// The operators are exactly the helpers' results -- fma(1, l, r) is
    /// l + r, fma(-1, r, l) is l - r, fma(l, r, -0) is l * r with its sign of
    /// zero -- once nothing fuses them, and the pragma is what stops Metal
    /// fusing: its Safe mode alone still contracts a*b+c. Only for modules
    /// that have a precise op, so a shader without one keeps its FMAs.
    void lower_precise_to_plain_operators() {
        for (const auto& [id, meta] : ir.meta) {
            (void)id;
            if (meta.decoration.decoration_flags.get(
                    spv::DecorationNoContraction)) {
                add_header_line("#pragma clang fp contract(off)");
                m_precise_plain = true;
                return;
            }
        }
    }
    bool m_precise_plain = false;

    /// Give the unbounded descriptor array `variable` (OpTypeRuntimeArray)
    /// the fixed `length` its argument-buffer table leaves it. SPIRV-Cross
    /// sizes a runtime array of images from the resource binding's count,
    /// but declares a runtime array of BUFFERS as `T* name[1]` whatever the
    /// count (CompilerGLSL::to_array_size), which lets the Metal compiler
    /// assume every index is 0; a literal length is right for both.
    void set_descriptor_array_length(uint32_t variable, uint32_t length) {
        auto& var = get<spirv_cross::SPIRVariable>(variable);
        for (uint32_t type_id = var.basetype; type_id != 0;) {
            auto& type = get<spirv_cross::SPIRType>(type_id);
            if (type.array.empty()) break;
            if (type.array_size_literal.back() && type.array.back() == 0u) {
                type.array.back() = length;
            }
            if (!type.pointer) break;
            type_id = type.parent_type;
        }
    }

protected:
    /// SPIRV-Cross 1.3.283 pads an argument buffer up to each member's
    /// [[id]] and then advances past the member by the count of the FIRST
    /// padded slot's binding -- 1 -- rather than the member's own, so the
    /// padding that follows a descriptor array re-declares the ids inside it
    /// (Metal: "attribute 'id' set location to 3, but minimum is 10"). Those
    /// padding members lie within the array's own slots and are dropped
    /// here; every other member keeps its id, and the struct its layout of
    /// one 8-byte descriptor per slot.
    void emit_struct_member(const spirv_cross::SPIRType& type,
                            uint32_t member_type_id, uint32_t index,
                            const std::string& qualifier,
                            uint32_t base_offset) override {
        if (padding_inside_descriptor_array(type, index)) return;
        CompilerMSL::emit_struct_member(type, member_type_id, index,
                                        qualifier, base_offset);
    }

    bool padding_inside_descriptor_array(const spirv_cross::SPIRType& type,
                                         uint32_t index) {
        const std::string name = get_member_name(type.self, index);
        if (name.size() < 4 || name.compare(name.size() - 4, 4, "_pad") != 0 ||
            get_name(type.self).rfind("spvDescriptorSetBuffer", 0) != 0) {
            return false;
        }
        const auto id_of = [&](uint32_t member) {
            return get_extended_member_decoration(
                type.self, member,
                spirv_cross::SPIRVCrossDecorationResourceIndexPrimary);
        };
        const uint32_t id = id_of(index);
        for (uint32_t member = 0; member < index; ++member) {
            const auto& member_type = get<spirv_cross::SPIRType>(
                type.member_types[member]);
            if (member_type.array.empty() ||
                !member_type.array_size_literal.back()) {
                continue;
            }
            const uint32_t first = id_of(member);
            const uint32_t length = member_type.array.back();
            if (id > first && id < first + length) return true;
        }
        return false;
    }

    /// SPIRV-Cross reads the coverage input of a sample-rate fragment shader
    /// as `gl_SampleMaskIn & (1 << gl_SampleID)` (Vulkan's per-sample
    /// SampleMask). A SampleMask input marked kRasterCoverageSemantic is
    /// D3D's vCoverage, the pixel's raster coverage, which is Metal's
    /// [[sample_mask]] unchanged in every per-sample invocation (measured on
    /// Apple9).
    std::string builtin_to_glsl(spv::BuiltIn builtin,
                                spv::StorageClass storage) override {
        if (builtin == spv::BuiltInSampleMask &&
            storage == spv::StorageClassInput && raster_coverage_input()) {
            return CompilerGLSL::builtin_to_glsl(builtin, storage);
        }
        return CompilerMSL::builtin_to_glsl(builtin, storage);
    }

    bool raster_coverage_input() {
        if (m_raster_coverage < 0) {
            m_raster_coverage = 0;
            ir.for_each_typed_id<spirv_cross::SPIRVariable>(
                [&](uint32_t id, spirv_cross::SPIRVariable& var) {
                    if (var.storage == spv::StorageClassInput &&
                        has_decoration(id, spv::DecorationBuiltIn) &&
                        get_decoration(id, spv::DecorationBuiltIn) ==
                            spv::BuiltInSampleMask &&
                        get_decoration_string(id,
                                              spv::DecorationUserSemantic) ==
                            kRasterCoverageSemantic) {
                        m_raster_coverage = 1;
                    }
                });
        }
        return m_raster_coverage == 1;
    }
    int m_raster_coverage = -1;

    void emit_instruction(const spirv_cross::Instruction& instruction) override {
        const auto op = static_cast<spv::Op>(instruction.op);
        if (m_precise_plain && !msl_options.invariant_float_math &&
            (op == spv::OpFAdd || op == spv::OpFSub || op == spv::OpFMul)) {
            const uint32_t* ops = stream(instruction);
            if (has_decoration(ops[1], spv::DecorationNoContraction)) {
                emit_binary_op(ops[0], ops[1], ops[2], ops[3],
                               op == spv::OpFAdd   ? "+"
                               : op == spv::OpFSub ? "-"
                                                   : "*");
                return;
            }
        }
        // SPIRV-Cross 1.3.283 lowers a NoContraction OpFMul to its
        // spvFMul(l, r) = fma(l, r, T(0)), and -0 + +0 is +0: every product
        // that should be -0 (-1 * 0) comes out +0, so a later 1/x is +inf
        // instead of -inf (D3D11.3 3.1.3.1: IEEE-754 multiply). Measured:
        // a D3D11 pixel shader's precise `mul r1.zw, -1, 0` fed
        // 1/r1.w; WARP -inf, Vitreo +inf (tools/oracle_probe/dxbc_truncate.py
        // differential). -0 is the exact additive identity, so
        // fma(l, r, -0.0) is the product for every input.
        if (static_cast<spv::Op>(instruction.op) == spv::OpFMul) {
            const uint32_t* ops = stream(instruction);
            if (msl_options.invariant_float_math ||
                has_decoration(ops[1], spv::DecorationNoContraction)) {
                emit_binary_func_op(ops[0], ops[1], ops[2], ops[3],
                                    kPreciseMultiply);
                return;
            }
        }
        if (static_cast<spv::Op>(instruction.op) != spv::OpMemoryBarrier ||
            get_execution_model() != spv::ExecutionModelGLCompute ||
            !msl_memory_fence_available()) {
            CompilerMSL::emit_instruction(instruction);
            return;
        }
        const uint32_t* ops = stream(instruction);
        const uint32_t scope = evaluate_constant_u32(ops[0]);
        const uint32_t semantics = evaluate_constant_u32(ops[1]);
        std::string flags;
        const auto add = [&](const char* flag) {
            if (!flags.empty()) flags += " | ";
            flags += flag;
        };
        if (semantics & (spv::MemorySemanticsUniformMemoryMask |
                         spv::MemorySemanticsCrossWorkgroupMemoryMask)) {
            add("mem_flags::mem_device");
        }
        if (semantics & (spv::MemorySemanticsSubgroupMemoryMask |
                         spv::MemorySemanticsWorkgroupMemoryMask)) {
            add("mem_flags::mem_threadgroup");
        }
        if (semantics & spv::MemorySemanticsImageMemoryMask) {
            add("mem_flags::mem_texture");
        }
        if (flags.empty()) flags = "mem_flags::mem_none";
        const char* thread_scope =
            scope <= spv::ScopeDevice ? "thread_scope_device"
            : scope == spv::ScopeWorkgroup ? "thread_scope_threadgroup"
                                           : "thread_scope_simdgroup";
        statement("atomic_thread_fence(", flags, ", memory_order_seq_cst, ",
                  thread_scope, ");");
        // Same bookkeeping CompilerMSL::emit_barrier does: values cached in
        // registers must be re-read after the fence.
        flush_control_dependent_expressions(current_emitting_block->self);
        flush_all_active_variables();
    }
};


spv::ExecutionModel stage_to_execution_model(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:        return spv::ExecutionModelVertex;
    case ShaderStage::Fragment:      return spv::ExecutionModelFragment;
    case ShaderStage::Compute:       return spv::ExecutionModelGLCompute;
    case ShaderStage::Geometry:      return spv::ExecutionModelGeometry;
    case ShaderStage::Hull:          return spv::ExecutionModelTessellationControl;
    case ShaderStage::Domain:        return spv::ExecutionModelTessellationEvaluation;
    case ShaderStage::Mesh:          return spv::ExecutionModelMeshEXT;
    case ShaderStage::Amplification: return spv::ExecutionModelTaskEXT;
    }
    return spv::ExecutionModelVertex;
}

void extract_bindings(
    const spirv_cross::CompilerMSL& compiler,
    const spirv_cross::ShaderResources& resources,
    std::vector<ResourceBinding>& out)
{
    // Constant buffers (UBOs)
    for (const auto& r : resources.uniform_buffers) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({BindingType::ConstantBuffer, set, binding, 1, mtl});
    }

    // Shader resources (SRVs / sampled images)
    for (const auto& r : resources.sampled_images) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({BindingType::ShaderResource, set, binding, 1, mtl});
    }

    for (const auto& r : resources.separate_images) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({BindingType::ShaderResource, set, binding, 1, mtl});
    }

    // Storage buffers. Descriptor set 1 is the D3D t# namespace, so a storage
    // buffer there is a StructuredBuffer *SRV*, not a UAV — the two live in
    // separate D3D register namespaces and are bound by different API calls.
    for (const auto& r : resources.storage_buffers) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({set == 1u ? BindingType::ShaderResource
                                 : BindingType::UnorderedAccess,
                       set, binding, 1, mtl});
    }

    // Storage images (UAV textures)
    for (const auto& r : resources.storage_images) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({BindingType::UnorderedAccess, set, binding, 1, mtl});
    }

    // Samplers
    for (const auto& r : resources.separate_samplers) {
        auto set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        auto mtl = compiler.get_automatic_msl_resource_binding(r.id);
        out.push_back({BindingType::Sampler, set, binding, 1, mtl});
    }
}

void extract_io(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::SmallVector<spirv_cross::Resource>& stage_io,
    std::vector<ShaderIO>& out,
    bool is_input)
{
    for (const auto& r : stage_io) {
        ShaderIO io{};
        io.location = compiler.get_decoration(r.id, spv::DecorationLocation);
        io.semantic_index = static_cast<u8>(io.location);

        // Try to determine semantic from name
        auto name = compiler.get_name(r.id);
        if (name.find("POSITION") != std::string::npos || name.find("position") != std::string::npos) {
            io.semantic = SemanticName::Position;
        } else if (name.find("NORMAL") != std::string::npos || name.find("normal") != std::string::npos) {
            io.semantic = SemanticName::Normal;
        } else if (name.find("TEXCOORD") != std::string::npos || name.find("texcoord") != std::string::npos) {
            io.semantic = SemanticName::Texcoord;
        } else if (name.find("COLOR") != std::string::npos || name.find("color") != std::string::npos) {
            io.semantic = SemanticName::Color;
        } else if (name.find("TANGENT") != std::string::npos || name.find("tangent") != std::string::npos) {
            io.semantic = SemanticName::Tangent;
        } else {
            io.semantic = is_input ? SemanticName::Texcoord : SemanticName::Target;
        }

        // Component count from type
        auto& type = compiler.get_type(r.type_id);
        io.component_count = static_cast<u8>(type.vecsize);

        out.push_back(io);
    }
}

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

CompiledShader compile_spirv_to_msl(
    const u32* spirv_data,
    usize spirv_word_count,
    ShaderStage stage,
    const MslCompileOptions& options)
{
    CompiledShader result{};
    result.stage = stage;
    result.workgroup_size_x = 1;
    result.workgroup_size_y = 1;
    result.workgroup_size_z = 1;
    result.uses_push_constants = false;

    if (!spirv_data || spirv_word_count == 0) {
        return result;
    }

    // Metal has no geometry-shader stage. SPIRV-Cross currently prints an
    // `unknown` entry-point qualifier plus unresolved EmitVertex helpers for
    // this execution model, which is not compilable MSL. Keep the backend
    // boundary honest until a separate, explicit GS emulation path exists.
    if (stage == ShaderStage::Geometry) {
        return result;
    }

    // SPIRV-Cross cannot express demotion before MSL 2.3 and throws on
    // this instruction. Refuse explicitly in our exception-free build.
    if (options.msl_version < 20300u) {
        for (usize i = 5; i < spirv_word_count;) {
            const u32 words = spirv_data[i] >> 16;
            if (words == 0 || words > spirv_word_count - i) return result;
            if ((spirv_data[i] & 0xFFFFu) ==
                static_cast<u32>(spv::OpDemoteToHelperInvocationEXT)) {
                VITREO_TRACE("[msl] pixel discard requires MSL 2.3\n");
                return result;
            }
            i += words;
        }
    }

    // Create SPIRV-Cross MSL compiler
    std::vector<u32> spirv_vec(spirv_data, spirv_data + spirv_word_count);
    FencingCompilerMSL compiler(std::move(spirv_vec));
    compiler.declare_precise_multiply();
    if (options.precise_without_helpers) {
        compiler.lower_precise_to_plain_operators();
    }

    // Configure MSL options
    auto msl_opts = compiler.get_msl_options();
    msl_opts.msl_version = options.msl_version;
    // Metal promises discarded helpers cannot write resources, but Cross
    // documents devices that violate it. Its guard is emitted only for
    // fragment shaders combining discard with storage writes/atomics.
    msl_opts.check_discarded_frag_stores = true;
    // A D3D typed buffer (`Buffer<T>` / SPIR-V Dim=Buffer) is a texture, not a
    // storage buffer: the hardware decodes the view's format. Metal expresses
    // exactly that as `texture_buffer<T>` from MSL 2.1. Without this SPIRV-Cross
    // emulates the type over a plain buffer and loses the format conversion.
    msl_opts.texture_buffer_native = true;
    msl_opts.platform = static_cast<spirv_cross::CompilerMSL::Options::Platform>(
        options.platform);
    msl_opts.argument_buffers = options.argument_buffers;
    msl_opts.force_native_arrays = options.force_native_arrays;
    msl_opts.pad_fragment_output_components = options.pad_fragment_output;
    msl_opts.use_framebuffer_fetch_subpasses = options.framebuffer_fetch;
    msl_opts.buffer_size_buffer_index = options.buffer_size_buffer_index;
    if (options.argument_buffers) {
        // Tier 2 is what a descriptor table needs: higher resource counts, and
        // writable images inside the argument buffer, which Tier 1 refuses on
        // iOS and which D3D's u# space requires.
        msl_opts.argument_buffers_tier =
            spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        // The two options that make the layout a CONTRACT instead of a
        // consequence of which resources this particular shader happens to use.
        //
        // D3D register spaces are fixed: t0..t127 exist whether or not a shader
        // reads them, and the host binds into absolute slot numbers. Without
        // padding, SPIRV-Cross packs only the declared resources, so a shader
        // reading t0 and t9 would put t9 at id(1) and silently read whatever the
        // host encoded for t1. Padding gives every declared slot its own id, and
        // forcing resources active keeps a slot occupied even when the shader
        // never references it, so two shaders over the same table agree.
        msl_opts.pad_argument_buffer_resources = true;
        msl_opts.force_active_argument_buffer_resources = true;
    }
    compiler.set_msl_options(msl_opts);


    // Set entry point
    auto entry_points = compiler.get_entry_points_and_stages();
    auto target_model = stage_to_execution_model(stage);
    std::string entry_name = "main0";
    for (const auto& ep : entry_points) {
        if (ep.execution_model == target_model) {
            compiler.set_entry_point(ep.name, ep.execution_model);
            entry_name = ep.name;
            break;
        }
    }

    // Descriptor arrays (the DXBC translator's shader model 5.1 ranges):
    // (set, binding) -> descriptor count, 0 for a runtime (unbounded) array.
    // A runtime array has no length of its own; SPIRV-Cross takes it from the
    // resource binding's count, which only an argument-buffer table can give
    // (the descriptors from its binding to the end of the table). Without
    // one it would emit a descriptor POINTER no backend binds -- or, in the
    // flat model, throw -- so such a shader is refused here.
    std::map<std::pair<u32, u32>, u32> descriptor_arrays;
    std::map<std::pair<u32, u32>, u32> descriptor_array_variables;
    {
        const auto resources = compiler.get_shader_resources();
        const auto note_arrays = [&](const auto& list) {
            for (const auto& resource : list) {
                const auto& type = compiler.get_type(resource.type_id);
                if (type.array.empty()) continue;
                const u32 set = compiler.get_decoration(
                    resource.id, spv::DecorationDescriptorSet);
                const u32 binding =
                    compiler.get_decoration(resource.id, spv::DecorationBinding);
                // A literal length, or the id of the OpConstant holding it;
                // OpTypeRuntimeArray reads as literal 0.
                const u32 length = type.array_size_literal.back()
                    ? type.array.back()
                    : compiler.get_constant(type.array.back()).scalar();
                descriptor_arrays[{set, binding}] = length;
                descriptor_array_variables[{set, binding}] = resource.id;
            }
        };
        note_arrays(resources.uniform_buffers);
        note_arrays(resources.storage_buffers);
        note_arrays(resources.separate_images);
        note_arrays(resources.storage_images);
        note_arrays(resources.separate_samplers);
        // An atomic on an element of a storage-image array: SPIRV-Cross
        // emulates image atomics below MSL 3.1 and cannot for an array (it
        // aborts the process, "Cannot emulate array of storage images with
        // atomics"); the translator flags such a shader
        // (DxbcToSpirvResult::uses_typed_uav_atomics) for 3.1.
        bool image_arrays = false;
        for (const auto& resource : resources.storage_images) {
            if (!compiler.get_type(resource.type_id).array.empty()) {
                image_arrays = true;
            }
        }
        if (image_arrays && options.msl_version < 30100u) {
            for (usize i = 5; i < spirv_word_count;) {
                const u32 words = spirv_data[i] >> 16;
                if (words == 0) break;
                if ((spirv_data[i] & 0xFFFFu) ==
                    static_cast<u32>(spv::OpImageTexelPointer)) {
                    VITREO_TRACE("[msl] atomics on a storage-image array "
                                 "need MSL 3.1\n");
                    return result;
                }
                i += words;
            }
        }
        for (const auto& [key, count] : descriptor_arrays) {
            if (count != 0u) continue;
            const u32 set = key.first;
            if (!options.argument_buffers || set >= MSL_DESCRIPTOR_SET_COUNT ||
                options.argument_buffer_sets[set].slot_count <= key.second) {
                VITREO_TRACE("[msl] unbounded descriptor array at set %u "
                             "binding %u has no argument-buffer table to "
                             "size it\n", set, key.second);
                return result;
            }
        }
    }

    {
        // The push-constant block always binds where the host puts it; see
        // MslCompileOptions::push_constant_buffer_index.
        spirv_cross::MSLResourceBinding push{};
        push.stage = target_model;
        push.desc_set = spirv_cross::kPushConstDescSet;
        push.binding = spirv_cross::kPushConstBinding;
        push.msl_buffer = options.push_constant_buffer_index;
        push.basetype = spirv_cross::SPIRType::Float;
        push.count = 1;
        compiler.add_msl_resource_binding(push);
    }

    if (options.argument_buffers) {
        // Reflection high-water can be lower than a binding this SPIR-V
        // actually declares (a texture the declaration loop emitted, or a
        // buffer sharing the t# space). Padding then asks for that index and
        // aborts because nothing registered a base type for it. Grow each
        // table to the highest real binding. Sentinel bindings (swizzle,
        // push constants) are not D3D slots.
        std::array<u32, MSL_DESCRIPTOR_SET_COUNT> slot_count{};
        for (u32 set = 0; set < MSL_DESCRIPTOR_SET_COUNT; ++set) {
            slot_count[set] = options.argument_buffer_sets[set].slot_count;
        }
        const auto resources = compiler.get_shader_resources();
        const auto note = [&](const auto& list) {
            for (const auto& resource : list) {
                if (!compiler.has_decoration(resource.id,
                                             spv::DecorationDescriptorSet) ||
                    !compiler.has_decoration(resource.id,
                                             spv::DecorationBinding)) {
                    continue;
                }
                const u32 set = compiler.get_decoration(
                    resource.id, spv::DecorationDescriptorSet);
                const u32 binding = compiler.get_decoration(
                    resource.id, spv::DecorationBinding);
                if (set >= MSL_DESCRIPTOR_SET_COUNT || binding >= 0x80000000u) {
                    continue;
                }
                // A set the caller left discrete (slot count 0) stays discrete.
                // Growing it into a table would bind resources the host never
                // encodes.
                if (options.argument_buffer_sets[set].slot_count == 0) continue;
                // A descriptor array spans its length (a runtime array ends
                // with the table, checked above).
                const auto array = descriptor_arrays.find({set, binding});
                const u32 span = array == descriptor_arrays.end()
                    ? 1u : std::max(1u, array->second);
                slot_count[set] = std::max(slot_count[set], binding + span);
            }
        };
        note(resources.uniform_buffers);
        note(resources.storage_buffers);
        note(resources.separate_images);
        note(resources.sampled_images);
        note(resources.storage_images);
        note(resources.separate_samplers);
        for (u32 set = 0; set < MSL_DESCRIPTOR_SET_COUNT; ++set) {
            const auto& layout = options.argument_buffer_sets[set];
            if (slot_count[set] == 0) {
                // A set with no declared table keeps its flat bindings. Say so
                // explicitly: SPIRV-Cross otherwise treats every set as an
                // argument buffer once the feature is on.
                compiler.add_discrete_descriptor_set(set);
                continue;
            }
            // Where this set's argument buffer itself binds.
            spirv_cross::MSLResourceBinding argument_buffer{};
            argument_buffer.stage = target_model;
            argument_buffer.desc_set = set;
            argument_buffer.binding = spirv_cross::kArgumentBufferBinding;
            argument_buffer.msl_buffer = layout.metal_buffer_index;
            // With padding on, SPIRV-Cross indexes every registered binding by
            // basetype and aborts the process on an unset one -- including this
            // one, which describes the argument buffer itself. An argument
            // buffer is a buffer, so a scalar basetype is both accepted and
            // accurate.
            argument_buffer.basetype = spirv_cross::SPIRType::Float;
            argument_buffer.count = 1;
            compiler.add_msl_resource_binding(argument_buffer);

            // Padding needs a basetype and count for every slot in the set,
            // declared or not; that is what pins slot N to id(N). A
            // descriptor array's binding counts its whole length, so padding
            // resumes past it and element k sits at id(binding + k).
            for (u32 slot = 0; slot < slot_count[set]; ++slot) {
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = set;
                binding.binding = slot;
                binding.count = 1;
                if (const auto array = descriptor_arrays.find({set, slot});
                    array != descriptor_arrays.end()) {
                    binding.count = array->second;
                    if (binding.count == 0u) {
                        // Unbounded: every slot from here to the end of the
                        // table.
                        binding.count = slot_count[set] - slot;
                        compiler.set_descriptor_array_length(
                            descriptor_array_variables.at({set, slot}),
                            binding.count);
                    }
                }
                // A t# register is a texture or a buffer, and SPIRV-Cross
                // reads a different index field for each. Leaving the unused
                // fields at 0 makes every buffer in the set report index 0, so
                // the second one asks padding for index 1 and the compiler
                // aborts. The slot number is the index in every namespace.
                binding.msl_buffer = slot;
                binding.msl_texture = slot;
                binding.msl_sampler = slot;
                switch (layout.slot_class) {
                case MslDescriptorClass::Texture:
                    binding.basetype = spirv_cross::SPIRType::Image;
                    binding.msl_texture = slot;
                    break;
                case MslDescriptorClass::Sampler:
                    binding.basetype = spirv_cross::SPIRType::Sampler;
                    binding.msl_sampler = slot;
                    break;
                case MslDescriptorClass::Buffer:
                    // Any scalar basetype selects buffer-shaped padding; the
                    // member is a pointer either way.
                    binding.basetype = spirv_cross::SPIRType::Float;
                    binding.msl_buffer = slot;
                    break;
                }
                compiler.add_msl_resource_binding(binding);
            }
        }
        // SPIRV-Cross treats every set below kMaxArgumentBuffers as an
        // argument buffer once the feature is on, and aborts if padding asks
        // for an index the app never gave a base type. Sets this compiler
        // does not build a table for — the geometry-expansion and vertex
        // capture set, and anything above the four D3D register spaces — stay
        // discrete, which is the binding model their MSL indices already use.
        for (u32 set = MSL_DESCRIPTOR_SET_COUNT;
             set < spirv_cross::kMaxArgumentBuffers; ++set) {
            compiler.add_discrete_descriptor_set(set);
        }
    }

    if (options.dual_source_blending) {
        if (stage != ShaderStage::Fragment) return result;
        bool has_second_source = false;
        for (const auto& output : compiler.get_shader_resources().stage_outputs) {
            if (!compiler.has_decoration(output.id, spv::DecorationLocation)) continue;
            const u32 location = compiler.get_decoration(output.id, spv::DecorationLocation);
            // D3D11 dual-source blending operates on RT0 only. Keep the
            // normal MRT translation untouched unless this variant is requested.
            if (location > 1) return result;
            compiler.set_decoration(output.id, spv::DecorationLocation, 0);
            compiler.set_decoration(output.id, spv::DecorationIndex, location);
            has_second_source |= location == 1;
        }
        if (!has_second_source) return result;
    }

    // The flat index windows below are what a shader binds at when its
    // descriptor sets are DISCRETE. In argument-buffer mode the same registers
    // are addressed by `[[id(n)]]` inside the set's buffer instead, so the
    // remapping is not just unnecessary -- registering these bindings without a
    // basetype aborts the process once pad_argument_buffer_resources is on.
    if (!options.argument_buffers) {
        // The MSL index windows for b#, t#, u# and the input-assembler streams are
        // one shared contract, defined and bounds-checked in
        // graphics/contracts/backend_device.hpp. Read them from there rather than
        // restating them: the backend binds at these indices and the frontends fill
        // their call structs at them, and a disagreement is a silent wrong-render.
        {
            auto resources = compiler.get_shader_resources();
            constexpr uint32_t k_msl_cb_base = BACKEND_CONSTANT_BUFFER_BASE;
            for (const auto& r : resources.uniform_buffers) {
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set =
                    compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
                binding.binding =
                    compiler.get_decoration(r.id, spv::DecorationBinding);
                binding.msl_buffer = k_msl_cb_base + binding.binding;
                compiler.add_msl_resource_binding(binding);
            }

            // D3D constant buffers, buffer SRVs, and buffer UAVs occupy independent
            // register namespaces. Metal combines all three into one buffer-index
            // namespace, so each class gets a disjoint window.
            constexpr uint32_t k_msl_srv_buffer_base = BACKEND_SRV_BUFFER_BASE;
            constexpr uint32_t k_msl_uav_buffer_base = BACKEND_UAV_BUFFER_BASE;
            // Descriptor set 4 is the geometry-shader expansion, which has
            // its own window. Leaving it out of this loop did not fail
            // loudly: SPIRV-Cross simply assigned indices of its own
            // choosing, which can land anywhere in the flat windows above.
            constexpr uint32_t k_msl_gs_buffer_base =
                BACKEND_GS_EXPANSION_BUFFER_BASE;
            for (const auto& r : resources.storage_buffers) {
                const uint32_t set =
                    compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
                if (set != 1u && set != 3u && set != 4u) continue;
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = set;
                binding.binding =
                    compiler.get_decoration(r.id, spv::DecorationBinding);
                // The capture vertex shader writes set 4 from a real draw
                // whose vertex buffers are bound, so the compute mapping would
                // put it on vertex buffer 0. It has exactly one buffer there.
                const bool vertex_capture =
                    set == 4u && target_model == spv::ExecutionModelVertex;
                if (vertex_capture && binding.binding != 0u) {
                    VITREO_TRACE("[msl] vertex stage declares expansion "
                                 "binding %u; only the capture buffer "
                                 "(binding 0) has an index\n",
                                 binding.binding);
                    return result;
                }
                binding.msl_buffer =
                    vertex_capture ? BACKEND_VERTEX_CAPTURE_BUFFER_INDEX
                    : (set == 1u   ? k_msl_srv_buffer_base
                       : set == 3u ? k_msl_uav_buffer_base
                                   : k_msl_gs_buffer_base) +
                          binding.binding;
                compiler.add_msl_resource_binding(binding);
            }

            // t# (set 1) and s# (set 2) keep their register numbers, which is
            // where every frontend binds them. Left unregistered, SPIRV-Cross
            // numbers them from 0 in declaration order: a pixel shader reading
            // only t3 with s2 compiled to texture(0) / sampler(0) and sampled
            // whatever t0 and s0 held -- a silent wrong render for any shader
            // whose registers do not start at 0.
            for (const auto& r : resources.separate_images) {
                if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                    1u) {
                    continue;
                }
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = 1u;
                binding.binding =
                    compiler.get_decoration(r.id, spv::DecorationBinding);
                binding.msl_texture = BACKEND_SRV_TEXTURE_BASE + binding.binding;
                compiler.add_msl_resource_binding(binding);
            }
            for (const auto& r : resources.separate_samplers) {
                if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                    2u) {
                    continue;
                }
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = 2u;
                binding.binding =
                    compiler.get_decoration(r.id, spv::DecorationBinding);
                binding.msl_sampler = binding.binding;
                compiler.add_msl_resource_binding(binding);
            }

            // D3D t# and u# are independent namespaces. Metal uses one texture
            // index namespace per stage, so SRVs keep the low window and
            // storage-image u# bindings are remapped above it.
            constexpr uint32_t k_msl_uav_texture_base = BACKEND_UAV_TEXTURE_BASE;
            for (const auto& r : resources.storage_images) {
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set =
                    compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
                binding.binding =
                    compiler.get_decoration(r.id, spv::DecorationBinding);
                binding.msl_texture = k_msl_uav_texture_base + binding.binding;
                compiler.add_msl_resource_binding(binding);
            }
        }
    }

    // A u# set left discrete in argument-buffer mode -- a pixel shader's, whose
    // UAVs arrive through OMSetRenderTargetsAndUnorderedAccessViews and bind
    // flat (see msl_options_for in the D3D11 frontend) -- still has to land
    // on the flat u# windows the backend binds. The block above is skipped in
    // this mode, and without these SPIRV-Cross numbers the UAVs itself: a
    // fragment `texture2d<..., access::read_write>` at texture(0) aliased t0.
    // Padding aborts on a binding without a basetype, so each carries one.
    if (options.argument_buffers &&
        options.argument_buffer_sets[3].slot_count == 0) {
        const auto resources = compiler.get_shader_resources();
        for (const auto& r : resources.storage_images) {
            if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                3u) {
                continue;
            }
            spirv_cross::MSLResourceBinding binding{};
            binding.stage = target_model;
            binding.desc_set = 3u;
            binding.binding =
                compiler.get_decoration(r.id, spv::DecorationBinding);
            binding.msl_texture = BACKEND_UAV_TEXTURE_BASE + binding.binding;
            binding.basetype = spirv_cross::SPIRType::Image;
            binding.count = 1;
            compiler.add_msl_resource_binding(binding);
        }
        for (const auto& r : resources.storage_buffers) {
            if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                3u) {
                continue;
            }
            spirv_cross::MSLResourceBinding binding{};
            binding.stage = target_model;
            binding.desc_set = 3u;
            binding.binding =
                compiler.get_decoration(r.id, spv::DecorationBinding);
            binding.msl_buffer = BACKEND_UAV_BUFFER_BASE + binding.binding;
            binding.basetype = spirv_cross::SPIRType::UInt;
            binding.count = 1;
            compiler.add_msl_resource_binding(binding);
        }
    }

    // UAV counters bind flat in both modes: they are one small buffer each,
    // never part of a D3D register space a table could hold. Only compute and
    // fragment have the window (see BACKEND_UAV_COUNTER_BUFFER_BASE); the
    // translator refuses a counter elsewhere, and this refuses again rather
    // than let SPIRV-Cross choose an index on a vertex stage that has none.
    {
        bool has_counters = false;
        for (const auto& r : compiler.get_shader_resources().storage_buffers) {
            if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                SPIRV_UAV_COUNTER_DESCRIPTOR_SET) {
                continue;
            }
            const u32 k = compiler.get_decoration(r.id, spv::DecorationBinding);
            if ((stage != ShaderStage::Compute &&
                 stage != ShaderStage::Fragment) ||
                k >= BACKEND_MAX_UAV_COUNTER_BINDINGS) {
                VITREO_TRACE("[msl] UAV counter binding %u has no index in "
                             "this stage\n", k);
                return result;
            }
            spirv_cross::MSLResourceBinding binding{};
            binding.stage = target_model;
            binding.desc_set = SPIRV_UAV_COUNTER_DESCRIPTOR_SET;
            binding.binding = k;
            binding.msl_buffer = BACKEND_UAV_COUNTER_BUFFER_BASE + k;
            // Argument-buffer mode aborts on a binding without a basetype.
            binding.basetype = spirv_cross::SPIRType::UInt;
            binding.count = 1;
            compiler.add_msl_resource_binding(binding);
            has_counters = true;
        }
        if (has_counters && options.argument_buffers) {
            compiler.add_discrete_descriptor_set(
                SPIRV_UAV_COUNTER_DESCRIPTOR_SET);
        }
    }

    // The PIPELINE_STATISTICS record of a counting pixel shader
    // (DxbcTranslationOptions::count_pixel_invocations) binds flat in both
    // modes, like the counters above: one small buffer, fragment only.
    {
        bool has_statistics = false;
        for (const auto& r : compiler.get_shader_resources().storage_buffers) {
            if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                SPIRV_STATISTICS_DESCRIPTOR_SET) {
                continue;
            }
            if (stage != ShaderStage::Fragment ||
                compiler.get_decoration(r.id, spv::DecorationBinding) != 0u) {
                VITREO_TRACE("[msl] statistics record has no index in this "
                             "stage\n");
                return result;
            }
            spirv_cross::MSLResourceBinding binding{};
            binding.stage = target_model;
            binding.desc_set = SPIRV_STATISTICS_DESCRIPTOR_SET;
            binding.binding = 0u;
            binding.msl_buffer = BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX;
            binding.basetype = spirv_cross::SPIRType::UInt;
            binding.count = 1;
            compiler.add_msl_resource_binding(binding);
            has_statistics = true;
        }
        if (has_statistics && options.argument_buffers) {
            compiler.add_discrete_descriptor_set(
                SPIRV_STATISTICS_DESCRIPTOR_SET);
        }
    }

    // Compile to MSL.
    // Vitreo uses -fno-exceptions + SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS, so
    // invalid SPIR-V aborts the host process. DXBC→SPIR-V must emit valid modules.
    std::string msl = compiler.compile();

    // Store MSL source
    result.msl_source.assign(msl.begin(), msl.end());
    result.msl_source.push_back(0); // null-terminate

    // Extract resource bindings
    auto resources = compiler.get_shader_resources();
    extract_bindings(compiler, resources, result.bindings);

    // Check for push constants
    result.uses_push_constants = !resources.push_constant_buffers.empty();

    // Extract input/output signatures
    extract_io(compiler, resources.stage_inputs, result.inputs, true);
    extract_io(compiler, resources.stage_outputs, result.outputs, false);

    // Extract workgroup size for compute shaders
    if (stage == ShaderStage::Compute) {
        auto wg = compiler.get_execution_mode_argument(
            spv::ExecutionModeLocalSize, 0);
        auto wg_y = compiler.get_execution_mode_argument(
            spv::ExecutionModeLocalSize, 1);
        auto wg_z = compiler.get_execution_mode_argument(
            spv::ExecutionModeLocalSize, 2);
        result.workgroup_size_x = wg > 0 ? wg : 1;
        result.workgroup_size_y = wg_y > 0 ? wg_y : 1;
        result.workgroup_size_z = wg_z > 0 ? wg_z : 1;
    }

    // Store intermediate SPIR-V
    result.spirv_binary.resize(spirv_word_count * sizeof(u32));
    std::memcpy(result.spirv_binary.data(), spirv_data,
                spirv_word_count * sizeof(u32));

    return result;
}

ShaderReflection reflect_spirv(
    const u32* spirv_data,
    usize spirv_word_count)
{
    ShaderReflection refl{};
    refl.push_constant_size = 0;
    refl.workgroup_size[0] = 1;
    refl.workgroup_size[1] = 1;
    refl.workgroup_size[2] = 1;

    if (!spirv_data || spirv_word_count == 0) {
        return refl;
    }

    std::vector<u32> spirv_vec(spirv_data, spirv_data + spirv_word_count);
    spirv_cross::Compiler compiler(std::move(spirv_vec));
    auto resources = compiler.get_shader_resources();

    // Constant buffers
    for (const auto& r : resources.uniform_buffers) {
        ConstantBufferReflection cb{};
        cb.name = compiler.get_name(r.id);
        cb.binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        cb.set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto& type = compiler.get_type(r.base_type_id);
        cb.size_bytes = static_cast<u32>(compiler.get_declared_struct_size(type));

        // Extract members
        for (u32 i = 0; i < type.member_types.size(); ++i) {
            UniformMember member{};
            member.name = compiler.get_member_name(r.base_type_id, i);
            member.offset = compiler.type_struct_member_offset(type, i);
            member.size = static_cast<u32>(
                compiler.get_declared_struct_member_size(type, i));
            cb.members.push_back(std::move(member));
        }

        refl.constant_buffers.push_back(std::move(cb));
    }

    // Textures (sampled images)
    for (const auto& r : resources.sampled_images) {
        TextureReflection tex{};
        tex.name = compiler.get_name(r.id);
        tex.binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        tex.set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto& type = compiler.get_type(r.type_id);
        tex.dimensions = static_cast<u8>(type.image.dim + 1);
        tex.is_array = type.image.arrayed;
        tex.is_storage = false;
        refl.textures.push_back(std::move(tex));
    }

    // Storage images
    for (const auto& r : resources.storage_images) {
        TextureReflection tex{};
        tex.name = compiler.get_name(r.id);
        tex.binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        tex.set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        auto& type = compiler.get_type(r.type_id);
        tex.dimensions = static_cast<u8>(type.image.dim + 1);
        tex.is_array = type.image.arrayed;
        tex.is_storage = true;
        refl.textures.push_back(std::move(tex));
    }

    // Samplers
    for (const auto& r : resources.separate_samplers) {
        SamplerReflection samp{};
        samp.name = compiler.get_name(r.id);
        samp.binding = compiler.get_decoration(r.id, spv::DecorationBinding);
        samp.set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
        refl.samplers.push_back(std::move(samp));
    }

    // Push constants
    for (const auto& r : resources.push_constant_buffers) {
        auto& type = compiler.get_type(r.base_type_id);
        refl.push_constant_size = static_cast<u32>(
            compiler.get_declared_struct_size(type));
    }

    // I/O
    extract_io(compiler, resources.stage_inputs, refl.inputs, true);
    extract_io(compiler, resources.stage_outputs, refl.outputs, false);

    // Workgroup size
    auto entry_points = compiler.get_entry_points_and_stages();
    for (const auto& ep : entry_points) {
        if (ep.execution_model == spv::ExecutionModelGLCompute) {
            refl.workgroup_size[0] = compiler.get_execution_mode_argument(
                spv::ExecutionModeLocalSize, 0);
            refl.workgroup_size[1] = compiler.get_execution_mode_argument(
                spv::ExecutionModeLocalSize, 1);
            refl.workgroup_size[2] = compiler.get_execution_mode_argument(
                spv::ExecutionModeLocalSize, 2);
            break;
        }
    }

    return refl;
}

} // namespace vitreo::graphics
