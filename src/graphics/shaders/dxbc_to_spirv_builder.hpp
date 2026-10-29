// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv_builder.hpp
/// @brief SpirvBuilder, the SPIR-V module writer the DXBC translator emits through.

#include "graphics/shaders/dxbc_to_spirv_spv_defs.hpp"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vitreo::graphics::dxbc_spirv_detail {

// =============================================================================
// SPIR-V Builder
// =============================================================================

class SpirvBuilder {
public:
    enum class Section : u8 {
        Capabilities,
        Extensions,
        ExtInstImports,
        MemoryModel,
        EntryPoints,
        ExecutionModes,
        Debug,
        Annotations,
        TypesGlobals,
        Functions,
    };

    SpirvBuilder() : m_next_id(SPIRV_BOUND_BASE) {}

    [[nodiscard]] u32 allocate_id() { return m_next_id++; }

    void set_section(Section section) { m_section = section; }

    void emit(SpvOp op, const std::vector<u32>& operands = {}) {
        u32 word_count = 1 + static_cast<u32>(operands.size());
        u32 instruction = (word_count << 16) | static_cast<u32>(op);
        auto& dest = section_words(m_section);
        dest.push_back(instruction);
        dest.insert(dest.end(), operands.begin(), operands.end());
    }

    /// OpCapability, once per capability.
    void emit_capability(SpvCapability cap) {
        if (!m_capability_set.insert(static_cast<u32>(cap)).second) return;
        set_section(Section::Capabilities);
        emit(SpvOp::OpCapability, {static_cast<u32>(cap)});
    }

    /// OpExtension, once per name.
    void emit_extension(const std::string& name) {
        if (!m_extension_names.insert(name).second) return;
        set_section(Section::Extensions);
        std::vector<u32> words;
        append_string(words, name);
        emit(SpvOp::OpExtension, words);
    }

    [[nodiscard]] u32 emit_ext_inst_import(const std::string& name) {
        set_section(Section::ExtInstImports);
        u32 id = allocate_id();
        std::vector<u32> words = {id};
        append_string(words, name);
        emit(SpvOp::OpExtInstImport, words);
        return id;
    }

    void emit_memory_model() {
        set_section(Section::MemoryModel);
        // Addressing model: Logical, Memory model: GLSL450
        emit(SpvOp::OpMemoryModel, {0, 1});
    }

    void emit_entry_point(SpvExecutionModel model, u32 func_id,
                         const std::string& name,
                         const std::vector<u32>& interface_ids) {
        set_section(Section::EntryPoints);
        std::vector<u32> words = {
            static_cast<u32>(model),
            func_id
        };
        append_string(words, name);
        words.insert(words.end(), interface_ids.begin(), interface_ids.end());
        emit(SpvOp::OpEntryPoint, words);
    }

    void emit_execution_mode(u32 func_id, SpvExecutionMode mode) {
        set_section(Section::ExecutionModes);
        emit(SpvOp::OpExecutionMode, {func_id, static_cast<u32>(mode)});
    }

    void emit_execution_mode(u32 func_id, SpvExecutionMode mode, u32 literal) {
        set_section(Section::ExecutionModes);
        emit(SpvOp::OpExecutionMode,
             {func_id, static_cast<u32>(mode), literal});
    }

    void emit_execution_mode(u32 func_id, SpvExecutionMode mode,
                             u32 x, u32 y, u32 z) {
        set_section(Section::ExecutionModes);
        emit(SpvOp::OpExecutionMode,
             {func_id, static_cast<u32>(mode), x, y, z});
    }

    void emit_name(u32 target_id, const std::string& name) {
        set_section(Section::Debug);
        std::vector<u32> words = {target_id};
        append_string(words, name);
        emit(SpvOp::OpName, words);
    }

    void emit_decorate(u32 target_id, SpvDecoration decoration, u32 value) {
        set_section(Section::Annotations);
        emit(SpvOp::OpDecorate, {target_id, static_cast<u32>(decoration), value});
    }

    void emit_decorate(u32 target_id, SpvDecoration decoration) {
        set_section(Section::Annotations);
        emit(SpvOp::OpDecorate, {target_id, static_cast<u32>(decoration)});
    }

    void emit_decorate_string(u32 target_id, SpvDecoration decoration,
                              const std::string& value) {
        set_section(Section::Annotations);
        std::vector<u32> words = {target_id, static_cast<u32>(decoration)};
        append_string(words, value);
        emit(SpvOp::OpDecorateString, words);
    }

    void emit_member_decorate(u32 struct_id, u32 member, SpvDecoration decoration, u32 value) {
        set_section(Section::Annotations);
        emit(SpvOp::OpMemberDecorate,
             {struct_id, member, static_cast<u32>(decoration), value});
    }

    /// Member decoration that takes no literal operand (NonWritable, ...).
    void emit_member_decorate(u32 struct_id, u32 member,
                              SpvDecoration decoration) {
        set_section(Section::Annotations);
        emit(SpvOp::OpMemberDecorate,
             {struct_id, member, static_cast<u32>(decoration)});
    }

    [[nodiscard]] u32 emit_type_void() {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeVoid, {id});
        return id;
    }

    [[nodiscard]] u32 emit_type_bool() {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeBool, {id});
        return id;
    }

    [[nodiscard]] u32 emit_type_float(u32 width) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeFloat, {id, width});
        return id;
    }

    [[nodiscard]] u32 emit_type_int(u32 width, bool is_signed) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeInt, {id, width, is_signed ? 1u : 0u});
        return id;
    }

    [[nodiscard]] u32 emit_type_vector(u32 component_type, u32 count) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeVector, {id, component_type, count});
        return id;
    }

    [[nodiscard]] u32 emit_type_array(u32 element_type, u32 length_id) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeArray, {id, element_type, length_id});
        return id;
    }

    [[nodiscard]] u32 emit_type_runtime_array(u32 element_type) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeRuntimeArray, {id, element_type});
        return id;
    }

    [[nodiscard]] u32 emit_type_struct(const std::vector<u32>& member_types) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        std::vector<u32> words = {id};
        words.insert(words.end(), member_types.begin(), member_types.end());
        emit(SpvOp::OpTypeStruct, words);
        return id;
    }

    [[nodiscard]] u32 emit_type_pointer(SpvStorageClass storage, u32 type) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypePointer, {id, static_cast<u32>(storage), type});
        m_pointee_types[id] = type;
        return id;
    }
    /// The type a pointer type made by emit_type_pointer points to; 0 for
    /// any other id.
    [[nodiscard]] u32 pointee_type(u32 pointer_type) const {
        const auto found = m_pointee_types.find(pointer_type);
        return found == m_pointee_types.end() ? 0u : found->second;
    }

    [[nodiscard]] u32 emit_type_function(u32 return_type,
                                        const std::vector<u32>& param_types = {}) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        std::vector<u32> words = {id, return_type};
        words.insert(words.end(), param_types.begin(), param_types.end());
        emit(SpvOp::OpTypeFunction, words);
        return id;
    }

    [[nodiscard]] u32 emit_type_sampled_image(u32 image_type) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeSampledImage, {id, image_type});
        return id;
    }

    [[nodiscard]] u32 emit_type_sampler() {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        emit(SpvOp::OpTypeSampler, {id});
        return id;
    }

    [[nodiscard]] u32 emit_type_image(u32 sampled_type, SpvDim dim,
                                      bool arrayed = false,
                                      bool multisampled = false,
                                      u32 sampled = 1u,
                                      u32 format = 0u,
                                      u32 depth = 2u) {
        // SPIR-V 1.6 3.31 (Capability): a Dim=Buffer image needs
        // SampledBuffer when sampled (Buffer<T>) and ImageBuffer when it is
        // a storage image (RWBuffer<T>).
        if (dim == SpvDim::DimBuffer) {
            emit_capability(sampled == 2u ? SpvCapability::ImageBuffer
                                          : SpvCapability::SampledBuffer);
        }
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        // Depth=2 leaves depth-versus-color use unspecified. D3D permits the
        // same resource declaration shape to feed ordinary and comparison
        // sampling opcodes; the sampling instruction supplies that semantic.
        // A Dim=Buffer image passes 0: no comparison opcode can name one.
        emit(SpvOp::OpTypeImage,
              {id, sampled_type, static_cast<u32>(dim), depth,
              arrayed ? 1u : 0u, multisampled ? 1u : 0u, sampled, format});
        return id;
    }

    [[nodiscard]] u32 emit_constant_float(u32 type_id, float value) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        u32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        emit(SpvOp::OpConstant, {type_id, id, bits});
        return id;
    }

    [[nodiscard]] u32 emit_constant_int(u32 type_id, u32 value) {
        set_section(Section::TypesGlobals);
        // Cache small uint constants so AccessChain indices stay unique.
        if (type_id == m_cached_uint_type) {
            auto it = m_uint_constants.find(value);
            if (it != m_uint_constants.end()) return it->second;
        }
        u32 id = allocate_id();
        emit(SpvOp::OpConstant, {type_id, id, value});
        if (type_id == m_cached_uint_type) {
            m_uint_constants[value] = id;
        }
        return id;
    }

    void set_cached_uint_type(u32 type_id) { m_cached_uint_type = type_id; }

    [[nodiscard]] u32 emit_variable(u32 type_id, SpvStorageClass storage) {
        // Function-local variables belong inside the function; globals in types.
        if (storage == SpvStorageClass::Function) {
            set_section(Section::Functions);
        } else {
            set_section(Section::TypesGlobals);
        }
        u32 id = allocate_id();
        emit(SpvOp::OpVariable, {type_id, id, static_cast<u32>(storage)});
        if (storage == SpvStorageClass::Private) m_private_variables.push_back(id);
        return id;
    }

    /// Every Private (module-scope, per-invocation) variable declared so far:
    /// temps shared by subroutine and class-linkage bodies, loop guards, the
    /// UAV leader flag. SPIR-V 1.4+ lists them all in the entry point's
    /// interface; none becomes an MSL entry-point argument.
    [[nodiscard]] const std::vector<u32>& private_variables() const {
        return m_private_variables;
    }

    [[nodiscard]] u32 emit_load(u32 result_type, u32 pointer) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpLoad, {result_type, id, pointer});
        return id;
    }

    void emit_store(u32 pointer, u32 object) {
        set_section(Section::Functions);
        emit(SpvOp::OpStore, {pointer, object});
    }

    [[nodiscard]] u32 emit_access_chain(u32 result_type, u32 base,
                                       const std::vector<u32>& indices) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, base};
        words.insert(words.end(), indices.begin(), indices.end());
        emit(SpvOp::OpAccessChain, words);
        return id;
    }

    /// Module-level constant composite (OpConstantComposite). Required for
    /// valid SPIR-V; OpCompositeConstruct is a function-body opcode only.
    [[nodiscard]] u32 emit_constant_composite(u32 result_type,
                                              const std::vector<u32>& constituents) {
        set_section(Section::TypesGlobals);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id};
        words.insert(words.end(), constituents.begin(), constituents.end());
        emit(SpvOp::OpConstantComposite, words);
        return id;
    }

    [[nodiscard]] u32 emit_composite_construct(u32 result_type,
                                               const std::vector<u32>& constituents) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id};
        words.insert(words.end(), constituents.begin(), constituents.end());
        emit(SpvOp::OpCompositeConstruct, words);
        return id;
    }

    [[nodiscard]] u32 emit_composite_extract(u32 result_type, u32 composite,
                                             const std::vector<u32>& indices) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, composite};
        words.insert(words.end(), indices.begin(), indices.end());
        emit(SpvOp::OpCompositeExtract, words);
        return id;
    }

    /// OpCompositeInsert: @p composite with @p object at @p indices.
    [[nodiscard]] u32 emit_composite_insert(u32 result_type, u32 object,
                                            u32 composite,
                                            const std::vector<u32>& indices) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, object, composite};
        words.insert(words.end(), indices.begin(), indices.end());
        emit(SpvOp::OpCompositeInsert, words);
        return id;
    }

    /// While set, every float arithmetic result is decorated NoContraction.
    /// DXBC `precise` (OpcodeToken0 bits 22:19, D3D11 functional spec: the
    /// instruction is IEEE-exact and may not be fused, reassociated or
    /// otherwise refactored). SPIRV-Cross lowers the decoration to its
    /// optnone spvFMul/spvFAdd helpers, which is what keeps two shaders
    /// that share a position expression bit-identical -- the invariance a
    /// depth prepass + GREATER_EQUAL base pass depends on.
    void set_no_contraction(bool on) { m_no_contraction = on; }
    bool m_no_contraction{false};
    void note_float_result(u32 id) {
        if (m_no_contraction) emit_decorate(id, SpvDecoration::NoContraction);
    }

    [[nodiscard]] u32 emit_fadd(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFAdd, {result_type, id, op1, op2});
        note_float_result(id);
        return id;
    }

    [[nodiscard]] u32 emit_snegate(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpSNegate, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_fnegate(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFNegate, {result_type, id, value});
        note_float_result(id);
        return id;
    }

    /// OpGroupNonUniformBallot: the lanes of @p scope whose @p predicate is
    /// true, as a uvec4 bitmask.
    [[nodiscard]] u32 emit_group_ballot(u32 result_type, u32 scope,
                                        u32 predicate) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpGroupNonUniformBallot, {result_type, id, scope, predicate});
        return id;
    }

    /// OpGroupNonUniformQuadBroadcast: @p value from quad lane @p index.
    [[nodiscard]] u32 emit_quad_broadcast(u32 result_type, u32 scope,
                                          u32 value, u32 index) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpGroupNonUniformQuadBroadcast,
             {result_type, id, scope, value, index});
        return id;
    }

    [[nodiscard]] u32 emit_derivative(SpvOp opcode, u32 result_type,
                                      u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(opcode, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_image_query_size(u32 result_type, u32 image,
                                            u32 lod) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageQuerySizeLod, {result_type, id, image, lod});
        return id;
    }

    [[nodiscard]] u32 emit_image_query_levels(u32 result_type, u32 image) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageQueryLevels, {result_type, id, image});
        return id;
    }

    [[nodiscard]] u32 emit_image_query_size(u32 result_type, u32 image) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageQuerySize, {result_type, id, image});
        return id;
    }

    /// OpImageQueryLod: (clamped mip level, unclamped LOD) as a vec2.
    [[nodiscard]] u32 emit_image_query_lod(u32 result_type,
                                           u32 sampled_image,
                                           u32 coordinate) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageQueryLod,
             {result_type, id, sampled_image, coordinate});
        return id;
    }

    [[nodiscard]] u32 emit_image_query_samples(u32 result_type, u32 image) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageQuerySamples, {result_type, id, image});
        return id;
    }

    [[nodiscard]] u32 emit_all(u32 bool_type, u32 vector) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpAll, {bool_type, id, vector});
        return id;
    }

    [[nodiscard]] u32 emit_iadd(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpIAdd, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_isub(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpISub, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_imul(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpIMul, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_udiv(u32 result_type, u32 dividend, u32 divisor) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpUDiv, {result_type, id, dividend, divisor});
        return id;
    }

    [[nodiscard]] u32 emit_umod(u32 result_type, u32 dividend, u32 divisor) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpUMod, {result_type, id, dividend, divisor});
        return id;
    }

    [[nodiscard]] u32 emit_smul_extended(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpSMulExtended, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_umul_extended(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpUMulExtended, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_fsub(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFSub, {result_type, id, op1, op2});
        note_float_result(id);
        return id;
    }

    [[nodiscard]] u32 emit_fmul(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFMul, {result_type, id, op1, op2});
        note_float_result(id);
        return id;
    }

    [[nodiscard]] u32 emit_funord_ne(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFUnordNotEqual, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_ford_eq(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFOrdEqual, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_is_nan(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpIsNan, {result_type, id, value});
        return id;
    }

    /// OpLogicalOr / OpLogicalAnd on booleans (or boolean vectors).
    [[nodiscard]] u32 emit_logical(SpvOp opcode, u32 result_type, u32 op1,
                                   u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(opcode, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_logical_not(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpLogicalNot, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_ford_ge(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFOrdGreaterThanEqual,
             {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_ford_lt(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFOrdLessThan, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_bitwise_and(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpBitwiseAnd, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_shift_right_logical(u32 result_type, u32 value,
                                                u32 count) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpShiftRightLogical,
             {result_type, id, value, count});
        return id;
    }

    [[nodiscard]] u32 emit_shift_right_arithmetic(u32 result_type, u32 value,
                                                   u32 count) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpShiftRightArithmetic,
             {result_type, id, value, count});
        return id;
    }

    [[nodiscard]] u32 emit_shift_left_logical(u32 result_type, u32 value,
                                               u32 count) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpShiftLeftLogical,
             {result_type, id, value, count});
        return id;
    }

    [[nodiscard]] u32 emit_bitwise_or(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpBitwiseOr, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_bitwise_xor(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpBitwiseXor, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_not(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpNot, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_bit_field_extract(SpvOp opcode, u32 result_type,
                                             u32 base, u32 offset,
                                             u32 count) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(opcode, {result_type, id, base, offset, count});
        return id;
    }

    [[nodiscard]] u32 emit_unary_integer(SpvOp opcode, u32 result_type,
                                         u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(opcode, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_ext_inst(u32 result_type, u32 set, u32 instruction,
                                    const std::vector<u32>& operands) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, set, instruction};
        words.insert(words.end(), operands.begin(), operands.end());
        emit(SpvOp::OpExtInst, words);
        return id;
    }

    [[nodiscard]] u32 emit_select(u32 result_type, u32 cond, u32 a, u32 b) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpSelect, {result_type, id, cond, a, b});
        return id;
    }

    [[nodiscard]] u32 emit_bitcast(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpBitcast, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_convert_f_to_u(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpConvertFToU, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_convert_f_to_s(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpConvertFToS, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_convert_s_to_f(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpConvertSToF, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_convert_u_to_f(u32 result_type, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpConvertUToF, {result_type, id, value});
        return id;
    }

    [[nodiscard]] u32 emit_inotequal(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpINotEqual, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_integer_compare(SpvOp opcode, u32 result_type,
                                            u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(opcode, {result_type, id, op1, op2});
        return id;
    }

    [[nodiscard]] u32 emit_vector_shuffle(u32 result_type, u32 v1, u32 v2,
                                         const std::vector<u32>& components) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, v1, v2};
        words.insert(words.end(), components.begin(), components.end());
        emit(SpvOp::OpVectorShuffle, words);
        return id;
    }

    [[nodiscard]] u32 emit_fdiv(u32 result_type, u32 op1, u32 op2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFDiv, {result_type, id, op1, op2});
        note_float_result(id);
        return id;
    }

    [[nodiscard]] u32 emit_dot(u32 result_type, u32 vec1, u32 vec2) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpDot, {result_type, id, vec1, vec2});
        note_float_result(id);
        return id;
    }

    [[nodiscard]] u32 emit_sampled_image(u32 result_type, u32 image,
                                         u32 sampler) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpSampledImage,
             {result_type, id, image, sampler});
        return id;
    }

    /// SPIR-V atomic read-modify-write on a pointer. Learn (Interlocked*):
    /// D3D's atomics are device-scoped and unordered with respect to other
    /// memory, which is Device scope with Relaxed semantics.
    [[nodiscard]] u32 emit_atomic_rmw(SpvOp op, u32 result_type, u32 pointer,
                                      u32 scope, u32 semantics, u32 value) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(op, {result_type, id, pointer, scope, semantics, value});
        return id;
    }

    /// SPIR-V OpImageTexelPointer: takes the image VARIABLE (a pointer to
    /// OpTypeImage, not a loaded image) and a texel coordinate, and yields an
    /// Image-class pointer to that texel which the atomic instructions can
    /// operate on. Sample must be 0 for a non-multisampled image.
    [[nodiscard]] u32 emit_image_texel_pointer(u32 result_type, u32 image_var,
                                               u32 coordinate, u32 sample) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageTexelPointer,
             {result_type, id, image_var, coordinate, sample});
        return id;
    }

    /// SPIR-V atomic compare-and-swap. Learn
    /// (InterlockedCompareExchange / InterlockedCompareStore): the stored
    /// value replaces the old one only when the old one equals the
    /// comparand, and the value read before the operation is returned. Note
    /// the SPIR-V operand order is Value then Comparator, which is the
    /// reverse of the DXBC source order.
    [[nodiscard]] u32 emit_atomic_compare_exchange(u32 result_type,
                                                   u32 pointer, u32 scope,
                                                   u32 equal_semantics,
                                                   u32 unequal_semantics,
                                                   u32 value,
                                                   u32 comparator) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpAtomicCompareExchange,
             {result_type, id, pointer, scope, equal_semantics,
              unequal_semantics, value, comparator});
        return id;
    }

    [[nodiscard]] u32 emit_image_sample(u32 result_type, u32 sampled_image,
                                        u32 coordinate, u32 bias = 0,
                                        u32 const_offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        // SPIR-V image operands are listed in increasing bit order:
        // Bias (0x1) then ConstOffset (0x8).
        std::vector<u32> words{result_type, id, sampled_image, coordinate};
        const u32 mask = (bias != 0 ? 0x1u : 0u) |
                         (const_offset != 0 ? 0x8u : 0u);
        if (mask != 0) {
            words.push_back(mask);
            if (bias != 0) words.push_back(bias);
            if (const_offset != 0) words.push_back(const_offset);
        }
        emit(SpvOp::OpImageSampleImplicitLod, words);
        return id;
    }

    [[nodiscard]] u32 emit_image_sample_explicit_lod(
        u32 result_type, u32 sampled_image, u32 coordinate, u32 lod,
        u32 const_offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        // Lod (0x2) then ConstOffset (0x8), in increasing bit order.
        if (const_offset == 0) {
            emit(SpvOp::OpImageSampleExplicitLod,
                 {result_type, id, sampled_image, coordinate, 0x2u, lod});
        } else {
            emit(SpvOp::OpImageSampleExplicitLod,
                 {result_type, id, sampled_image, coordinate, 0xAu, lod,
                  const_offset});
        }
        return id;
    }

    /// @p const_offset (ConstOffset, 0x8) or @p offset (Offset, 0x10, a
    /// runtime value; needs ImageGatherExtended) -- at most one.
    [[nodiscard]] u32 emit_image_gather(u32 result_type, u32 sampled_image,
                                        u32 coordinate, u32 component,
                                        u32 const_offset = 0,
                                        u32 offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        if (offset != 0) {
            emit(SpvOp::OpImageGather,
                 {result_type, id, sampled_image, coordinate, component,
                  0x10u, offset});
        } else if (const_offset == 0) {
            emit(SpvOp::OpImageGather,
                 {result_type, id, sampled_image, coordinate, component});
        } else {
            emit(SpvOp::OpImageGather,
                 {result_type, id, sampled_image, coordinate, component,
                  0x8u, const_offset});
        }
        return id;
    }

    /// The comparison gather: four depth comparisons against @p reference.
    [[nodiscard]] u32 emit_image_dref_gather(u32 result_type,
                                             u32 sampled_image,
                                             u32 coordinate, u32 reference,
                                             u32 const_offset = 0,
                                             u32 offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        if (offset != 0) {
            emit(SpvOp::OpImageDrefGather,
                 {result_type, id, sampled_image, coordinate, reference,
                  0x10u, offset});
        } else if (const_offset == 0) {
            emit(SpvOp::OpImageDrefGather,
                 {result_type, id, sampled_image, coordinate, reference});
        } else {
            emit(SpvOp::OpImageDrefGather,
                 {result_type, id, sampled_image, coordinate, reference,
                  0x8u, const_offset});
        }
        return id;
    }

    [[nodiscard]] u32 emit_image_sample_grad(
        u32 result_type, u32 sampled_image, u32 coordinate,
        u32 dx, u32 dy, u32 const_offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        // Grad (0x4) then ConstOffset (0x8), in increasing bit order.
        if (const_offset == 0) {
            emit(SpvOp::OpImageSampleExplicitLod,
                 {result_type, id, sampled_image, coordinate, 0x4u, dx, dy});
        } else {
            emit(SpvOp::OpImageSampleExplicitLod,
                 {result_type, id, sampled_image, coordinate, 0xCu, dx, dy,
                  const_offset});
        }
        return id;
    }

    [[nodiscard]] u32 emit_image_fetch(
        u32 result_type, u32 image, u32 coordinate, u32 lod,
        u32 const_offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        // SPIR-V Image Operands Lod bit followed by the integer mip ID.
        if (const_offset != 0) {
            // Image Operands Lod|ConstOffset; operands follow bit order.
            emit(SpvOp::OpImageFetch,
                 {result_type, id, image, coordinate, 0xAu, lod,
                  const_offset});
        } else {
            emit(SpvOp::OpImageFetch,
                 {result_type, id, image, coordinate, 0x2u, lod});
        }
        return id;
    }

    /// Fetch with no image operands. A Dim=Buffer image has no mip chain and
    /// no sample index, so SPIR-V forbids the Lod operand the texture path
    /// always supplies.
    [[nodiscard]] u32 emit_image_fetch_plain(
        u32 result_type, u32 image, u32 coordinate) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageFetch, {result_type, id, image, coordinate});
        return id;
    }

    [[nodiscard]] u32 emit_image_fetch_sample(
        u32 result_type, u32 image, u32 coordinate, u32 sample,
        u32 const_offset = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        if (const_offset != 0) {
            // ConstOffset (0x8) precedes Sample (0x40) by operand bit order.
            emit(SpvOp::OpImageFetch,
                 {result_type, id, image, coordinate, 0x48u,
                  const_offset, sample});
        } else {
            emit(SpvOp::OpImageFetch,
                 {result_type, id, image, coordinate, 0x40u, sample});
        }
        return id;
    }

    void emit_image_write(u32 image, u32 coordinate, u32 texel) {
        set_section(Section::Functions);
        emit(SpvOp::OpImageWrite, {image, coordinate, texel});
    }

    void emit_control_barrier(u32 execution_scope, u32 memory_scope,
                              u32 memory_semantics) {
        set_section(Section::Functions);
        emit(SpvOp::OpControlBarrier,
             {execution_scope, memory_scope, memory_semantics});
    }

    void emit_memory_barrier(u32 memory_scope, u32 memory_semantics) {
        set_section(Section::Functions);
        emit(SpvOp::OpMemoryBarrier, {memory_scope, memory_semantics});
    }

    [[nodiscard]] u32 emit_image_read(u32 result_type, u32 image,
                                      u32 coordinate) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageRead, {result_type, id, image, coordinate});
        return id;
    }

    [[nodiscard]] u32 emit_phi(
        u32 result_type,
        const std::vector<std::pair<u32, u32>>& incoming) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        std::vector<u32> words = {result_type, id};
        for (const auto& [value, parent] : incoming) {
            words.push_back(value);
            words.push_back(parent);
        }
        emit(SpvOp::OpPhi, words);
        return id;
    }

    [[nodiscard]] u32 emit_image_sample_dref(
        u32 result_type, u32 sampled_image, u32 coordinate, u32 reference,
        u32 bias = 0) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        if (bias == 0) {
            emit(SpvOp::OpImageSampleDrefImplicitLod,
                 {result_type, id, sampled_image, coordinate, reference});
        } else {
            emit(SpvOp::OpImageSampleDrefImplicitLod,
                 {result_type, id, sampled_image, coordinate, reference,
                  0x1u, bias});
        }
        return id;
    }

    [[nodiscard]] u32 emit_image_sample_dref_lz(
        u32 result_type, u32 sampled_image, u32 coordinate, u32 reference,
        u32 lod) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpImageSampleDrefExplicitLod,
             {result_type, id, sampled_image, coordinate, reference, 0x2u,
              lod});
        return id;
    }

    [[nodiscard]] u32 emit_function(u32 result_type, u32 function_type) {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpFunction, {result_type, id, 0, function_type});
        return id;
    }

    /// OpFunctionCall with no arguments (a hull-shader phase function).
    u32 emit_function_call(u32 result_type, u32 function) {
        set_section(Section::Functions);
        const u32 id = allocate_id();
        emit(SpvOp::OpFunctionCall, {result_type, id, function});
        return id;
    }

    [[nodiscard]] u32 emit_function_call(u32 result_type, u32 function,
                                         const std::vector<u32>& args) {
        set_section(Section::Functions);
        const u32 id = allocate_id();
        std::vector<u32> words = {result_type, id, function};
        words.insert(words.end(), args.begin(), args.end());
        emit(SpvOp::OpFunctionCall, words);
        return id;
    }

    void emit_function_id(u32 id, u32 result_type, u32 function_type) {
        set_section(Section::Functions);
        emit(SpvOp::OpFunction, {result_type, id, 0, function_type});
    }

    [[nodiscard]] u32 emit_label() {
        set_section(Section::Functions);
        u32 id = allocate_id();
        emit(SpvOp::OpLabel, {id});
        return id;
    }

    void emit_label_id(u32 id) {
        set_section(Section::Functions);
        emit(SpvOp::OpLabel, {id});
    }

    void emit_selection_merge(u32 merge_block) {
        set_section(Section::Functions);
        emit(SpvOp::OpSelectionMerge, {merge_block, 0u});
    }

    void emit_loop_merge(u32 merge_block, u32 continue_block) {
        set_section(Section::Functions);
        emit(SpvOp::OpLoopMerge, {merge_block, continue_block, 0u});
    }

    void emit_branch(u32 target) {
        set_section(Section::Functions);
        emit(SpvOp::OpBranch, {target});
    }

    void emit_branch_conditional(u32 condition, u32 true_block,
                                 u32 false_block) {
        set_section(Section::Functions);
        emit(SpvOp::OpBranchConditional,
             {condition, true_block, false_block});
    }

    void emit_switch(u32 selector, u32 default_block,
                     const std::vector<std::pair<u32, u32>>& targets) {
        set_section(Section::Functions);
        std::vector<u32> words = {selector, default_block};
        for (const auto& [literal, target] : targets) {
            words.push_back(literal);
            words.push_back(target);
        }
        emit(SpvOp::OpSwitch, words);
    }

    void emit_kill() {
        set_section(Section::Functions);
        emit(SpvOp::OpKill);
    }

    void emit_vertex() {
        set_section(Section::Functions);
        emit(SpvOp::OpEmitVertex);
    }

    void emit_end_primitive() {
        set_section(Section::Functions);
        emit(SpvOp::OpEndPrimitive);
    }

    void emit_return() {
        set_section(Section::Functions);
        emit(SpvOp::OpReturn);
    }

    void emit_function_end() {
        set_section(Section::Functions);
        emit(SpvOp::OpFunctionEnd);
    }

    [[nodiscard]] u32 emit_function_parameter(u32 type) {
        set_section(Section::Functions);
        const u32 id = allocate_id();
        emit(SpvOp::OpFunctionParameter, {type, id});
        return id;
    }

    void emit_return_value(u32 value) {
        set_section(Section::Functions);
        emit(SpvOp::OpReturnValue, {value});
    }

    /// Index of the next word of the Functions section: a loop header's
    /// OpPhi is emitted before its back-edge value exists and completed
    /// through patch_function_word.
    [[nodiscard]] usize function_word_count() const {
        return m_functions.size();
    }
    void patch_function_word(usize index, u32 value) {
        m_functions[index] = value;
    }

    [[nodiscard]] std::vector<u32> finalize(u32 bound) {
        std::vector<u32> module;
        module.push_back(SPIRV_MAGIC);
        module.push_back(SPIRV_VERSION);
        module.push_back(SPIRV_GENERATOR);
        module.push_back(bound);
        module.push_back(0);
        const Section order[] = {
            Section::Capabilities,
            Section::Extensions,
            Section::ExtInstImports,
            Section::MemoryModel,
            Section::EntryPoints,
            Section::ExecutionModes,
            Section::Debug,
            Section::Annotations,
            Section::TypesGlobals,
            Section::Functions,
        };
        for (Section s : order) {
            const auto& words = section_words(s);
            module.insert(module.end(), words.begin(), words.end());
        }
        return module;
    }

private:
    [[nodiscard]] std::vector<u32>& section_words(Section section) {
        switch (section) {
        case Section::Capabilities: return m_capabilities;
        case Section::Extensions: return m_extensions;
        case Section::ExtInstImports: return m_ext_inst_imports;
        case Section::MemoryModel: return m_memory_model;
        case Section::EntryPoints: return m_entry_points;
        case Section::ExecutionModes: return m_execution_modes;
        case Section::Debug: return m_debug;
        case Section::Annotations: return m_annotations;
        case Section::TypesGlobals: return m_types_globals;
        case Section::Functions: return m_functions;
        }
        return m_functions;
    }

    void append_string(std::vector<u32>& words, const std::string& str) {
        const char* cstr = str.c_str();
        usize len = str.length();
        usize aligned = (len + 4) / 4;
        for (usize i = 0; i < aligned; ++i) {
            u32 word = 0;
            for (usize j = 0; j < 4 && (i * 4 + j) <= len; ++j) {
                word |= static_cast<u32>(cstr[i * 4 + j]) << (j * 8);
            }
            words.push_back(word);
        }
    }

    u32 m_next_id;
    Section m_section{Section::Capabilities};
    u32 m_cached_uint_type{0};
    std::map<u32, u32> m_uint_constants;
    std::vector<u32> m_capabilities;
    std::vector<u32> m_extensions;
    std::set<std::string> m_extension_names;
    std::set<u32> m_capability_set;
    std::vector<u32> m_private_variables;
    std::vector<u32> m_ext_inst_imports;
    std::vector<u32> m_memory_model;
    std::vector<u32> m_entry_points;
    std::vector<u32> m_execution_modes;
    std::vector<u32> m_debug;
    std::vector<u32> m_annotations;
    std::vector<u32> m_types_globals;
    std::map<u32, u32> m_pointee_types;
    std::vector<u32> m_functions;
};

} // namespace vitreo::graphics::dxbc_spirv_detail
