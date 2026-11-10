// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_vitreo_ext.cpp
/// @brief The Vitreo extension instructions (dxbc_vitreo_ext.hpp): shader
/// model 6 wave and quad operations, 64-bit integers and halves.
///
/// Wave semantics (DirectX Shader Compiler wiki, "Wave Intrinsics", and
/// DXIL.rst opcodes 110-123, 135-136, 165-167): a wave operation sees the
/// active lanes that are not helper lanes; in a pixel shader every
/// operation but the lane queries and the quad operations runs inside
/// `if (!HelperInvocation)`, so helper lanes neither contribute nor read
/// (their results are undefined, as the spec leaves them). Quad operations
/// read every lane of the 2x2 quad, helpers included. Prefix operations are
/// exclusive. A ballot's bit k is lane k; bits of inactive or helper lanes
/// and bits at or above the lane count are zero.
///
/// SPIR-V's GroupNonUniform operations are the Metal simd_* / quad_*
/// functions (SPIRV-Cross). Metal has no 64-bit SIMD-group operation and no
/// multi-prefix one, so 64-bit reductions and scans, WaveMatch and
/// WaveMultiPrefix* run a loop over the wave's lanes that reads each lane
/// with a 32-bit shuffle and combines the active ones in lane order.
///
/// Measured on Apple M4 Pro (macOS 26, Metal 3.2): inside a short branch
/// the Metal compiler executes simd_ballot, simd_broadcast_first and
/// simd_is_first as though every lane were active (a lane outside the
/// branch is seen in the ballot and as the first lane), while simd_sum,
/// simd_min, simd_or, simd_prefix_exclusive_sum, simd_all and simd_shuffle
/// see only the active lanes. So no wave operation here uses
/// GroupNonUniformBallot, Elect, BroadcastFirst, BallotBitCount or
/// SPIRV-Cross's AllEqual (a broadcast-first compare): the ballot is the
/// bitwise-or reduction of each lane's bit, the first lane the minimum lane
/// index, the bit counts sums of 0 / 1.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"

#include <cstring>

namespace vitreo::graphics::dxbc_spirv_detail {

using namespace dxbc_vitreo_ext;

namespace {

constexpr u32 kScopeSubgroup = 3;
// GroupOperation (SPIR-V 1.6 3.28).
constexpr u32 kGroupReduce = 0;
constexpr u32 kGroupExclusiveScan = 2;

u32 immediate_bits(const Sm4Operand& operand) {
    u32 bits = 0;
    std::memcpy(&bits, &operand.immediate[0], sizeof(bits));
    return bits;
}

}  // namespace

// ---- types and builtins ----------------------------------------------------

u32 DxbcTranslator::vx_type_u64() {
    if (m_vx_u64 == 0) {
        m_builder.emit_capability(SpvCapability::Int64);
        m_vx_u64 = m_builder.emit_type_int(64, false);
    }
    return m_vx_u64;
}

u32 DxbcTranslator::vx_type_i64() {
    if (m_vx_i64 == 0) {
        m_builder.emit_capability(SpvCapability::Int64);
        m_vx_i64 = m_builder.emit_type_int(64, true);
    }
    return m_vx_i64;
}

u32 DxbcTranslator::vx_type_half() {
    if (m_vx_half == 0) {
        m_builder.emit_capability(SpvCapability::Float16);
        m_vx_half = m_builder.emit_type_float(16);
    }
    return m_vx_half;
}

u32 DxbcTranslator::vx_type_u16() {
    if (m_vx_u16 == 0) {
        m_builder.emit_capability(SpvCapability::Int16);
        m_vx_u16 = m_builder.emit_type_int(16, false);
    }
    return m_vx_u16;
}

u32 DxbcTranslator::vx_builtin(SpvBuiltIn builtin, u32 pointer_type, u32 type,
                               u32& variable, const char* name) {
    if (variable == 0) {
        variable = m_builder.emit_variable(pointer_type, SpvStorageClass::Input);
        m_builder.emit_name(variable, name);
        m_builder.emit_decorate(variable, SpvDecoration::BuiltIn, static_cast<u32>(builtin));
        if (builtin == SpvBuiltIn::SubgroupSize || builtin == SpvBuiltIn::SubgroupLocalInvocationId) {
            // Fragment inputs of integer type are Flat (Vulkan); the
            // subgroup builtins are integer inputs in every stage.
            if (m_result.stage == ShaderStage::Fragment) {
                m_builder.emit_decorate(variable, SpvDecoration::Flat);
            }
        }
        m_vx_interface.push_back(variable);
    }
    return m_builder.emit_load(type, variable);
}

u32 DxbcTranslator::vx_u64_of(u32 uvec4, u32 first) {
    const u32 pair = m_builder.emit_vector_shuffle(m_type_uvec2, uvec4, uvec4, {first, first + 1});
    return m_builder.emit_bitcast(vx_type_u64(), pair);
}

u32 DxbcTranslator::vx_uvec4_of_u64(u32 value) {
    const u32 pair = m_builder.emit_bitcast(m_type_uvec2, value);
    return m_builder.emit_vector_shuffle(m_type_uvec4, pair, pair, {0, 1, 0, 1});
}

namespace {

/// A GroupNonUniform arithmetic operation (scope Subgroup).
u32 group_op(SpirvBuilder& b, SpvOp o, u32 type, u32 scope, u32 operation, u32 value) {
    b.set_section(SpirvBuilder::Section::Functions);
    const u32 id = b.allocate_id();
    b.emit(o, {type, id, scope, operation, value});
    return id;
}

}  // namespace

u32 DxbcTranslator::vx_lane() {
    return vx_builtin(SpvBuiltIn::SubgroupLocalInvocationId, m_type_ptr_input_uint, m_type_uint, m_vx_subgroup_lane,
                      "vitreo_wave_lane");
}

u32 DxbcTranslator::vx_ballot(u32 condition) {
    // Word w of the ballot: the or of 1 << (lane & 31) over the active
    // lanes in [32w, 32w + 32) whose condition holds.
    m_builder.emit_capability(SpvCapability::GroupNonUniformArithmetic);
    const auto uint = [&](u32 value) { return m_builder.emit_constant_int(m_type_uint, value); };
    const u32 scope = uint(kScopeSubgroup);
    const u32 lane = vx_lane();
    const u32 bit = m_builder.emit_shift_left_logical(m_type_uint, uint(1),
                                                      m_builder.emit_bitwise_and(m_type_uint, lane, uint(31)));
    const u32 word_of_lane = m_builder.emit_shift_right_logical(m_type_uint, lane, uint(5));
    std::vector<u32> words;
    for (u32 w = 0; w < 4; ++w) {
        const u32 here = m_builder.emit_logical(
            SpvOp::OpLogicalAnd, m_type_bool, condition,
            m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, word_of_lane, uint(w)));
        words.push_back(group_op(m_builder, SpvOp::OpGroupNonUniformBitwiseOr, m_type_uint, scope, kGroupReduce,
                                 m_builder.emit_select(m_type_uint, here, bit, uint(0))));
    }
    return m_builder.emit_composite_construct(m_type_uvec4, words);
}

u32 DxbcTranslator::vx_first_lane() {
    m_builder.emit_capability(SpvCapability::GroupNonUniformArithmetic);
    return group_op(m_builder, SpvOp::OpGroupNonUniformUMin, m_type_uint,
                    m_builder.emit_constant_int(m_type_uint, kScopeSubgroup), kGroupReduce, vx_lane());
}

// ---- dispatch ---------------------------------------------------------------

bool DxbcTranslator::translate_vitreo_extension(u32 opcode) {
    if (opcode == kOpDclAccelerationStructure) {
        return fail("vitreo acceleration-structure declaration in the program body");
    }
    Sm4Operand control;
    if (!m_decoder.decode_operand(control) || !control.is_immediate) {
        return fail("vitreo extension " + std::to_string(opcode) + ": no control operand");
    }
    const u32 bits = immediate_bits(control);
    switch (opcode) {
    case kOpWave: return translate_vx_wave(bits);
    case kOpI64: return translate_vx_i64(bits);
    case kOpF16: return translate_vx_f16(bits);
    case kOpPackBytes:
    case kOpUnpackBytes: {
        const bool packing = opcode == kOpPackBytes;
        const bool narrow = (bits & 0x10u) != 0u;
        const u32 mode = bits & ~0x10u;
        if (mode > (packing ? 2u : 1u)) return fail("VX_BYTES: bad control");
        Sm4Operand dst;
        if (!m_decoder.decode_operand(dst)) return fail("VX_BYTES: bad destination");
        std::vector<u32> lanes;
        for (u32 i = 0; i < (packing ? 4u : 1u); ++i) {
            Sm4Operand source;
            if (!m_decoder.decode_operand(source) || source.modifier != 0u)
                return fail("VX_BYTES: bad source");
            u32 loaded = 0;
            if (!load_vec4(source, loaded)) return false;
            lanes.push_back(m_builder.emit_composite_extract(
                m_type_uint, m_builder.emit_bitcast(m_type_uvec4, loaded), {0u}));
        }
        m_builder.emit_capability(SpvCapability::Int8);
        const auto convert = [&](SpvOp op, u32 type, u32 value) {
            const u32 id = m_builder.allocate_id();
            m_builder.set_section(SpirvBuilder::Section::Functions);
            m_builder.emit(op, {type, id, value});
            return id;
        };
        const auto splat = [&](u32 type, u32 scalar_type, u32 value) {
            const u32 v = m_builder.emit_constant_int(scalar_type, value);
            return m_builder.emit_composite_construct(type, {v, v, v, v});
        };
        if (!packing) {
            const bool sign = mode == 1u;
            const u32 bytes = m_builder.emit_type_vector(m_builder.emit_type_int(8, sign), 4);
            u32 result = convert(sign ? SpvOp::OpSConvert : SpvOp::OpUConvert,
                                 sign ? m_type_ivec4 : m_type_uvec4,
                                 m_builder.emit_bitcast(bytes, lanes[0]));
            if (sign) result = m_builder.emit_bitcast(m_type_uvec4, result);
            // Internal i16 values occupy a zero-extended 32-bit lane.
            if (narrow) result = m_builder.emit_bitwise_and(
                m_type_uvec4, result, splat(m_type_uvec4, m_type_uint, 0xFFFFu));
            return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result));
        }
        u32 result = m_builder.emit_composite_construct(m_type_uvec4, lanes);
        if (mode != 0u) {
            if (narrow) {
                const u32 shift = splat(m_type_uvec4, m_type_uint, 16u);
                result = m_builder.emit_shift_left_logical(m_type_uvec4, result, shift);
                result = m_builder.emit_bitcast(m_type_ivec4, result);
                result = m_builder.emit_shift_right_arithmetic(m_type_ivec4, result, shift);
            } else {
                result = m_builder.emit_bitcast(m_type_ivec4, result);
            }
            // Both clamp forms take SIGNED inputs, including clamp_u8.
            const u32 lo = splat(m_type_ivec4, m_type_int,
                                 mode == 1u ? 0u : static_cast<u32>(-128));
            const u32 hi = splat(m_type_ivec4, m_type_int, mode == 1u ? 255u : 127u);
            result = m_builder.emit_select(m_type_ivec4,
                m_builder.emit_integer_compare(SpvOp::OpSLessThan, m_type_bvec4, result, lo), lo, result);
            result = m_builder.emit_select(m_type_ivec4,
                m_builder.emit_integer_compare(SpvOp::OpSGreaterThan, m_type_bvec4, result, hi), hi, result);
            result = m_builder.emit_bitcast(m_type_uvec4, result);
        }
        const u32 bytes = m_builder.emit_type_vector(m_builder.emit_type_int(8, false), 4);
        const u32 packed = m_builder.emit_bitcast(
            m_type_uint, convert(SpvOp::OpUConvert, bytes, result));
        const u32 out = m_builder.emit_composite_construct(m_type_uvec4, {packed, packed, packed, packed});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, out));
    }
    case kOpPackedDot: {
        Sm4Operand dst;
        std::array<Sm4Operand, 3> sources{};
        if (bits > 1u || !m_decoder.decode_operand(dst)) {
            return fail("VX_PACKED_DOT: bad control or destination");
        }
        std::array<u32, 3> values{};
        for (u32 i = 0; i < sources.size(); ++i) {
            if (!m_decoder.decode_operand(sources[i]) || sources[i].modifier != 0u) {
                return fail("VX_PACKED_DOT: bad source");
            }
            u32 loaded = 0;
            if (!load_vec4(sources[i], loaded)) return false;
            values[i] = m_builder.emit_composite_extract(
                m_type_uint, m_builder.emit_bitcast(m_type_uvec4, loaded), {0u});
        }
        const bool signed_bytes = bits == 0u;
        m_builder.emit_capability(SpvCapability::Int8);
        const u32 bytes = m_builder.emit_type_vector(
            m_builder.emit_type_int(8u, signed_bytes), 4u);
        if (signed_bytes) {
            // An exact float dot remains cheaper in large signed kernels:
            // widening thousands of short products can increase register
            // pressure. All signed byte products and partial sums fit
            // exactly in float32 (total in [-65024,65536]); keep only the
            // small dot in float, never the full-width accumulator.
            const u32 a = m_builder.emit_convert_s_to_f(
                m_type_vec4, m_builder.emit_bitcast(bytes, values[1]));
            const u32 b = m_builder.emit_convert_s_to_f(
                m_type_vec4, m_builder.emit_bitcast(bytes, values[2]));
            const u32 dot = m_builder.emit_dot(m_type_float, a, b);
            const u32 small = m_builder.emit_bitcast(
                m_type_uint, m_builder.emit_convert_f_to_s(m_type_int, dot));
            const u32 sum = m_builder.emit_iadd(m_type_uint, values[0], small);
            const u32 result = m_builder.emit_composite_construct(m_type_uvec4, {sum, sum, sum, sum});
            return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result));
        }
        m_builder.emit_capability(SpvCapability::Int16);
        const u32 shorts = m_builder.emit_type_vector(
            m_builder.emit_type_int(16u, false), 4u);
        const auto widen = [&](u32 type, u32 value) {
            const u32 id = m_builder.allocate_id();
            m_builder.set_section(SpirvBuilder::Section::Functions);
            m_builder.emit(SpvOp::OpUConvert, {type, id, value});
            return id;
        };
        const auto unpack = [&](u32 packed) {
            return widen(shorts, m_builder.emit_bitcast(bytes, packed));
        };
        // Every unsigned byte product fits exactly in 16 bits: [0,65025].
        // Widen each product
        // BEFORE summing: four products do not in general fit in 16 bits.
        // Keep the accumulator and sum in unsigned 32-bit arithmetic so
        // the SM6.4 wrapping contract never depends on float conversions.
        const u32 products = m_builder.emit_imul(shorts, unpack(values[1]), unpack(values[2]));
        const u32 wide = widen(m_type_uvec4, products);
        std::array<u32, 4> components{};
        for (u32 i = 0; i < components.size(); ++i)
            components[i] = m_builder.emit_composite_extract(m_type_uint, wide, {i});
        const u32 small_sum = m_builder.emit_iadd(m_type_uint,
            m_builder.emit_iadd(m_type_uint, components[0], components[1]),
            m_builder.emit_iadd(m_type_uint, components[2], components[3]));
        const u32 sum = m_builder.emit_iadd(m_type_uint, values[0], small_sum);
        const u32 result = m_builder.emit_composite_construct(m_type_uvec4, {sum, sum, sum, sum});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result));
    }
    case kOpViewId: {
        // SV_ViewID: a constant of this translation (the frontend builds
        // one pipeline per view instance; 0 when view instancing is off,
        // as a PSO with ViewInstanceCount 0 must still run the shader).
        Sm4Operand dst;
        if (bits != 0 || !m_decoder.decode_operand(dst)) return fail("VX_VIEWID: bad operands");
        const u32 id = m_builder.emit_constant_int(m_type_uint, m_options.view_id);
        const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {id, id, id, id});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
    }
    case kOpSamplerState: {
        // A word of the sampler's record (sampler_state_buffer), which only
        // a descriptor-table translation has.
        Sm4Operand dst, sampler;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(sampler) ||
            sampler.type != Sm4OperandType::Sampler || bits >= kSamplerStateRecordWords) {
            return fail("VX_SAMPLER_STATE: bad operands");
        }
        if (m_sampler_state_var == 0) {
            return fail("VX_SAMPLER_STATE: the translation has no sampler_state_buffer");
        }
        const ResourceRange* range = find_range(DxbcRegisterClass::Sampler, sampler.index0);
        if (range == nullptr || range->is_array || sampler.register_relative) {
            return fail("VX_SAMPLER_STATE: a sampler descriptor array");
        }
        const u32 w = sampler_state_word(sampler, bits);
        const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {w, w, w, w});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
    }
    case kOpMesh: return translate_vx_mesh(bits);
    case kOpRayQuery: return translate_vx_ray_query(bits);
    case kOpF32P: return translate_vx_f32p(bits);
    case kOpTex: return translate_vx_tex(bits);
    case kOpBary: return translate_vx_bary(bits);
    case kOpDrawParam: {
        if (m_result.stage != ShaderStage::Vertex) return fail("VX_DRAW_PARAM outside the vertex stage");
        Sm4Operand dst;
        if (!m_decoder.decode_operand(dst)) return fail("VX_DRAW_PARAM: bad destination");
        if (bits > 1u) return fail("VX_DRAW_PARAM: unknown parameter");
        // Metal [[base_vertex]] / [[base_instance]] (SPIR-V BaseVertex /
        // BaseInstance) are the draw's vertexStart or baseVertex and its
        // baseInstance: D3D's StartVertexLocation / BaseVertexLocation and
        // StartInstanceLocation (the same identity SV_VertexID and
        // SV_InstanceID are derived by, dxbc_to_spirv_signature.cpp).
        m_builder.emit_capability(SpvCapability::DrawParameters);
        m_builder.emit_extension("SPV_KHR_shader_draw_parameters");
        u32 value = m_builder.emit_load(
            m_type_uint, builtin_input(bits == 0u ? SpvBuiltIn::BaseVertex : SpvBuiltIn::BaseInstance,
                                       bits == 0u ? "vitreo_base_vertex" : "vitreo_base_instance"));
        if (bits == 1u && vs_capture()) {
            // A capture draws instance k alone as first instance start + k
            // and names k in push member 3 (kInstanceIdBiasOffset), as
            // SV_InstanceID reads it: the start is base - k.
            value = m_builder.emit_isub(
                m_type_uint, value,
                m_builder.emit_load(m_type_uint,
                                    m_builder.emit_access_chain(m_type_ptr_push_uint, push_block_var(),
                                                                {m_builder.emit_constant_int(m_type_uint, 3u)})));
        }
        const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {value, value, value, value});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
    }
    case kOpTexOffset: {
        Sm4Operand offset;
        if (!m_decoder.decode_operand(offset)) return fail("VX_TEX_OFFSET: bad operand");
        u32 value = 0;
        if (!load_vec4(offset, value)) return false;
        m_vx_dynamic_offset = m_builder.emit_bitcast(m_type_ivec4, value);
        return true;
    }
    default:
        return fail("vitreo extension opcode " + std::to_string(opcode) + " is not defined");
    }
}

// ---- 16-bit floats ------------------------------------------------------------

bool DxbcTranslator::translate_vx_f16(u32 control) {
    Sm4Operand dst, src;
    if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(src)) {
        return fail("VX_F16: bad operands");
    }
    u32 bits = 0;
    if (!load_bits(src, bits)) return false;
    const u32 half = vx_type_half();
    const u32 u16 = vx_type_u16();
    const u32 x = m_builder.emit_composite_extract(m_type_uint, bits, {0u});
    u32 result = 0;
    switch (static_cast<F16Op>(control)) {
    case F16Op::Round: {
        const u32 f = m_builder.emit_bitcast(m_type_float, x);
        const u32 h = m_builder.emit_unary_integer(SpvOp::OpFConvert, half, f);
        result = m_builder.emit_bitcast(m_type_uint, m_builder.emit_unary_integer(SpvOp::OpFConvert, m_type_float, h));
        break;
    }
    case F16Op::ToBits: {
        const u32 f = m_builder.emit_bitcast(m_type_float, x);
        const u32 h = m_builder.emit_unary_integer(SpvOp::OpFConvert, half, f);
        result = m_builder.emit_unary_integer(SpvOp::OpUConvert, m_type_uint, m_builder.emit_bitcast(u16, h));
        break;
    }
    case F16Op::FromBits: {
        const u32 s = m_builder.emit_unary_integer(SpvOp::OpUConvert, u16, x);
        const u32 h = m_builder.emit_bitcast(half, s);
        result = m_builder.emit_bitcast(m_type_uint, m_builder.emit_unary_integer(SpvOp::OpFConvert, m_type_float, h));
        break;
    }
    default:
        return fail("VX_F16: unknown operation " + std::to_string(control));
    }
    const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {result, result, result, result});
    return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
}

// ---- 64-bit integers ------------------------------------------------------------

bool DxbcTranslator::translate_vx_i64(u32 control) {
    const auto op = static_cast<I64Op>(control);
    if (control >= static_cast<u32>(I64Op::Count)) return fail("VX_I64: unknown operation");
    u32 sources = 2;
    switch (op) {
    case I64Op::SExt32: case I64Op::ZExt32: case I64Op::UToF32: case I64Op::SToF32:
    case I64Op::F32ToU: case I64Op::F32ToS: case I64Op::CountBits: case I64Op::FirstBitLo:
    case I64Op::FirstBitHi: case I64Op::FirstBitSHi: case I64Op::BitReverse:
        sources = 1;
        break;
    case I64Op::Mad: sources = 3; break;
    default: break;
    }
    Sm4Operand dst, s[3];
    if (!m_decoder.decode_operand(dst)) return fail("VX_I64: bad operands");
    u32 v[3] = {0, 0, 0};
    for (u32 k = 0; k < sources; ++k) {
        if (!m_decoder.decode_operand(s[k]) || !load_bits(s[k], v[k])) return fail("VX_I64: bad operands");
    }
    const u32 u64 = vx_type_u64();
    const u32 i64 = vx_type_i64();
    const auto uint = [&](u32 value) { return m_builder.emit_constant_int(m_type_uint, value); };
    const auto lo = [&](u32 k) { return m_builder.emit_composite_extract(m_type_uint, v[k], {0u}); };
    const auto hi = [&](u32 k) { return m_builder.emit_composite_extract(m_type_uint, v[k], {1u}); };
    const auto wide = [&](u32 k) { return vx_u64_of(v[k]); };
    const auto k64 = [&](u32 l, u32 h) {
        return m_builder.emit_bitcast(u64, m_builder.emit_constant_composite(m_type_uvec2, {uint(l), uint(h)}));
    };
    const auto mask = [&](u32 cond) { return m_builder.emit_select(m_type_uint, cond, uint(~0u), uint(0)); };
    const auto cmp = [&](SpvOp o, u32 a, u32 b) { return m_builder.emit_integer_compare(o, m_type_bool, a, b); };
    const auto binop = [&](SpvOp o, u32 type, u32 a, u32 b) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        const u32 id = m_builder.allocate_id();
        m_builder.emit(o, {type, id, a, b});
        return id;
    };
    const auto as_i64 = [&](u32 x) { return m_builder.emit_bitcast(i64, x); };
    const auto as_u64 = [&](u32 x) { return m_builder.emit_bitcast(u64, x); };
    u32 pair = 0;    // a u64 result
    u32 scalar = 0;  // a 32-bit result
    switch (op) {
    case I64Op::Add: pair = m_builder.emit_iadd(u64, wide(0), wide(1)); break;
    case I64Op::Sub: pair = m_builder.emit_isub(u64, wide(0), wide(1)); break;
    case I64Op::Mul: pair = m_builder.emit_imul(u64, wide(0), wide(1)); break;
    case I64Op::Mad:
        pair = m_builder.emit_iadd(u64, m_builder.emit_imul(u64, wide(0), wide(1)), wide(2));
        break;
    case I64Op::UDiv:
    case I64Op::URem: {
        const u32 a = wide(0), b = wide(1);
        const u32 zero = cmp(SpvOp::OpIEqual, b, k64(0, 0));
        const u32 safe = m_builder.emit_select(u64, zero, k64(1, 0), b);
        const u32 r = op == I64Op::UDiv ? m_builder.emit_udiv(u64, a, safe) : m_builder.emit_umod(u64, a, safe);
        pair = m_builder.emit_select(u64, zero, k64(~0u, ~0u), r);
        break;
    }
    case I64Op::SDiv:
    case I64Op::SRem: {
        // b == 0 gives all ones; INT64_MIN / -1 divides by 1 instead, which
        // is the wrapped quotient (INT64_MIN) and the remainder 0.
        const u32 a = wide(0), b = wide(1);
        const u32 zero = cmp(SpvOp::OpIEqual, b, k64(0, 0));
        const u32 overflow = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool,
                                                    cmp(SpvOp::OpIEqual, a, k64(0, 0x80000000u)),
                                                    cmp(SpvOp::OpIEqual, b, k64(~0u, ~0u)));
        const u32 bad = m_builder.emit_logical(SpvOp::OpLogicalOr, m_type_bool, zero, overflow);
        const u32 safe = as_i64(m_builder.emit_select(u64, bad, k64(1, 0), b));
        const u32 r = binop(op == I64Op::SDiv ? SpvOp::OpSDiv : SpvOp::OpSRem, i64, as_i64(a), safe);
        pair = m_builder.emit_select(u64, zero, k64(~0u, ~0u), as_u64(r));
        break;
    }
    case I64Op::Shl:
    case I64Op::LShr:
    case I64Op::AShr: {
        const u32 count = m_builder.emit_unary_integer(
            SpvOp::OpUConvert, u64, m_builder.emit_bitwise_and(m_type_uint, lo(1), uint(63)));
        if (op == I64Op::Shl) pair = m_builder.emit_shift_left_logical(u64, wide(0), count);
        else if (op == I64Op::LShr) pair = m_builder.emit_shift_right_logical(u64, wide(0), count);
        else pair = as_u64(m_builder.emit_shift_right_arithmetic(i64, as_i64(wide(0)), count));
        break;
    }
    case I64Op::And: pair = m_builder.emit_bitwise_and(u64, wide(0), wide(1)); break;
    case I64Op::Or: pair = m_builder.emit_bitwise_or(u64, wide(0), wide(1)); break;
    case I64Op::Xor: pair = m_builder.emit_bitwise_xor(u64, wide(0), wide(1)); break;
    case I64Op::UMin: case I64Op::UMax: case I64Op::SMin: case I64Op::SMax: {
        const u32 a = wide(0), b = wide(1);
        const bool sign = op == I64Op::SMin || op == I64Op::SMax;
        const u32 less = sign ? cmp(SpvOp::OpSLessThan, as_i64(a), as_i64(b)) : cmp(SpvOp::OpULessThan, a, b);
        const bool min = op == I64Op::UMin || op == I64Op::SMin;
        pair = m_builder.emit_select(u64, less, min ? a : b, min ? b : a);
        break;
    }
    case I64Op::Eq: scalar = mask(cmp(SpvOp::OpIEqual, wide(0), wide(1))); break;
    case I64Op::Ne: scalar = mask(cmp(SpvOp::OpINotEqual, wide(0), wide(1))); break;
    case I64Op::ULt: scalar = mask(cmp(SpvOp::OpULessThan, wide(0), wide(1))); break;
    case I64Op::UGe: scalar = mask(cmp(SpvOp::OpUGreaterThanEqual, wide(0), wide(1))); break;
    case I64Op::SLt: scalar = mask(cmp(SpvOp::OpSLessThan, as_i64(wide(0)), as_i64(wide(1)))); break;
    case I64Op::SGe: scalar = mask(cmp(SpvOp::OpSGreaterThanEqual, as_i64(wide(0)), as_i64(wide(1)))); break;
    case I64Op::SExt32:
        pair = as_u64(m_builder.emit_unary_integer(SpvOp::OpSConvert, i64, m_builder.emit_bitcast(m_type_int, lo(0))));
        break;
    case I64Op::ZExt32: pair = m_builder.emit_unary_integer(SpvOp::OpUConvert, u64, lo(0)); break;
    case I64Op::UToF32: scalar = m_builder.emit_bitcast(m_type_uint, m_builder.emit_convert_u_to_f(m_type_float, wide(0))); break;
    case I64Op::SToF32:
        scalar = m_builder.emit_bitcast(m_type_uint, m_builder.emit_convert_s_to_f(m_type_float, as_i64(wide(0))));
        break;
    case I64Op::F32ToU:
    case I64Op::F32ToS: {
        // NaN -> 0; values beyond the range saturate (SM5 ftou / ftoi).
        const u32 f = m_builder.emit_bitcast(m_type_float, lo(0));
        const u32 nan = m_builder.emit_is_nan(m_type_bool, f);
        const auto fconst = [&](float value) { return m_builder.emit_constant_float(m_type_float, value); };
        if (op == I64Op::F32ToU) {
            const u32 low = m_builder.emit_ford_lt(m_type_bool, f, fconst(1.0f));
            const u32 high = m_builder.emit_ford_ge(m_type_bool, f, fconst(18446744073709551616.0f));
            const u32 clamped = m_builder.emit_select(m_type_float, low, fconst(0.0f), f);
            u32 r = m_builder.emit_convert_f_to_u(u64, m_builder.emit_select(m_type_float, high, fconst(1.0f), clamped));
            r = m_builder.emit_select(u64, high, k64(~0u, ~0u), r);
            pair = m_builder.emit_select(u64, nan, k64(0, 0), r);
        } else {
            const u32 low = m_builder.emit_ford_lt(m_type_bool, f, fconst(-9223372036854775808.0f));
            const u32 high = m_builder.emit_ford_ge(m_type_bool, f, fconst(9223372036854775808.0f));
            const u32 safe = m_builder.emit_select(m_type_float,
                                                   m_builder.emit_logical(SpvOp::OpLogicalOr, m_type_bool, low, high),
                                                   fconst(0.0f), f);
            u32 r = as_u64(m_builder.emit_convert_f_to_s(i64, safe));
            r = m_builder.emit_select(u64, low, k64(0, 0x80000000u), r);
            r = m_builder.emit_select(u64, high, k64(~0u, 0x7FFFFFFFu), r);
            pair = m_builder.emit_select(u64, nan, k64(0, 0), r);
        }
        break;
    }
    case I64Op::CountBits:
        scalar = m_builder.emit_iadd(m_type_uint, m_builder.emit_unary_integer(SpvOp::OpBitCount, m_type_uint, lo(0)),
                                     m_builder.emit_unary_integer(SpvOp::OpBitCount, m_type_uint, hi(0)));
        break;
    case I64Op::FirstBitLo: {
        const u32 l = lo(0), h = hi(0);
        const u32 lsb_l = m_builder.emit_ext_inst(m_type_uint, m_glsl_std450, kGlslStd450FindILsb, {l});
        const u32 lsb_h = m_builder.emit_ext_inst(m_type_uint, m_glsl_std450, kGlslStd450FindILsb, {h});
        const u32 high = m_builder.emit_select(m_type_uint, cmp(SpvOp::OpIEqual, h, uint(0)), uint(~0u),
                                               m_builder.emit_iadd(m_type_uint, lsb_h, uint(32)));
        scalar = m_builder.emit_select(m_type_uint, cmp(SpvOp::OpIEqual, l, uint(0)), high, lsb_l);
        break;
    }
    case I64Op::FirstBitHi:
    case I64Op::FirstBitSHi: {
        // DXIL FirstbitHi / FirstbitSHi, like SM5 firstbit_hi: the
        // position counted from the most significant bit (0 = bit 63), ~0
        // when no bit qualifies. For the signed form the bits searched are
        // those differing from the sign: the complement of a negative value.
        u32 l = lo(0), h = hi(0);
        if (op == I64Op::FirstBitSHi) {
            const u32 negative = cmp(SpvOp::OpSLessThan, m_builder.emit_bitcast(m_type_int, h), m_builder.emit_constant_int(m_type_int, 0));
            l = m_builder.emit_select(m_type_uint, negative, m_builder.emit_not(m_type_uint, l), l);
            h = m_builder.emit_select(m_type_uint, negative, m_builder.emit_not(m_type_uint, h), h);
        }
        const u32 msb_l = m_builder.emit_ext_inst(m_type_uint, m_glsl_std450, kGlslStd450FindUMsb, {l});
        const u32 msb_h = m_builder.emit_ext_inst(m_type_uint, m_glsl_std450, kGlslStd450FindUMsb, {h});
        const u32 from_h = m_builder.emit_isub(m_type_uint, uint(31), msb_h);
        const u32 from_l = m_builder.emit_isub(m_type_uint, uint(63), msb_l);
        const u32 low = m_builder.emit_select(m_type_uint, cmp(SpvOp::OpIEqual, l, uint(0)), uint(~0u), from_l);
        scalar = m_builder.emit_select(m_type_uint, cmp(SpvOp::OpIEqual, h, uint(0)), low, from_h);
        break;
    }
    case I64Op::BitReverse: {
        const u32 rl = m_builder.emit_unary_integer(SpvOp::OpBitReverse, m_type_uint, lo(0));
        const u32 rh = m_builder.emit_unary_integer(SpvOp::OpBitReverse, m_type_uint, hi(0));
        const u32 r = m_builder.emit_composite_construct(m_type_uvec4, {rh, rl, rh, rl});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, r));
    }
    default:
        return fail("VX_I64: unknown operation");
    }
    const u32 r = pair != 0 ? vx_uvec4_of_u64(pair)
                            : m_builder.emit_composite_construct(m_type_uvec4, {scalar, scalar, scalar, scalar});
    return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, r));
}

// ---- waves --------------------------------------------------------------------

u32 DxbcTranslator::vx_lane_loop(u32 kind_bits, u32 op_bits, u32 type_bits, u32 lo, u32 hi, u32 mask) {
    // for (k = 0; k < WaveGetLaneCount(); ++k) if (lane k qualifies)
    //     acc = acc (op) value of lane k
    const auto kind = static_cast<WaveKind>(kind_bits);
    const auto op = static_cast<WaveOp>(op_bits);
    const auto type = static_cast<WaveType>(type_bits);
    m_builder.emit_capability(SpvCapability::GroupNonUniformShuffle);
    const bool is64 = wave_type_is_64(type);
    const bool fp = type == WaveType::F32 || type == WaveType::F16;
    const bool match = kind == WaveKind::Match;
    const bool count = kind == WaveKind::MultiPrefixCountBits;
    const bool prefix = kind == WaveKind::Prefix || kind == WaveKind::MultiPrefix || count;
    const auto uint = [&](u32 value) { return m_builder.emit_constant_int(m_type_uint, value); };
    const u32 scope = uint(kScopeSubgroup);
    const u32 u64 = is64 ? vx_type_u64() : 0;
    const u32 i64 = is64 ? vx_type_i64() : 0;
    const u32 acc_type = match ? m_type_uvec4 : is64 ? u64 : m_type_uint;
    const u32 acc_ptr = m_builder.emit_type_pointer(SpvStorageClass::Private, acc_type);
    const u32 uint_ptr = m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint);
    const u32 acc = m_builder.emit_variable(acc_ptr, SpvStorageClass::Private);
    const u32 k_var = m_builder.emit_variable(uint_ptr, SpvStorageClass::Private);
    // Identity. Min / Max never reach the loop for 32-bit values, and an
    // Active reduction always includes the lane itself.
    u32 identity = 0;
    const bool product = op == WaveOp::Product;
    const bool all_ones = op == WaveOp::And;
    if (match) {
        identity = m_const_u_zero4;
    } else if (is64) {
        u32 l = product ? 1u : all_ones ? ~0u : 0u, h = all_ones ? ~0u : 0u;
        if (op == WaveOp::Min) { l = ~0u; h = type == WaveType::I64 ? 0x7FFFFFFFu : ~0u; }
        if (op == WaveOp::Max) { l = 0; h = type == WaveType::I64 ? 0x80000000u : 0u; }
        identity = m_builder.emit_bitcast(u64, m_builder.emit_constant_composite(m_type_uvec2, {uint(l), uint(h)}));
    } else if (fp && !count) {
        identity = uint(product ? 0x3F800000u : 0u);
    } else {
        identity = uint(product ? 1u : all_ones ? ~0u : 0u);
    }
    const u32 active = vx_ballot(m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, uint(0), uint(0)));
    const u32 self = vx_lane();
    const u32 size = vx_builtin(SpvBuiltIn::SubgroupSize, m_type_ptr_input_uint, m_type_uint,
                                m_vx_subgroup_size, "vitreo_wave_size");
    const u32 mine = is64 ? m_builder.emit_bitcast(u64, m_builder.emit_composite_construct(m_type_uvec2, {lo, hi})) : lo;
    m_builder.emit_store(acc, identity);
    m_builder.emit_store(k_var, uint(0));
    const u32 header = m_builder.allocate_id();
    const u32 body = m_builder.allocate_id();
    const u32 cont = m_builder.allocate_id();
    const u32 merge = m_builder.allocate_id();
    m_builder.emit_branch(header);
    m_builder.emit_label_id(header);
    const u32 k = m_builder.emit_load(m_type_uint, k_var);
    const u32 go = m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool, k, size);
    m_builder.emit_loop_merge(merge, cont);
    m_builder.emit_branch_conditional(go, body, merge);
    m_builder.emit_label_id(body);
    const auto shuffle = [&](u32 value) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        const u32 id = m_builder.allocate_id();
        m_builder.emit(SpvOp::OpGroupNonUniformShuffle, {m_type_uint, id, scope, value, k});
        return id;
    };
    // Bit k of a ballot-shaped uvec4.
    const auto bit_of = [&](u32 vec) {
        const u32 word_index = m_builder.emit_shift_right_logical(m_type_uint, k, uint(5));
        u32 word = m_builder.emit_composite_extract(m_type_uint, vec, {0u});
        for (u32 w = 1; w < 4; ++w) {
            word = m_builder.emit_select(m_type_uint,
                                         m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, word_index, uint(w)),
                                         m_builder.emit_composite_extract(m_type_uint, vec, {w}), word);
        }
        const u32 bit = m_builder.emit_bitwise_and(
            m_type_uint, m_builder.emit_shift_right_logical(m_type_uint, word, m_builder.emit_bitwise_and(m_type_uint, k, uint(31))),
            uint(1));
        return m_builder.emit_integer_compare(SpvOp::OpINotEqual, m_type_bool, bit, uint(0));
    };
    u32 take = bit_of(active);
    if (prefix) {
        take = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, take,
                                      m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool, k, self));
    }
    if (mask != 0) take = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, take, bit_of(mask));
    const u32 value_lo = shuffle(lo);
    u32 value = value_lo;
    if (is64) {
        value = m_builder.emit_bitcast(u64, m_builder.emit_composite_construct(m_type_uvec2, {value_lo, shuffle(hi)}));
    }
    const u32 current = m_builder.emit_load(acc_type, acc);
    u32 next = 0;
    if (match) {
        // Set bit k where lane k holds this lane's value.
        const u32 equal = m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, value, mine);
        const u32 hit = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, take, equal);
        const u32 bit = m_builder.emit_shift_left_logical(m_type_uint, uint(1), m_builder.emit_bitwise_and(m_type_uint, k, uint(31)));
        const u32 word_index = m_builder.emit_shift_right_logical(m_type_uint, k, uint(5));
        std::vector<u32> words;
        for (u32 w = 0; w < 4; ++w) {
            const u32 old = m_builder.emit_composite_extract(m_type_uint, current, {w});
            const u32 here = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, hit,
                                                    m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, word_index, uint(w)));
            words.push_back(m_builder.emit_select(m_type_uint, here, m_builder.emit_bitwise_or(m_type_uint, old, bit), old));
        }
        next = m_builder.emit_composite_construct(m_type_uvec4, words);
    } else {
        u32 combined = 0;
        const u32 t = acc_type;
        if (count) {
            const u32 set = m_builder.emit_integer_compare(SpvOp::OpINotEqual, m_type_bool, value, uint(0));
            combined = m_builder.emit_select(m_type_uint, set, m_builder.emit_iadd(m_type_uint, current, uint(1)), current);
        } else if (fp) {
            const u32 a = m_builder.emit_bitcast(m_type_float, current), b = m_builder.emit_bitcast(m_type_float, value);
            combined = m_builder.emit_bitcast(m_type_uint, product ? m_builder.emit_fmul(m_type_float, a, b)
                                                                   : m_builder.emit_fadd(m_type_float, a, b));
        } else {
            switch (op) {
            case WaveOp::Sum: combined = m_builder.emit_iadd(t, current, value); break;
            case WaveOp::Product: combined = m_builder.emit_imul(t, current, value); break;
            case WaveOp::And: combined = m_builder.emit_bitwise_and(t, current, value); break;
            case WaveOp::Or: combined = m_builder.emit_bitwise_or(t, current, value); break;
            case WaveOp::Xor: combined = m_builder.emit_bitwise_xor(t, current, value); break;
            case WaveOp::Min:
            case WaveOp::Max: {
                const bool sign = type == WaveType::I64;
                const u32 less = sign ? m_builder.emit_integer_compare(SpvOp::OpSLessThan, m_type_bool,
                                                                       m_builder.emit_bitcast(i64, value),
                                                                       m_builder.emit_bitcast(i64, current))
                                      : m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool, value, current);
                combined = op == WaveOp::Min ? m_builder.emit_select(t, less, value, current)
                                             : m_builder.emit_select(t, less, current, value);
                break;
            }
            default: combined = current; break;
            }
        }
        next = m_builder.emit_select(acc_type, take, combined, current);
    }
    m_builder.emit_store(acc, next);
    m_builder.emit_branch(cont);
    m_builder.emit_label_id(cont);
    m_builder.emit_store(k_var, m_builder.emit_iadd(m_type_uint, k, uint(1)));
    m_builder.emit_branch(header);
    m_builder.emit_label_id(merge);
    const u32 result = m_builder.emit_load(acc_type, acc);
    if (match) return result;
    if (is64) return vx_uvec4_of_u64(result);
    return m_builder.emit_composite_construct(m_type_uvec4, {result, result, result, result});
}

bool DxbcTranslator::translate_vx_wave(u32 control) {
    const auto kind = static_cast<WaveKind>(control & 0xFFu);
    const auto op = static_cast<WaveOp>((control >> 8) & 0xFFu);
    const auto type = static_cast<WaveType>((control >> 16) & 0xFFu);
    if (static_cast<u32>(kind) > static_cast<u32>(WaveKind::IsHelperLane) ||
        static_cast<u32>(op) > static_cast<u32>(WaveOp::Xor) ||
        static_cast<u32>(type) > static_cast<u32>(WaveType::F16)) {
        return fail("VX_WAVE: unknown control " + std::to_string(control));
    }
    u32 sources = 1;
    switch (kind) {
    case WaveKind::IsFirstLane: case WaveKind::LaneIndex: case WaveKind::LaneCount: case WaveKind::IsHelperLane:
        sources = 0;
        break;
    case WaveKind::ReadLaneAt: case WaveKind::MultiPrefix: case WaveKind::MultiPrefixCountBits:
    case WaveKind::QuadReadLaneAt:
        sources = 2;
        break;
    default: break;
    }
    Sm4Operand dst, s[2];
    if (!m_decoder.decode_operand(dst)) return fail("VX_WAVE: bad operands");
    for (u32 k = 0; k < sources; ++k) {
        if (!m_decoder.decode_operand(s[k])) return fail("VX_WAVE: bad operands");
    }
    m_builder.emit_capability(SpvCapability::GroupNonUniform);
    const auto uint = [&](u32 value) { return m_builder.emit_constant_int(m_type_uint, value); };
    const u32 scope = uint(kScopeSubgroup);
    const bool quad = kind >= WaveKind::QuadReadLaneAt;
    const bool query = kind == WaveKind::LaneIndex || kind == WaveKind::LaneCount || kind == WaveKind::IsHelperLane;
    // waveops-include-helper-lanes: helper lanes are active lanes.
    const bool include_helpers = (control & kWaveIncludeHelpers) != 0;
    const bool is64 = wave_type_is_64(type);
    const bool fp = type == WaveType::F32 || type == WaveType::F16;
    const auto group = [&](SpvOp o, u32 result_type, std::vector<u32> operands) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        const u32 id = m_builder.allocate_id();
        std::vector<u32> words{result_type, id, scope};
        words.insert(words.end(), operands.begin(), operands.end());
        m_builder.emit(o, words);
        return id;
    };
    const auto splat = [&](u32 x) { return m_builder.emit_composite_construct(m_type_uvec4, {x, x, x, x}); };
    const auto mask = [&](u32 cond) { return m_builder.emit_select(m_type_uint, cond, uint(~0u), uint(0)); };
    const auto nonzero = [&](u32 x) { return m_builder.emit_integer_compare(SpvOp::OpINotEqual, m_type_bool, x, uint(0)); };
    const auto lane_index = [&]() { return vx_lane(); };

    // Helper lanes take no part (pixel shaders): everything but the lane
    // queries and the quad operations runs where !HelperInvocation.
    u32 merge = 0;
    if (m_result.stage == ShaderStage::Fragment && !quad && !query && !include_helpers) {
        const u32 helper = vx_builtin(SpvBuiltIn::HelperInvocation, m_type_ptr_input_bool, m_type_bool,
                                      m_vx_helper, "vitreo_wave_helper");
        const u32 run = m_builder.allocate_id();
        merge = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge);
        m_builder.emit_branch_conditional(helper, merge, run);
        m_builder.emit_label_id(run);
    }

    u32 v[2] = {0, 0};
    for (u32 k = 0; k < sources; ++k) {
        if (!load_bits(s[k], v[k])) return false;
    }
    const auto comp = [&](u32 k, u32 c) { return m_builder.emit_composite_extract(m_type_uint, v[k], {c}); };
    // A 16-bit integer result is its low 16 bits; a half result is rounded.
    const auto narrow = [&](u32 x) {
        if (type == WaveType::U16 || type == WaveType::I16) {
            return m_builder.emit_bitwise_and(m_type_uint, x, uint(0xFFFFu));
        }
        if (type == WaveType::F16) {
            const u32 h = m_builder.emit_unary_integer(SpvOp::OpFConvert, vx_type_half(),
                                                       m_builder.emit_bitcast(m_type_float, x));
            return m_builder.emit_bitcast(m_type_uint, m_builder.emit_unary_integer(SpvOp::OpFConvert, m_type_float, h));
        }
        return x;
    };
    // Per 32-bit half of the value: the same lane-to-lane movement.
    const auto per_word = [&](const auto& move) {
        const u32 l = move(comp(0, 0));
        if (!is64) return splat(l);
        const u32 h = move(comp(0, 1));
        return m_builder.emit_composite_construct(m_type_uvec4, {l, h, l, h});
    };
    u32 result = 0;
    switch (kind) {
    case WaveKind::IsFirstLane:
        result = splat(mask(m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, lane_index(), vx_first_lane())));
        break;
    case WaveKind::LaneIndex:
        result = splat(lane_index());
        break;
    case WaveKind::IsHelperLane:
        if (m_result.stage == ShaderStage::Fragment) {
            result = splat(mask(vx_builtin(SpvBuiltIn::HelperInvocation, m_type_ptr_input_bool, m_type_bool,
                                           m_vx_helper, "vitreo_wave_helper")));
        } else {
            result = splat(uint(0));
        }
        break;
    case WaveKind::LaneCount:
        result = splat(vx_builtin(SpvBuiltIn::SubgroupSize, m_type_ptr_input_uint, m_type_uint, m_vx_subgroup_size,
                                  "vitreo_wave_size"));
        break;
    case WaveKind::AnyTrue:
    case WaveKind::AllTrue:
        m_builder.emit_capability(SpvCapability::GroupNonUniformVote);
        result = splat(mask(group(kind == WaveKind::AnyTrue ? SpvOp::OpGroupNonUniformAny : SpvOp::OpGroupNonUniformAll,
                                  m_type_bool, {nonzero(comp(0, 0))})));
        break;
    case WaveKind::AllEqual: {
        // Every lane equal to the first active lane's value: float
        // equality for floats (-0 equals +0, NaN equals nothing), bits
        // otherwise.
        m_builder.emit_capability(SpvCapability::GroupNonUniformVote);
        m_builder.emit_capability(SpvCapability::GroupNonUniformShuffle);
        const u32 first = vx_first_lane();
        const auto equal_first = [&](u32 x) {
            const u32 theirs = group(SpvOp::OpGroupNonUniformShuffle, m_type_uint, {x, first});
            if (fp) {
                return m_builder.emit_ford_eq(m_type_bool, m_builder.emit_bitcast(m_type_float, x),
                                              m_builder.emit_bitcast(m_type_float, theirs));
            }
            return m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, x, theirs);
        };
        u32 equal = equal_first(comp(0, 0));
        if (is64) equal = m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, equal, equal_first(comp(0, 1)));
        result = splat(mask(group(SpvOp::OpGroupNonUniformAll, m_type_bool, {equal})));
        break;
    }
    case WaveKind::Ballot:
        result = vx_ballot(nonzero(comp(0, 0)));
        break;
    case WaveKind::ReadLaneAt: {
        m_builder.emit_capability(SpvCapability::GroupNonUniformShuffle);
        const u32 lane = comp(1, 0);
        result = per_word([&](u32 x) { return group(SpvOp::OpGroupNonUniformShuffle, m_type_uint, {x, lane}); });
        break;
    }
    case WaveKind::ReadFirst: {
        m_builder.emit_capability(SpvCapability::GroupNonUniformShuffle);
        const u32 first = vx_first_lane();
        result = per_word([&](u32 x) { return group(SpvOp::OpGroupNonUniformShuffle, m_type_uint, {x, first}); });
        break;
    }
    case WaveKind::ActiveCountBits:
    case WaveKind::PrefixCountBits: {
        m_builder.emit_capability(SpvCapability::GroupNonUniformArithmetic);
        const u32 one = m_builder.emit_select(m_type_uint, nonzero(comp(0, 0)), uint(1), uint(0));
        result = splat(group(SpvOp::OpGroupNonUniformIAdd, m_type_uint,
                             {kind == WaveKind::ActiveCountBits ? kGroupReduce : kGroupExclusiveScan, one}));
        break;
    }
    case WaveKind::Active:
    case WaveKind::Prefix: {
        const bool bitwise = op == WaveOp::And || op == WaveOp::Or || op == WaveOp::Xor;
        if (is64 && !(bitwise && kind == WaveKind::Active)) {
            result = vx_lane_loop(static_cast<u32>(kind), static_cast<u32>(op), static_cast<u32>(type),
                                  comp(0, 0), comp(0, 1), 0);
            break;
        }
        m_builder.emit_capability(SpvCapability::GroupNonUniformArithmetic);
        const u32 operation = kind == WaveKind::Active ? kGroupReduce : kGroupExclusiveScan;
        if (fp) {
            SpvOp o = SpvOp::OpGroupNonUniformFAdd;
            if (op == WaveOp::Product) o = SpvOp::OpGroupNonUniformFMul;
            else if (op == WaveOp::Min) o = SpvOp::OpGroupNonUniformFMin;
            else if (op == WaveOp::Max) o = SpvOp::OpGroupNonUniformFMax;
            else if (op != WaveOp::Sum) return fail("VX_WAVE: bitwise operation on a float");
            const u32 r = group(o, m_type_float, {operation, m_builder.emit_bitcast(m_type_float, comp(0, 0))});
            result = splat(narrow(m_builder.emit_bitcast(m_type_uint, r)));
            break;
        }
        const bool sign = type == WaveType::I32 || type == WaveType::I16;
        SpvOp o = SpvOp::OpGroupNonUniformIAdd;
        switch (op) {
        case WaveOp::Sum: o = SpvOp::OpGroupNonUniformIAdd; break;
        case WaveOp::Product: o = SpvOp::OpGroupNonUniformIMul; break;
        case WaveOp::Min: o = sign ? SpvOp::OpGroupNonUniformSMin : SpvOp::OpGroupNonUniformUMin; break;
        case WaveOp::Max: o = sign ? SpvOp::OpGroupNonUniformSMax : SpvOp::OpGroupNonUniformUMax; break;
        case WaveOp::And: o = SpvOp::OpGroupNonUniformBitwiseAnd; break;
        case WaveOp::Or: o = SpvOp::OpGroupNonUniformBitwiseOr; break;
        case WaveOp::Xor: o = SpvOp::OpGroupNonUniformBitwiseXor; break;
        default: return fail("VX_WAVE: no operation");
        }
        if (is64) {  // bitwise reduction of a pair: word by word
            result = per_word([&](u32 x) { return group(o, m_type_uint, {operation, x}); });
            break;
        }
        u32 x = comp(0, 0);
        const bool minmax = op == WaveOp::Min || op == WaveOp::Max;
        if (sign && minmax) {
            if (type == WaveType::I16) {
                x = m_builder.emit_bit_field_extract(SpvOp::OpBitFieldSExtract, m_type_int,
                                                     m_builder.emit_bitcast(m_type_int, x), uint(0), uint(16));
            } else {
                x = m_builder.emit_bitcast(m_type_int, x);
            }
            const u32 r = group(o, m_type_int, {operation, x});
            result = splat(narrow(m_builder.emit_bitcast(m_type_uint, r)));
        } else {
            result = splat(narrow(group(o, m_type_uint, {operation, x})));
        }
        break;
    }
    case WaveKind::Match:
        result = vx_lane_loop(static_cast<u32>(kind), 0, static_cast<u32>(type), comp(0, 0),
                              is64 ? comp(0, 1) : uint(0), 0);
        break;
    case WaveKind::MultiPrefix:
    case WaveKind::MultiPrefixCountBits: {
        result = vx_lane_loop(static_cast<u32>(kind), static_cast<u32>(op), static_cast<u32>(type), comp(0, 0),
                              is64 ? comp(0, 1) : uint(0), v[1]);
        if (kind == WaveKind::MultiPrefix && !is64) {
            result = splat(narrow(m_builder.emit_composite_extract(m_type_uint, result, {0u})));
        }
        break;
    }
    case WaveKind::QuadReadLaneAt: {
        m_builder.emit_capability(SpvCapability::GroupNonUniformQuad);
        if (s[1].is_immediate || s[1].type == Sm4OperandType::Immediate32) {
            const u32 index = uint(immediate_bits(s[1]) & 3u);
            result = per_word([&](u32 x) { return m_builder.emit_quad_broadcast(m_type_uint, scope, x, index); });
        } else {
            // A lane chosen at run time: the shuffle from this quad's lane.
            m_builder.emit_capability(SpvCapability::GroupNonUniformShuffle);
            const u32 base = m_builder.emit_bitwise_and(m_type_uint, lane_index(), uint(~3u));
            const u32 lane = m_builder.emit_bitwise_or(m_type_uint, base,
                                                       m_builder.emit_bitwise_and(m_type_uint, comp(1, 0), uint(3)));
            result = per_word([&](u32 x) { return group(SpvOp::OpGroupNonUniformShuffle, m_type_uint, {x, lane}); });
        }
        break;
    }
    case WaveKind::QuadAcrossX:
    case WaveKind::QuadAcrossY:
    case WaveKind::QuadDiagonal: {
        m_builder.emit_capability(SpvCapability::GroupNonUniformQuad);
        // QuadSwap Direction: 0 horizontal (X), 1 vertical (Y), 2 diagonal.
        const u32 direction = uint(static_cast<u32>(kind) - static_cast<u32>(WaveKind::QuadAcrossX));
        result = per_word([&](u32 x) { return group(SpvOp::OpGroupNonUniformQuadSwap, m_type_uint, {x, direction}); });
        break;
    }
    default:
        return fail("VX_WAVE: unknown kind");
    }
    if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) return false;
    if (merge != 0) {
        m_builder.emit_branch(merge);
        m_builder.emit_label_id(merge);
    }
    return true;
}

}  // namespace vitreo::graphics::dxbc_spirv_detail

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::declare_readonly_indexable_arrays() {
    // LLVM 3.7 global constants / DXIL.rst Immediate Constant Buffer: immutable
    // module data, not per-invocation stores. Nested arrays keep every SPIR-V
    // instruction below its 16-bit WordCount limit without a table-size cap.
    constexpr u32 radix = 1024u;
    for (const auto& [slot, words] : m_readonly_indexable_words) {
        auto found = m_readonly_indexable_vars.find(slot);
        if (found == m_readonly_indexable_vars.end()) {
            std::vector<u32> dimensions;
            u64 count = words.size();
            while (count > radix) {
                dimensions.push_back(radix);
                count = (count + radix - 1u) / radix;
            }
            dimensions.push_back(static_cast<u32>(count));
            std::reverse(dimensions.begin(), dimensions.end());
            std::vector<u32> types(dimensions.size()), strides(dimensions.size(), 1u);
            u32 element = m_type_uint;
            for (usize i = dimensions.size(); i-- > 0;) {
                types[i] = m_builder.emit_type_array(element,
                    m_builder.emit_constant_int(m_type_uint, dimensions[i]));
                element = types[i];
                if (i + 1 < dimensions.size())
                    strides[i] = strides[i + 1] * dimensions[i + 1];
            }
            std::vector<u32> zero_nodes(dimensions.size(), 0);
            std::function<u32(usize, u64)> node = [&](usize depth, u64 start) -> u32 {
                if (start >= words.size() && zero_nodes[depth]) return zero_nodes[depth];
                std::vector<u32> values;
                values.reserve(dimensions[depth]);
                for (u32 i = 0; i < dimensions[depth]; ++i) {
                    const u64 offset = start + static_cast<u64>(i) * strides[depth];
                    values.push_back(depth + 1 == dimensions.size()
                        ? m_builder.emit_constant_int(m_type_uint,
                            offset < words.size() ? words[static_cast<usize>(offset)] : 0u)
                        : node(depth + 1, offset));
                }
                const u32 result = m_builder.emit_constant_composite(types[depth], values);
                if (start >= words.size()) zero_nodes[depth] = result;
                return result;
            };
            const u32 initializer = node(0, 0);
            const u32 pointer = m_builder.emit_type_pointer(SpvStorageClass::Private, types[0]);
            const u32 variable = m_builder.emit_variable(pointer, SpvStorageClass::Private, initializer);
            m_builder.emit_name(variable, "dxil_const_x" + std::to_string(slot));
            const u32 scalar_pointer = m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint);
            found = m_readonly_indexable_vars.emplace(slot,
                IndexableTempVar{variable, m_type_uint, 1, scalar_pointer, std::move(strides)}).first;
        }
        m_indexable_temp_vars.emplace(slot, found->second);
    }
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
