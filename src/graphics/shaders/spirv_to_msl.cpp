// SPDX-License-Identifier: LGPL-2.1-only

/// @file spirv_to_msl.cpp
/// @brief SPIR-V → MSL compiler implementation using SPIRV-Cross.

#include "spirv_to_msl.hpp"
#include "compile_profile.hpp"
#include "translation_cache.hpp"
#include "minmax_reduction.hpp"

#include <algorithm>
#include <cctype>
#include <map>

// Suppress warnings from SPIRV-Cross headers
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include <spirv_msl.hpp>
#pragma clang diagnostic pop
#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/msl_mesh_emit.hpp"  // WS5
#include "graphics/shaders/dxbc_to_spirv.hpp"  // VERTEX_PULL_TABLE_BINDING
#include "graphics/shaders/mesh_stage.hpp"

#include <cstring>

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

    void materialize_metadata_snapshots() {
        // These internal SSA values hold immutable runtime metadata. Keep
        // each as one explicit read instead of forwarding the readonly
        // buffer expression into every bounds check. OpName is only an
        // optimization hint; stripping names cannot change guest behavior.
        for (const auto& [id, meta] : ir.meta) {
            (void)meta;
            if (get_name(id).rfind("vitreo_view_limit_snapshot", 0) == 0)
                forced_temporaries.insert(id);
        }
    }

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
    /// schedule across.
    ///
    /// D3D11.3 22.19.2 bars refactoring of a precise result only. A
    /// file-scope contract pragma also bars it for every other operation,
    /// and Safe mode still fuses those (measured: a*b+c becomes one FMA).
    /// The helpers below carry the pragma on their own expression, so an
    /// always_inline precise op stays unfused and the surrounding shader
    /// keeps its FMAs. `fp_contract_off` is the other contract, 22.3.2: no
    /// operation may fuse, so the pragma covers the shader and the precise
    /// ops are the plain operators (bit-identical to the old lowering).
    void lower_precise_to_plain_operators(bool contract_off) {
        for (const auto& [id, meta] : ir.meta) {
            (void)id;
            if (!meta.decoration.decoration_flags.get(
                    spv::DecorationNoContraction)) {
                continue;
            }
            if (contract_off) {
                add_header_line("#pragma clang fp contract(off)");
                m_precise_plain = true;
                return;
            }
            add_header_line(
                "template<typename T> [[clang::always_inline]] T "
                "vitreoPreciseAdd(T l, T r) { _Pragma(\"clang fp "
                "contract(off) reassociate(off)\") return l + r; }");
            add_header_line(
                "template<typename T> [[clang::always_inline]] T "
                "vitreoPreciseSub(T l, T r) { _Pragma(\"clang fp "
                "contract(off) reassociate(off)\") return l - r; }");
            add_header_line(
                "template<typename T> [[clang::always_inline]] T "
                "vitreoPreciseMul(T l, T r) { _Pragma(\"clang fp "
                "contract(off) reassociate(off)\") return l * r; }");
            add_header_line(
                "template<typename T, int N> [[clang::always_inline]] T "
                "vitreoPreciseDot(metal::vec<T, N> l, metal::vec<T, N> r) { "
                "_Pragma(\"clang fp contract(off) reassociate(off)\") "
                "return metal::dot(l, r); }");
            m_precise_isolated = true;
            return;
        }
    }
    bool m_precise_plain = false;
    bool m_precise_isolated = false;

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

    /// MslCompileOptions::raster_ordered_bindings: put every resource
    /// variable at (set, binding) in raster order group 0. SPIRV-Cross
    /// emits `raster_order_group(0)` for a member of `interlocked_resources`
    /// (an argument-buffer member through its InterfaceOrigID, an entry
    /// argument directly), which it otherwise fills only from a
    /// fragment-shader-interlock critical section. Returns how many
    /// variables it marked.
    uint32_t mark_raster_ordered(uint32_t set, uint32_t binding) {
        uint32_t marked = 0;
        ir.for_each_typed_id<spirv_cross::SPIRVariable>(
            [&](uint32_t id, spirv_cross::SPIRVariable& var) {
                if (var.storage != spv::StorageClassUniformConstant &&
                    var.storage != spv::StorageClassStorageBuffer &&
                    var.storage != spv::StorageClassUniform) {
                    return;
                }
                if (!has_decoration(id, spv::DecorationDescriptorSet) ||
                    !has_decoration(id, spv::DecorationBinding) ||
                    get_decoration(id, spv::DecorationDescriptorSet) != set ||
                    get_decoration(id, spv::DecorationBinding) != binding) {
                    return;
                }
                interlocked_resources.insert(id);
                ++marked;
            });
        return marked;
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
        if (emit_barycentric_member(type, member_type_id, index, qualifier)) return;
        CompilerMSL::emit_struct_member(type, member_type_id, index,
                                        qualifier, base_offset);
    }

    // ---- SM 6.1 barycentrics (dxbc_to_spirv_vitreo_bary.cpp) ----
    /// MSL qualifier of each BaryCoord builtin (the variable's Centroid /
    /// Sample decorations, removed before compile: SPIRV-Cross refuses
    /// them and knows only the center_ forms).
public:
    std::string m_bary_location{"center"};
    std::string m_bary_no_perspective_location{"center"};
    void prepare_barycentrics() {
        ir.for_each_typed_id<spirv_cross::SPIRVariable>(
            [&](uint32_t id, spirv_cross::SPIRVariable& var) {
                if (var.storage != spv::StorageClassInput ||
                    !has_decoration(id, spv::DecorationBuiltIn)) {
                    return;
                }
                const auto builtin = static_cast<spv::BuiltIn>(
                    get_decoration(id, spv::DecorationBuiltIn));
                if (builtin != spv::BuiltInBaryCoordKHR &&
                    builtin != spv::BuiltInBaryCoordNoPerspKHR) {
                    return;
                }
                std::string where = "center";
                if (has_decoration(id, spv::DecorationCentroid)) where = "centroid";
                if (has_decoration(id, spv::DecorationSample)) where = "sample";
                unset_decoration(id, spv::DecorationCentroid);
                unset_decoration(id, spv::DecorationSample);
                (builtin == spv::BuiltInBaryCoordKHR ? m_bary_location
                                                     : m_bary_no_perspective_location) = where;
            });
    }
protected:
    /// A stage-in BaryCoord member with a centroid / sample location: the
    /// qualifier SPIRV-Cross writes, center_ replaced.
    bool emit_barycentric_member(const spirv_cross::SPIRType& type, uint32_t member_type_id,
                                 uint32_t index, const std::string& qualifier) {
        if (get_entry_point().model != spv::ExecutionModelFragment) return false;
        spv::BuiltIn builtin = spv::BuiltInMax;
        const bool is_builtin = is_member_builtin(type, index, &builtin);
        const bool bary = is_builtin && (builtin == spv::BuiltInBaryCoordKHR ||
                                         builtin == spv::BuiltInBaryCoordNoPerspKHR);
        if (!bary) return false;
        const std::string& where = builtin == spv::BuiltInBaryCoordKHR
                                       ? m_bary_location
                                       : m_bary_no_perspective_location;
        if (where == "center") return false;
        builtin_declaration = true;
        std::string line = to_struct_member(type, member_type_id, index, qualifier);
        builtin_declaration = false;
        const auto at = line.find("center_");
        if (at == std::string::npos) return false;
        line.replace(at, 6, where);
        statement(line);
        return true;
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

    // ---- WS5 begin ----
    /// A mesh-lowered geometry shader (MslCompileOptions::mesh): the
    /// GLCompute entry becomes `[[mesh]] void main0(...)`, and the
    /// Vitreo.MeshGS instructions become the invocation index, the thread
    /// index and the mesh epilogue (msl_mesh_emit.hpp). SPIRV-Cross 1.3.283
    /// has no MSL mesh support of its own (M11).
public:
    void set_mesh(const GsMeshLayout* layout, uint32_t ext_set) {
        m_mesh = layout;
        m_mesh_set = ext_set;
    }
    /// A D3D12 mesh / amplification shader (mesh_stage.hpp).
    void set_mesh_stage(const MeshStageLayout* layout, uint32_t ext_set,
                        u32 payload_words) {
        m_stage = layout;
        m_stage_set = ext_set;
        m_stage_payload = payload_words;
    }

protected:

    void emit_function_prototype(spirv_cross::SPIRFunction& func,
                                 const spirv_cross::Bitset& flags) override {
        if ((m_mesh == nullptr && m_stage == nullptr) ||
            func.self != ir.default_entry_point || !func.arguments.empty()) {
            CompilerMSL::emit_function_prototype(func, flags);
            return;
        }
        // CompilerMSL::emit_function_prototype for an entry point without
        // arguments, with the mesh qualifier and parameters in front.
        local_variable_names = resource_names;
        processing_entry_point = true;
        const auto preamble = m_mesh != nullptr
                                  ? gs_mesh_preamble(*m_mesh)
                                  : mesh_stage_preamble(*m_stage, m_stage_payload);
        for (const auto& line : preamble) statement(line);
        std::string args = msl_options.argument_buffers
                               ? entry_point_args_argument_buffer(false)
                               : entry_point_args_classic(false);
        std::string params = m_mesh != nullptr
                                 ? gs_mesh_entry_parameters(*m_mesh)
                                 : mesh_stage_parameters(*m_stage, m_stage_payload);
        if (m_stage != nullptr && m_stage->vi_capture != kMeshViCaptureNone) {
            // View instancing captures index records by the group's linear
            // threadgroup index: SPIRV-Cross's own builtin arguments when it
            // declared them (Metal refuses a builtin attribute twice), else
            // ours.
            const auto builtin = [&](const char* attribute, const char* fallback) {
                const std::string tag = std::string("[[") + attribute + "]]";
                const usize at = args.find(tag);
                if (at == std::string::npos) {
                    params += std::string("uint3 ") + fallback + " " + tag + ", ";
                    return std::string(fallback);
                }
                usize end = at;
                while (end > 0 && args[end - 1] == ' ') --end;
                usize begin = end;
                while (begin > 0 && (std::isalnum(static_cast<unsigned char>(args[begin - 1])) ||
                                     args[begin - 1] == '_')) {
                    --begin;
                }
                return args.substr(begin, end - begin);
            };
            const std::string tg = builtin("threadgroup_position_in_grid", "vitreo_vi_tg");
            const std::string tgs = builtin("threadgroups_per_grid", "vitreo_vi_tgs");
            m_vi_linear = "(" + tg + ".x + " + tgs + ".x * (" + tg + ".y + " + tgs + ".y * " + tg + ".z))";
        }
        if (args.empty()) params.resize(params.size() - 2);  // ", "
        local_variable_names.insert(resource_names.begin(),
                                    resource_names.end());
        for (auto var_id : vars_needing_early_declaration) {
            auto& ed_var = get<spirv_cross::SPIRVariable>(var_id);
            spirv_cross::ID& initializer = ed_var.initializer;
            if (!initializer) initializer = ir.increase_bound_by(1);
            if (ir.ids[initializer].get_type() == spirv_cross::TypeNone ||
                ir.ids[initializer].get_type() == spirv_cross::TypeExpression) {
                set<spirv_cross::SPIRExpression>(ed_var.initializer, "{}",
                                                 ed_var.basetype, true);
            }
        }
        statement(m_mesh != nullptr ? std::string("[[mesh]] void ")
                                    : mesh_stage_qualifier(*m_stage),
                  to_name(func.self), "(", params, args, ")");
    }

    bool emit_mesh_stage_instruction(const spirv_cross::Instruction& instruction) {
        if (m_stage == nullptr ||
            static_cast<spv::Op>(instruction.op) != spv::OpExtInst) {
            return false;
        }
        const uint32_t* ops = stream(instruction);
        if (instruction.length < 4 || ops[2] != m_stage_set) return false;
        switch (ops[3]) {
        case kMeshStagePayloadCopy:
            if (instruction.length < 6) return false;
            for (const auto& line : mesh_stage_payload_copy(
                     *m_stage, m_stage_payload, to_name(ops[4]),
                     to_enclosed_expression(ops[5]))) {
                statement(line);
            }
            return true;
        case kMeshStageEpilogue:
            if (instruction.length < 10) return false;
            for (const auto& line : mesh_stage_epilogue(
                     *m_stage, to_name(ops[4]), to_name(ops[5]), to_name(ops[6]),
                     to_name(ops[7]), to_enclosed_expression(ops[8]),
                     to_enclosed_expression(ops[9]), m_vi_linear)) {
                statement(line);
            }
            return true;
        case kObjectStageEpilogue:
            if (instruction.length < 7) return false;
            for (const auto& line : object_stage_epilogue(
                     *m_stage, m_stage_payload, to_name(ops[4]), to_name(ops[5]),
                     to_enclosed_expression(ops[6]), m_vi_linear)) {
                statement(line);
            }
            return true;
        default:
            return false;
        }
    }
    const MeshStageLayout* m_stage = nullptr;
    /// The group's linear threadgroup index (view-instancing captures).
    std::string m_vi_linear;
    uint32_t m_stage_set = 0;
    u32 m_stage_payload = 0;

    bool emit_mesh_instruction(const spirv_cross::Instruction& instruction) {
        if (m_mesh == nullptr ||
            static_cast<spv::Op>(instruction.op) != spv::OpExtInst) {
            return false;
        }
        const uint32_t* ops = stream(instruction);
        if (instruction.length < 4 || ops[2] != m_mesh_set) return false;
        switch (ops[3]) {
        case kMeshGsInvocationIndex:
            emit_op(ops[0], ops[1], gs_mesh_invocation_expression(*m_mesh),
                    true);
            return true;
        case kMeshGsLocalIndex:
            emit_op(ops[0], ops[1], gs_mesh_local_expression(), true);
            return true;
        case kMeshGsEpilogue:
            if (instruction.length < 7) return false;
            for (const auto& line :
                 gs_mesh_epilogue(*m_mesh, to_name(ops[4]), to_name(ops[5]),
                                  to_expression(ops[6]))) {
                statement(line);
            }
            return true;
        default:
            return false;
        }
    }
    const GsMeshLayout* m_mesh = nullptr;
    uint32_t m_mesh_set = 0;
    // ---- WS5 end ----

    void emit_instruction(const spirv_cross::Instruction& instruction) override {
        const auto op = static_cast<spv::Op>(instruction.op);
        if (emit_mesh_instruction(instruction)) return;  // WS5
        if (emit_mesh_stage_instruction(instruction)) return;
        if ((m_precise_plain || m_precise_isolated) &&
            !msl_options.invariant_float_math &&
            (op == spv::OpFAdd || op == spv::OpFSub || op == spv::OpFMul ||
             (m_precise_isolated && op == spv::OpDot))) {
            const uint32_t* ops = stream(instruction);
            if (has_decoration(ops[1], spv::DecorationNoContraction)) {
                if (m_precise_isolated) {
                    const char* fn = op == spv::OpFAdd   ? "vitreoPreciseAdd"
                                     : op == spv::OpFSub ? "vitreoPreciseSub"
                                     : op == spv::OpFMul ? "vitreoPreciseMul"
                                                         : "vitreoPreciseDot";
                    emit_binary_func_op(ops[0], ops[1], ops[2], ops[3], fn);
                } else if (op != spv::OpDot) {
                    emit_binary_op(ops[0], ops[1], ops[2], ops[3],
                                   op == spv::OpFAdd   ? "+"
                                   : op == spv::OpFSub ? "-"
                                                       : "*");
                }
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

/// A refused translation: no MSL, and @p why as CompiledShader::failure.
CompiledShader refuse_msl(CompiledShader& result, const char* why) {
    result.msl_source.clear();
    result.failure = why;
    return result;
}

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

static CompiledShader compile_spirv_to_msl_uncached(
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
        return refuse_msl(result, "empty SPIR-V module");
    }

    // Metal has no geometry-shader stage. SPIRV-Cross currently prints an
    // `unknown` entry-point qualifier plus unresolved EmitVertex helpers for
    // this execution model, which is not compilable MSL. Keep the backend
    // boundary honest until a separate, explicit GS emulation path exists.
    if (stage == ShaderStage::Geometry) {
        return refuse_msl(result, "Metal has no geometry stage (a geometry shader reached SPIR-V->MSL)");
    }

    // SPIRV-Cross cannot express demotion before MSL 2.3 and throws on
    // this instruction. Refuse explicitly in our exception-free build.
    if (options.msl_version < 20300u) {
        for (usize i = 5; i < spirv_word_count;) {
            const u32 words = spirv_data[i] >> 16;
            if (words == 0 || words > spirv_word_count - i) return refuse_msl(result, "malformed SPIR-V instruction length");
            if ((spirv_data[i] & 0xFFFFu) ==
                static_cast<u32>(spv::OpDemoteToHelperInvocationEXT)) {
                VITREO_TRACE("[msl] pixel discard requires MSL 2.3\n");
                return refuse_msl(result, "OpDemoteToHelperInvocation (discard) needs MSL 2.3");
            }
            i += words;
        }
    }

    // Create SPIRV-Cross MSL compiler
    std::vector<u32> spirv_vec(spirv_data, spirv_data + spirv_word_count);
    // ---- WS5 begin ----
    // A mesh-lowered geometry shader: a GLCompute module importing
    // Vitreo.MeshGS. Anything else asked for as a mesh is refused, never
    // compiled as a kernel mislabelled as a mesh.
    uint32_t mesh_set = 0;
    if (options.mesh != nullptr) {
        if (stage != ShaderStage::Compute) return refuse_msl(result, "a mesh-lowered geometry shader must be a compute module");
        for (usize i = 5; i < spirv_word_count;) {
            const u32 words = spirv_data[i] >> 16;
            if (words == 0 || words > spirv_word_count - i) return refuse_msl(result, "malformed SPIR-V instruction length");
            if ((spirv_data[i] & 0xFFFFu) ==
                    static_cast<u32>(spv::OpExtInstImport) &&
                words >= 3) {
                const char* name =
                    reinterpret_cast<const char*>(spirv_data + i + 2);
                const usize bytes = (words - 2) * 4;
                if (std::string(name, strnlen(name, bytes)) ==
                    kMeshGsExtInstSet) {
                    mesh_set = spirv_data[i + 1];
                }
            }
            i += words;
        }
        if (mesh_set == 0) return refuse_msl(result, "mesh lowering asked for, but the module does not import Vitreo.MeshGS");
    }
    // ---- WS5 end ----
    uint32_t stage_set = 0;
    if (options.mesh_stage != nullptr) {
        if (stage != ShaderStage::Mesh && stage != ShaderStage::Amplification) {
            return refuse_msl(result, "a mesh-stage layout on a stage that is not mesh / amplification");
        }
        for (usize i = 5; i < spirv_word_count;) {
            const u32 words = spirv_data[i] >> 16;
            if (words == 0 || words > spirv_word_count - i) return refuse_msl(result, "malformed SPIR-V instruction length");
            if ((spirv_data[i] & 0xFFFFu) ==
                    static_cast<u32>(spv::OpExtInstImport) &&
                words >= 3) {
                const char* name =
                    reinterpret_cast<const char*>(spirv_data + i + 2);
                const usize bytes = (words - 2) * 4;
                if (std::string(name, strnlen(name, bytes)) ==
                    kMeshStageExtInstSet) {
                    stage_set = spirv_data[i + 1];
                }
            }
            i += words;
        }
        if (stage_set == 0) return refuse_msl(result, "mesh-stage layout asked for, but the module does not import the mesh-stage set");
    }
    FencingCompilerMSL compiler(std::move(spirv_vec));
    compiler.materialize_metadata_snapshots();
    if (options.mesh != nullptr) compiler.set_mesh(options.mesh, mesh_set);  // WS5
    if (options.mesh_stage != nullptr) {
        compiler.set_mesh_stage(options.mesh_stage, stage_set,
                                options.mesh_payload_words);
    }
    compiler.declare_precise_multiply();
    if (options.precise_without_helpers) {
        compiler.lower_precise_to_plain_operators(options.fp_contract_off);
    }
    if (!options.raster_ordered_bindings.empty()) {
        // Raster order groups exist for fragment functions only (MSL 2.0
        // and later); a binding that names no resource is a caller error,
        // not something to compile around.
        if (stage != ShaderStage::Fragment || options.msl_version < 20000) {
            return refuse_msl(result, "raster-ordered bindings outside a fragment shader or below MSL 2.0");
        }
        for (const auto& [set, binding] : options.raster_ordered_bindings) {
            if (compiler.mark_raster_ordered(set, binding) == 0) return refuse_msl(result, "a raster-ordered binding names no resource");
        }
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
    msl_opts.enable_frag_output_mask = options.fragment_output_mask;
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
    compiler.prepare_barycentrics();


    // Set entry point
    auto entry_points = compiler.get_entry_points_and_stages();
    auto target_model = options.mesh_stage != nullptr
                            ? spv::ExecutionModelGLCompute
                            : stage_to_execution_model(stage);
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
                    return refuse_msl(result, "atomics on a storage-image array need MSL 3.1");
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
                return refuse_msl(result, "an unbounded descriptor array has no argument-buffer table to size it");
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
        // A descriptor-heap window: its set is one argument buffer whose
        // arrays are each one binding of their whole length.
        const bool window = options.heap_window.enabled &&
                            options.heap_window.descriptor_set >=
                                MSL_DESCRIPTOR_SET_COUNT &&
                            options.heap_window.descriptor_set <
                                spirv_cross::kMaxArgumentBuffers;
        if (options.heap_window.enabled && !window) return refuse_msl(result, "descriptor-heap window set outside the argument-buffer range");
        if (window) {
            const u32 set = options.heap_window.descriptor_set;
            spirv_cross::MSLResourceBinding argument_buffer{};
            argument_buffer.stage = target_model;
            argument_buffer.desc_set = set;
            argument_buffer.binding = spirv_cross::kArgumentBufferBinding;
            argument_buffer.msl_buffer = options.heap_window.metal_buffer_index;
            argument_buffer.basetype = spirv_cross::SPIRType::Float;
            argument_buffer.count = 1;
            compiler.add_msl_resource_binding(argument_buffer);
            // Padding steps its cursor past each member by the member's
            // count and then looks up the binding at the cursor, so a
            // second array aliasing id(0) asks for id(length). That id names
            // this empty binding (count 0): nothing is padded and the cursor
            // stays put for the next aliasing array.
            u32 window_length = 0;
            for (const auto& [key, length] : descriptor_arrays) {
                if (key.first == set) window_length = std::max(window_length, length);
            }
            if (window_length != 0u) {
                spirv_cross::MSLResourceBinding end{};
                end.stage = target_model;
                end.desc_set = set;
                end.binding = kDescriptorHeapWindowBaseBinding - 1u;
                end.msl_buffer = window_length;
                end.basetype = spirv_cross::SPIRType::Float;
                end.count = 0;
                compiler.add_msl_resource_binding(end);
            }
            for (const auto& [key, length] : descriptor_arrays) {
                if (key.first != set) continue;
                // A window array has its declared length; a runtime array
                // has none to give the argument buffer.
                if (length == 0u) return refuse_msl(result, "a runtime-sized array in the descriptor-heap window has no length");
                const u32 id = descriptor_array_variables.at(key);
                // An array type keeps its element's basetype.
                const auto& type = compiler.get_type_from_variable(id);
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = set;
                binding.binding = key.second;
                binding.count = length;
                // Every window array of the shader is the same heap: all of
                // them start at id(0), and SPIRV-Cross aliases arrays that
                // share an index (Metal 3 argument-buffer aliasing).
                binding.msl_buffer = 0;
                binding.msl_texture = 0;
                binding.msl_sampler = 0;
                switch (type.basetype) {
                case spirv_cross::SPIRType::Image:
                case spirv_cross::SPIRType::SampledImage:
                    binding.basetype = spirv_cross::SPIRType::Image;
                    break;
                case spirv_cross::SPIRType::Sampler:
                    binding.basetype = spirv_cross::SPIRType::Sampler;
                    break;
                default:
                    binding.basetype = spirv_cross::SPIRType::Float;
                    break;
                }
                compiler.add_msl_resource_binding(binding);
            }
        }
        for (u32 set = MSL_DESCRIPTOR_SET_COUNT;
             set < spirv_cross::kMaxArgumentBuffers; ++set) {
            if (window && set == options.heap_window.descriptor_set) continue;
            compiler.add_discrete_descriptor_set(set);
        }
        // The vertex-pull table (set 4, VERTEX_PULL_TABLE_BINDING) of a
        // vertex shader whose b#/t#/u# are argument buffers: the first index
        // of the vertex-stream window, as in the flat model below. Left
        // unregistered, SPIRV-Cross numbered it buffer(0), the push block.
        if (target_model == spv::ExecutionModelVertex) {
            for (const auto& r : compiler.get_shader_resources().storage_buffers) {
                if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) !=
                        4u ||
                    compiler.get_decoration(r.id, spv::DecorationBinding) !=
                        VERTEX_PULL_TABLE_BINDING) {
                    continue;
                }
                spirv_cross::MSLResourceBinding binding{};
                binding.stage = target_model;
                binding.desc_set = 4u;
                binding.binding = VERTEX_PULL_TABLE_BINDING;
                binding.msl_buffer = BACKEND_VERTEX_BUFFER_BASE;
                // Argument-buffer padding indexes bindings by base type: a
                // buffer of uints.
                binding.basetype = spirv_cross::SPIRType::UInt;
                binding.count = 1;
                compiler.add_msl_resource_binding(binding);
            }
        }
    }

    if (options.dual_source_blending) {
        if (stage != ShaderStage::Fragment) return refuse_msl(result, "dual-source blending outside a fragment shader");
        bool has_second_source = false;
        for (const auto& output : compiler.get_shader_resources().stage_outputs) {
            if (!compiler.has_decoration(output.id, spv::DecorationLocation)) continue;
            const u32 location = compiler.get_decoration(output.id, spv::DecorationLocation);
            // D3D11 dual-source blending operates on RT0 only. Keep the
            // normal MRT translation untouched unless this variant is requested.
            if (location > 1) return refuse_msl(result, "dual-source blending with an output past location 1");
            compiler.set_decoration(output.id, spv::DecorationLocation, 0);
            compiler.set_decoration(output.id, spv::DecorationIndex, location);
            has_second_source |= location == 1;
        }
        if (!has_second_source) return refuse_msl(result, "dual-source blending without a second source output");
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
                const bool vertex_set4 =
                    set == 4u && target_model == spv::ExecutionModelVertex;
                // Binding 1 of a vertex shader is the vertex-pull table
                // (dxbc_to_spirv_vertex_pull.cpp): the first index of the
                // vertex-stream window, which a pulling draw leaves empty.
                const bool vertex_pull_table =
                    vertex_set4 &&
                    binding.binding == VERTEX_PULL_TABLE_BINDING;
                const bool vertex_capture = vertex_set4 && !vertex_pull_table;
                if (vertex_pull_table) {
                    binding.msl_buffer = BACKEND_VERTEX_BUFFER_BASE;
                    compiler.add_msl_resource_binding(binding);
                    continue;
                }
                if (vertex_capture && binding.binding != 0u) {
                    VITREO_TRACE("[msl] vertex stage declares expansion "
                                 "binding %u; only the capture buffer "
                                 "(binding 0) has an index\n",
                                 binding.binding);
                    return refuse_msl(result, "the vertex stage declares an expansion binding other than the capture buffer");
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
                return refuse_msl(result, "a UAV counter binding has no Metal buffer index in this stage");
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
                return refuse_msl(result, "the pipeline-statistics record has no Metal buffer index in this stage");
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
    if (options.mesh_stage != nullptr && options.mesh_stage->state.is_amplification()) {
        (void)object_stage_buffer_addresses(msl);
    }
    // A view-instancing replay's object payload carries one word past the
    // D3D payload (MeshStageLayout::vi_payload_base): its object_data array
    // is not the declared one's type, so that translation keeps the
    // threadgroup copy of the D3D payload.
    if (options.mesh_stage != nullptr && !options.mesh_stage->state.is_amplification() &&
        options.mesh_payload_words != 0 && !options.mesh_stage->vi_payload_base &&
        !mesh_stage_direct_payload(msl, options.mesh_payload_words)) {
        // The budget check (mesh_stage_threadgroup_bytes) assumed the
        // payload is read in place; an empty result refuses the shader.
        return refuse_msl(result, "the mesh shader's payload is not read in place (threadgroup budget assumed it)");
    }
    if (options.mesh_stage != nullptr && options.mesh_stage->device_staging &&
        !(options.mesh_stage->state.is_amplification()
              ? object_stage_device_staging(msl)
              : mesh_stage_device_staging(msl, *options.mesh_stage, options.mesh_payload_words))) {
        return refuse_msl(result, "mesh device staging assumed, but the MSL does not stage through device memory");  // device staging assumed; refuse rather than overrun
    }
    // SPIRV-Cross marks its multiply helpers optnone, so each one is a real
    // call the Metal compiler will not inline or schedule across. 320 of the
    // captured Satisfactory shaders call them. always_inline keeps the fma
    // body (the -0 contract is vitreoFMul, which stays optnone) and lets the
    // surrounding shader fuse non-precise operations.
    {
        const std::string optnone = "[[clang::optnone]]";
        const std::string inl = "[[clang::always_inline]]";
        std::string rewritten;
        rewritten.reserve(msl.size());
        for (usize at = 0; at < msl.size();) {
            const usize pos = msl.find(optnone, at);
            if (pos == std::string::npos) {
                rewritten.append(msl, at, std::string::npos);
                break;
            }
            rewritten.append(msl, at, pos - at);
            const usize eol = msl.find('\n', pos);
            const usize end = eol == std::string::npos ? msl.size() : eol;
            const bool cross_helper =
                msl.find("spvF", pos) != std::string::npos &&
                msl.find("spvF", pos) < end;
            rewritten += cross_helper ? inl : optnone;
            at = pos + optnone.size();
        }
        msl.swap(rewritten);
    }
    // The descriptor-table MIN/MAX reductions (dxbc_to_spirv_minmax_sample.cpp
    // emit_minmax_sample_call) stay out of line: inlined at every sample
    // site they multiplied a shader's Metal compile time (measured, the
    // captured Satisfactory pixel shaders), and they run only when a
    // sampler-state record asks for the reduction.
    {
        const std::string inl = "static inline __attribute__((always_inline))\n";
        const std::string kept = "static __attribute__((noinline))\n";
        for (usize at = msl.find(inl); at != std::string::npos;
             at = msl.find(inl, at + 1)) {
            const usize line = at + inl.size();
            const usize eol = msl.find('\n', line);
            const usize name = msl.find(" vitreoMinMaxSample", line);
            if (name != std::string::npos && name < eol) {
                msl.replace(at, inl.size(), kept);
            }
        }
    }
    fence_minmax_reduction(msl);
    // An acceleration structure in an argument buffer: this SPIRV-Cross
    // (vulkan-sdk-1.3.283) keeps only images and samplers by value there and
    // writes every other UniformConstant resource as a pointer member, which
    // Metal rejects for [[id(n)]] ("not valid for attribute 'id'"). A
    // descriptor is the structure itself (an MTLResourceID slot): declare
    // the member by value and drop the dereference at its uses.
    {
        const std::string pointer = "constant raytracing::acceleration_structure<raytracing::instancing>* ";
        for (usize pos = msl.find(pointer); pos != std::string::npos; pos = msl.find(pointer, pos)) {
            const usize name_at = pos + pointer.size();
            const usize name_end = msl.find_first_of(" [;", name_at);
            if (name_end == std::string::npos) break;
            const std::string name = msl.substr(name_at, name_end - name_at);
            msl.replace(pos, pointer.size(), "raytracing::acceleration_structure<raytracing::instancing> ");
            // (*spvDescriptorSetN.name) -> spvDescriptorSetN.name
            const std::string set_prefix = "(*spvDescriptorSet";
            for (usize use = msl.find(set_prefix); use != std::string::npos;
                 use = msl.find(set_prefix, use + 1)) {
                const usize dot = msl.find('.', use);
                const usize close = dot == std::string::npos ? std::string::npos : msl.find(')', dot);
                if (close == std::string::npos) break;
                if (msl.compare(dot + 1, close - dot - 1, name) != 0) continue;
                msl.erase(close, 1);
                msl.erase(use, 2);
            }
        }
    }

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
    if (stage == ShaderStage::Compute || options.mesh_stage != nullptr) {
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

namespace {

/// The key of a SPIR-V -> MSL translation (TranslationCache): the module,
/// the stage, every MslCompileOptions field, and the one host property the
/// translation reads (msl_memory_fence_available). Adding a field to
/// MslCompileOptions breaks the size check below until it is keyed here.
std::array<u8, 32> msl_cache_key(const u32* spirv, usize words, ShaderStage stage,
                                 const MslCompileOptions& o) {
    static_assert(sizeof(MslCompileOptions) == 144,
                  "a new MslCompileOptions field must be part of msl_cache_key");
    TranslationKey key(TranslationKind::SpirvToMsl);
    key.bytes(spirv, words * sizeof(u32));
    key.u32_value(static_cast<u32>(stage));
    key.u32_value(o.msl_version);
    key.u32_value(o.platform);
    key.u32_value((o.argument_buffers ? 1u : 0u) | (o.force_native_arrays ? 2u : 0u) |
                  (o.pad_fragment_output ? 4u : 0u) | (o.dual_source_blending ? 8u : 0u) |
                  (o.framebuffer_fetch ? 16u : 0u) | (o.precise_without_helpers ? 32u : 0u) |
                  (o.fp_contract_off ? 64u : 0u) | (o.heap_window.enabled ? 128u : 0u) |
                  (msl_memory_fence_available() ? 256u : 0u));
    key.u32_value(o.fragment_output_mask);
    key.u32_value(o.buffer_size_buffer_index);
    key.u32_value(o.push_constant_buffer_index);
    key.u32_value(static_cast<u32>(o.raster_ordered_bindings.size()));
    for (const auto& [set, binding] : o.raster_ordered_bindings) {
        key.u32_value(set);
        key.u32_value(binding);
    }
    for (const MslArgumentBufferSet& set : o.argument_buffer_sets) {
        key.u32_value(set.slot_count);
        key.u32_value(set.metal_buffer_index);
        key.u32_value(static_cast<u32>(set.slot_class));
    }
    key.u32_value(o.heap_window.descriptor_set);
    key.u32_value(o.heap_window.metal_buffer_index);
    key.u32_value(o.mesh_payload_words);
    return key.finish();
}

/// A translated shader as a cache value. The SPIR-V it was made from is the
/// lookup's own input and is not stored.
std::vector<u8> encode_compiled(const CompiledShader& c) {
    static_assert(sizeof(CompiledShader) == 168,
                  "a new CompiledShader field must be part of encode_compiled");
    PayloadWriter w;
    w.bytes(c.msl_source.data(), c.msl_source.size());
    w.u32_value(static_cast<u32>(c.bindings.size()));
    for (const ResourceBinding& b : c.bindings) {
        w.u32_value(static_cast<u32>(b.type));
        w.u32_value(b.set);
        w.u32_value(b.binding);
        w.u32_value(b.count);
        w.u32_value(b.mtl_index);
    }
    for (const auto* list : {&c.inputs, &c.outputs}) {
        w.u32_value(static_cast<u32>(list->size()));
        for (const ShaderIO& io : *list) {
            w.u32_value(static_cast<u32>(io.semantic) | (u32{io.semantic_index} << 8) |
                        (u32{io.component_count} << 16));
            w.u32_value(io.location);
        }
    }
    w.u32_value(c.workgroup_size_x);
    w.u32_value(c.workgroup_size_y);
    w.u32_value(c.workgroup_size_z);
    w.u32_value(c.uses_push_constants ? 1u : 0u);
    return std::move(w.out);
}

bool decode_compiled(const std::vector<u8>& payload, ShaderStage stage, const u32* spirv,
                     usize words, CompiledShader& c) {
    PayloadReader r(payload);
    c = CompiledShader{};
    c.stage = stage;
    u32 n = 0;
    if (!r.bytes(c.msl_source) || c.msl_source.empty() || !r.u32_value(n) || n > 65536) {
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        ResourceBinding b{};
        u32 type = 0;
        if (!r.u32_value(type) || !r.u32_value(b.set) || !r.u32_value(b.binding) ||
            !r.u32_value(b.count) || !r.u32_value(b.mtl_index)) {
            return false;
        }
        b.type = static_cast<BindingType>(type);
        c.bindings.push_back(b);
    }
    for (auto* list : {&c.inputs, &c.outputs}) {
        if (!r.u32_value(n) || n > 65536) return false;
        for (u32 i = 0; i < n; ++i) {
            u32 packed = 0;
            ShaderIO io{};
            if (!r.u32_value(packed) || !r.u32_value(io.location)) return false;
            io.semantic = static_cast<SemanticName>(packed & 0xFFu);
            io.semantic_index = static_cast<u8>(packed >> 8);
            io.component_count = static_cast<u8>(packed >> 16);
            list->push_back(io);
        }
    }
    u32 push = 0;
    if (!r.u32_value(c.workgroup_size_x) || !r.u32_value(c.workgroup_size_y) ||
        !r.u32_value(c.workgroup_size_z) || !r.u32_value(push) || !r.done()) {
        return false;
    }
    c.uses_push_constants = push != 0;
    c.spirv_binary.resize(words * sizeof(u32));
    std::memcpy(c.spirv_binary.data(), spirv, c.spirv_binary.size());
    return true;
}

}  // namespace

CompiledShader compile_spirv_to_msl(
    const u32* spirv_data,
    usize spirv_word_count,
    ShaderStage stage,
    const MslCompileOptions& options)
{
    const compile_profile::Scope timed(compile_profile::SpirvToMsl);
    // The mesh layouts are structures the key would have to walk; those
    // rare translations are not cached.
    TranslationCache& cache = TranslationCache::instance();
    const bool cached = cache.enabled() && spirv_data != nullptr && spirv_word_count != 0 &&
                        options.mesh == nullptr && options.mesh_stage == nullptr;
    std::array<u8, 32> key{};
    if (cached) {
        key = msl_cache_key(spirv_data, spirv_word_count, stage, options);
        std::vector<u8> payload;
        CompiledShader hit;
        if (cache.lookup(key, payload) &&
            decode_compiled(payload, stage, spirv_data, spirv_word_count, hit)) {
            compile_profile::note(compile_profile::MslHit);
            return hit;
        }
        compile_profile::note(compile_profile::MslMiss);
    }
    CompiledShader result =
        compile_spirv_to_msl_uncached(spirv_data, spirv_word_count, stage, options);
    // Only a translation that succeeded: a refusal is cheap and carries its
    // reason.
    if (cached && result.valid() && result.failure.empty()) {
        cache.store(key, encode_compiled(result));
    }
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
