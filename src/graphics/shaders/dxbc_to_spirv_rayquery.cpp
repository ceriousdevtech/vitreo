// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_rayquery.cpp
/// @brief Inline ray tracing (DXR 1.1 RayQuery, DXIL.rst opcodes 178-215):
/// the Vitreo acceleration-structure declaration and ray-query instruction
/// (dxbc_vitreo_ext.hpp) as SPV_KHR_ray_query, which SPIRV-Cross writes as
/// Metal's raytracing::intersection_query.
///
/// Semantics (DirectX-Specs Raytracing.md, "RayQuery"): a query is
/// initialised by TraceRayInline with the template flags ORed into the
/// dynamic ones; Proceed advances it; the candidate and committed getters
/// read the current state. Metal has no getter for a query's ray flags, so
/// each query keeps the flags it was initialised with in a private word.
/// InstanceContributionToHitGroupIndex has no Metal getter either and is
/// refused (UNIMPLEMENTED) until a top-level side table carries it.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"

#include <cstring>

namespace vitreo::graphics::dxbc_spirv_detail {

using namespace dxbc_vitreo_ext;

std::set<u32> DxbcTranslator::scan_acceleration_structures() const {
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::set<u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == kOpDclAccelerationStructure) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) && declaration.type == Sm4OperandType::Resource) {
                declarations.insert(declaration.index0);
            }
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

u32 DxbcTranslator::rq_type_acceleration_structure() {
    if (m_rq_type_as == 0) {
        m_builder.emit_capability(SpvCapability::RayQueryKHR);
        m_builder.emit_extension("SPV_KHR_ray_query");
        m_builder.set_section(SpirvBuilder::Section::TypesGlobals);
        m_rq_type_as = m_builder.allocate_id();
        m_builder.emit(SpvOp::OpTypeAccelerationStructureKHR, {m_rq_type_as});
    }
    return m_rq_type_as;
}

bool DxbcTranslator::declare_acceleration_structures() {
    for (const u32 slot : scan_acceleration_structures()) {
        const u32 type = rq_type_acceleration_structure();
        const u32 pointer = m_builder.emit_type_pointer(SpvStorageClass::UniformConstant, type);
        const u32 var = declare_resource(DxbcRegisterClass::ShaderResource, slot, pointer,
                                         SpvStorageClass::UniformConstant,
                                         DescriptorKind::SampledImage,
                                         "t" + std::to_string(slot) + "_as");
        if (var == 0) return false;
        m_rq_as_vars[slot] = var;
        // D3D_SIT_RTACCELERATIONSTRUCTURE (d3dcommon.h).
        m_result.texture_bindings.push_back({"t" + std::to_string(slot), 12, slot, 1, 0, 0});
    }
    return true;
}

bool DxbcTranslator::translate_vx_ray_query(u32 control) {
    const auto op = static_cast<RayQueryOp>(control & 0xFFu);
    const u32 query = (control >> 8) & 0xFFu;
    const u32 aux = control >> 16;
    const u32 uint_zero = m_builder.emit_constant_int(m_type_uint, 0);
    const u32 uint_one = m_builder.emit_constant_int(m_type_uint, 1);
    // The query object and its flags word, made on first use.
    if (!m_rq_vars.contains(query)) {
        rq_type_acceleration_structure();
        if (m_rq_type_query == 0) {
            m_builder.set_section(SpirvBuilder::Section::TypesGlobals);
            m_rq_type_query = m_builder.allocate_id();
            m_builder.emit(SpvOp::OpTypeRayQueryKHR, {m_rq_type_query});
        }
        m_rq_vars[query] = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_rq_type_query),
            SpvStorageClass::Private);
        m_rq_flag_vars[query] = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint),
            SpvStorageClass::Private);
    }
    const u32 q = m_rq_vars[query];
    const auto scalar = [&](const Sm4Operand& operand, u32& out) {
        u32 bits = 0;
        if (!load_bits(operand, bits)) return false;
        out = m_builder.emit_composite_extract(m_type_uint, bits, {0u});
        return true;
    };
    const auto as_float = [&](u32 bits) { return m_builder.emit_bitcast(m_type_float, bits); };
    const auto store_bits = [&](const Sm4Operand& dst, u32 bits) {
        const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {bits, bits, bits, bits});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
    };
    const auto store_float = [&](const Sm4Operand& dst, u32 f) {
        return store_bits(dst, m_builder.emit_bitcast(m_type_uint, f));
    };
    const auto store_bool = [&](const Sm4Operand& dst, u32 b) {
        return store_bits(dst, m_builder.emit_select(m_type_uint, b,
                                                     m_builder.emit_constant_int(m_type_uint, ~0u),
                                                     uint_zero));
    };
    const auto value = [&](SpvOp o, u32 type, std::vector<u32> extra) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        const u32 id = m_builder.allocate_id();
        std::vector<u32> words = {type, id, q};
        words.insert(words.end(), extra.begin(), extra.end());
        m_builder.emit(o, words);
        return id;
    };
    const auto statement = [&](SpvOp o, std::vector<u32> extra) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        std::vector<u32> words = {q};
        words.insert(words.end(), extra.begin(), extra.end());
        m_builder.emit(o, words);
    };
    // SPIR-V's Intersection operand: candidate 0, committed 1.
    const auto which = [&](bool committed) { return committed ? uint_one : uint_zero; };

    switch (op) {
    case RayQueryOp::TraceRayInline: {
        Sm4Operand as_operand, s[10];
        if (!m_decoder.decode_operand(as_operand) || as_operand.type != Sm4OperandType::Resource) {
            return fail("VX_RAY_QUERY: TraceRayInline without an acceleration structure");
        }
        const auto found = m_rq_as_vars.find(as_operand.index0);
        if (found == m_rq_as_vars.end()) {
            return fail("VX_RAY_QUERY: t" + std::to_string(as_operand.index0) +
                        " is not an acceleration structure");
        }
        u32 v[10] = {};
        for (u32 k = 0; k < 10; ++k) {
            if (!m_decoder.decode_operand(s[k]) || !scalar(s[k], v[k])) {
                return fail("VX_RAY_QUERY: bad TraceRayInline operands");
            }
        }
        // An element of an array of acceleration structures (a descriptor
        // array, or a raytracing megakernel's local-root views): its index
        // as for any other resource array.
        u32 pointer = found->second;
        if (const ResourceRange* range = find_range(DxbcRegisterClass::ShaderResource, as_operand.index0);
            range != nullptr && range->is_array) {
            pointer = array_element_pointer(as_operand, *range);
            if (pointer == 0) return false;
        }
        const u32 as = m_builder.emit_load(rq_type_acceleration_structure(), pointer);
        // Raytracing.md (RayQuery::TraceRayInline): the template flags OR
        // the dynamic ones; the instance mask is its low 8 bits.
        const u32 flags = m_builder.emit_bitwise_or(m_type_uint, v[0],
                                                    m_builder.emit_constant_int(m_type_uint, aux));
        const u32 mask = m_builder.emit_bitwise_and(m_type_uint, v[1],
                                                    m_builder.emit_constant_int(m_type_uint, 0xFFu));
        const u32 origin = m_builder.emit_composite_construct(
            m_type_vec3, {as_float(v[2]), as_float(v[3]), as_float(v[4])});
        const u32 direction = m_builder.emit_composite_construct(
            m_type_vec3, {as_float(v[6]), as_float(v[7]), as_float(v[8])});
        m_builder.emit_store(m_rq_flag_vars[query], flags);
        statement(SpvOp::OpRayQueryInitializeKHR,
                  {as, flags, mask, origin, as_float(v[5]), direction, as_float(v[9])});
        return true;
    }
    case RayQueryOp::Abort:
        statement(SpvOp::OpRayQueryTerminateKHR, {});
        return true;
    case RayQueryOp::CommitNonOpaqueTriangleHit:
        statement(SpvOp::OpRayQueryConfirmIntersectionKHR, {});
        return true;
    case RayQueryOp::CommitProceduralPrimitiveHit: {
        Sm4Operand t;
        u32 bits = 0;
        if (!m_decoder.decode_operand(t) || !scalar(t, bits)) return fail("VX_RAY_QUERY: bad t");
        statement(SpvOp::OpRayQueryGenerateIntersectionKHR, {as_float(bits)});
        return true;
    }
    default:
        break;
    }

    Sm4Operand dst;
    if (!m_decoder.decode_operand(dst)) return fail("VX_RAY_QUERY: no destination");
    const u32 vec3_type = m_type_vec3;
    const auto component3 = [&](SpvOp o, bool has_which, bool committed) {
        const u32 v = has_which ? value(o, vec3_type, {which(committed)}) : value(o, vec3_type, {});
        return store_float(dst, m_builder.emit_composite_extract(m_type_float, v, {aux & 3u}));
    };
    switch (op) {
    case RayQueryOp::Proceed:
        return store_bool(dst, value(SpvOp::OpRayQueryProceedKHR, m_type_bool, {}));
    case RayQueryOp::CommittedStatus:
        // COMMITTED_NOTHING 0 / TRIANGLE_HIT 1 / PROCEDURAL_PRIMITIVE_HIT 2:
        // SPIR-V's None / Triangle / Generated.
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionTypeKHR, m_type_uint, {which(true)}));
    case RayQueryOp::CandidateType:
        // CANDIDATE_NON_OPAQUE_TRIANGLE 0 / PROCEDURAL_PRIMITIVE 1.
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionTypeKHR, m_type_uint, {which(false)}));
    case RayQueryOp::CandidateObjectToWorld3x4:
    case RayQueryOp::CandidateWorldToObject3x4:
    case RayQueryOp::CommittedObjectToWorld3x4:
    case RayQueryOp::CommittedWorldToObject3x4: {
        const bool committed = op == RayQueryOp::CommittedObjectToWorld3x4 ||
                               op == RayQueryOp::CommittedWorldToObject3x4;
        const bool to_world = op == RayQueryOp::CandidateObjectToWorld3x4 ||
                              op == RayQueryOp::CommittedObjectToWorld3x4;
        // SPIR-V: a 4-column, 3-row matrix; the 3x4 element (row, col) is
        // column col, component row.
        if (m_rq_type_mat43 == 0) {
            m_builder.set_section(SpirvBuilder::Section::TypesGlobals);
            m_rq_type_mat43 = m_builder.allocate_id();
            m_builder.emit(SpvOp::OpTypeMatrix, {m_rq_type_mat43, m_type_vec3, 4u});
        }
        const u32 mat = m_rq_type_mat43;
        const u32 m = value(to_world ? SpvOp::OpRayQueryGetIntersectionObjectToWorldKHR
                                     : SpvOp::OpRayQueryGetIntersectionWorldToObjectKHR,
                            mat, {which(committed)});
        const u32 row = aux & 3u, column = (aux >> 2) & 3u;
        return store_float(dst, m_builder.emit_composite_extract(m_type_float, m, {column, row}));
    }
    case RayQueryOp::CandidateProceduralPrimitiveNonOpaque:
        return store_bool(dst, m_builder.emit_logical_not(
                                   m_type_bool, value(SpvOp::OpRayQueryGetIntersectionCandidateAABBOpaqueKHR,
                                                      m_type_bool, {})));
    case RayQueryOp::CandidateTriangleFrontFace:
    case RayQueryOp::CommittedTriangleFrontFace:
        return store_bool(dst, value(SpvOp::OpRayQueryGetIntersectionFrontFaceKHR, m_type_bool,
                                     {which(op == RayQueryOp::CommittedTriangleFrontFace)}));
    case RayQueryOp::CandidateTriangleBarycentrics:
    case RayQueryOp::CommittedTriangleBarycentrics: {
        const u32 v = value(SpvOp::OpRayQueryGetIntersectionBarycentricsKHR, m_type_vec2,
                            {which(op == RayQueryOp::CommittedTriangleBarycentrics)});
        return store_float(dst, m_builder.emit_composite_extract(m_type_float, v, {aux & 1u}));
    }
    case RayQueryOp::RayFlags:
        return store_bits(dst, m_builder.emit_load(m_type_uint, m_rq_flag_vars[query]));
    case RayQueryOp::WorldRayOrigin:
        return component3(SpvOp::OpRayQueryGetWorldRayOriginKHR, false, false);
    case RayQueryOp::WorldRayDirection:
        return component3(SpvOp::OpRayQueryGetWorldRayDirectionKHR, false, false);
    case RayQueryOp::RayTMin:
        return store_float(dst, value(SpvOp::OpRayQueryGetRayTMinKHR, m_type_float, {}));
    case RayQueryOp::CandidateTriangleRayT:
    case RayQueryOp::CommittedRayT:
        return store_float(dst, value(SpvOp::OpRayQueryGetIntersectionTKHR, m_type_float,
                                      {which(op == RayQueryOp::CommittedRayT)}));
    case RayQueryOp::CandidateInstanceIndex:
    case RayQueryOp::CommittedInstanceIndex:
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionInstanceIdKHR, m_type_uint,
                                     {which(op == RayQueryOp::CommittedInstanceIndex)}));
    case RayQueryOp::CandidateInstanceID:
    case RayQueryOp::CommittedInstanceID:
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionInstanceCustomIndexKHR, m_type_uint,
                                     {which(op == RayQueryOp::CommittedInstanceID)}));
    case RayQueryOp::CandidateGeometryIndex:
    case RayQueryOp::CommittedGeometryIndex:
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionGeometryIndexKHR, m_type_uint,
                                     {which(op == RayQueryOp::CommittedGeometryIndex)}));
    case RayQueryOp::CandidatePrimitiveIndex:
    case RayQueryOp::CommittedPrimitiveIndex:
        return store_bits(dst, value(SpvOp::OpRayQueryGetIntersectionPrimitiveIndexKHR, m_type_uint,
                                     {which(op == RayQueryOp::CommittedPrimitiveIndex)}));
    case RayQueryOp::CandidateObjectRayOrigin:
    case RayQueryOp::CommittedObjectRayOrigin:
        return component3(SpvOp::OpRayQueryGetIntersectionObjectRayOriginKHR, true,
                          op == RayQueryOp::CommittedObjectRayOrigin);
    case RayQueryOp::CandidateObjectRayDirection:
    case RayQueryOp::CommittedObjectRayDirection:
        return component3(SpvOp::OpRayQueryGetIntersectionObjectRayDirectionKHR, true,
                          op == RayQueryOp::CommittedObjectRayDirection);
    default:
        return fail("VX_RAY_QUERY: unknown operation " + std::to_string(control & 0xFFu));
    }
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
