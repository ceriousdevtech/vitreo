// SPDX-License-Identifier: LGPL-2.1-only
// Inline ray tracing (RayQuery, DXIL.rst opcodes 178-215, shader model
// 6.5) for dxil_lower.cpp's Lowerer: each call becomes one Vitreo ray-query
// instruction (dxbc_vitreo_ext.hpp kOpRayQuery). Included inside
// dxil_lower.cpp after dxil_lower_ops.inc; not a standalone file.

bool Lowerer::emit_ray_query(u32 opcode, const DxilInstruction& inst) {
    using dxbc_vitreo_ext::RayQueryOp;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    if (opcode == 178) {
        // AllocateRayQuery(constRayFlags): the RayQuery<flags> object.
        u32 flags = 0;
        if (!const_u32(arg(1), flags)) return malformed("AllocateRayQuery without constant flags");
        if (ray_query_flags_.size() >= 256) return unimplemented("more than 256 RayQuery objects");
        ray_queries_[inst.result] = static_cast<u32>(ray_query_flags_.size());
        ray_query_flags_.push_back(flags);
        return true;
    }
    const auto found = ray_queries_.find(arg(1));
    if (found == ray_queries_.end()) {
        return unimplemented("a RayQuery handle not made by AllocateRayQuery (phi / select of queries)");
    }
    const u32 query = found->second;
    const u32 rq_op = opcode == 214 ? static_cast<u32>(RayQueryOp::CandidateInstanceContributionToHitGroupIndex)
                   : opcode == 215 ? static_cast<u32>(RayQueryOp::CommittedInstanceContributionToHitGroupIndex)
                                   : opcode - 179u;
    if (opcode == 214 || opcode == 215) {
        // InstanceContributionToHitGroupIndex: Metal has no getter; the top
        // level's Vitreo blob holds the instance descriptions, read through
        // its companion view (dxil_raytracing.hpp, space kDxilRtSpace).
        const auto as = ray_query_as_.find(query);
        if (as == ray_query_as_.end()) return unimplemented("a RayQuery contribution before TraceRayInline");
        const Handle& as_handle = as->second;
        const auto [companion, companion_lower] = rt_companion_of(*as_handle.res);
        if (companion == ~0u) {
            return unimplemented("RayQuery contribution on an unbounded array of acceleration structures");
        }
        const Slot instance = rt_query(query, opcode == 214
                                                  ? dxbc_vitreo_ext::RayQueryOp::CandidateInstanceIndex
                                                  : dxbc_vitreo_ext::RayQueryOp::CommittedInstanceIndex);
        // The element's companion: the same index into the companion array.
        const Index at{as_handle.reg.relative, companion_lower + (as_handle.reg.immediate - as_handle.res->lower),
                       as_handle.reg.reg, as_handle.reg.comp};
        const Slot count = rt_word_at(companion, at, imm(16));
        const Slot a = fresh_scalar();
        const Operand ad = dst_temp(a.reg, 1u << a.comp), as_ = src_temp_scalar(a.reg, a.comp);
        const Operand cs = src_temp_scalar(count.reg, count.comp), is = src_temp_scalar(instance.reg, instance.comp);
        const Operand eight = imm(8), base = imm(256 + 52), sixty_four = imm(64);
        body_.inst(op(Sm4Opcode::IMAD), {&ad, &cs, &eight, &base});
        body_.inst(op(Sm4Opcode::IMAD), {&ad, &is, &sixty_four, &as_});
        const Slot word = rt_word_at(companion, at, as_);
        const Operand d = dst(inst.result), ws = src_temp_scalar(word.reg, word.comp), low24 = imm(0xFFFFFFu);
        body_.inst(op(Sm4Opcode::AND), {&d, &ws, &low24});
        return true;
    }
    u32 aux = 0;
    const auto control = [&]() { return rq_op | (query << 8) | (aux << 16); };
    switch (opcode) {
    case 179: {  // TraceRayInline(query, accelerationStructure, rayFlags, mask, origin, tMin, direction, tMax)
        Handle h;
        if (!handle_of(arg(2), h)) return false;
        if (h.res == nullptr || h.res->cls != ResourceClass::SRV ||
            h.res->shape != ResourceShape::RTAccelerationStructure) {
            return malformed("TraceRayInline on a resource that is not an acceleration structure");
        }
        aux = ray_query_flags_[query] & 0xFFFFu;
        ray_query_as_[query] = h;
        const Operand t = resource_src(h);
        Operand s[10];
        for (usize k = 0; k < 10; ++k) s[k] = src(arg(3 + k));
        emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, control(),
                 {&t, &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7], &s[8], &s[9]});
        return true;
    }
    case 181:  // Abort
    case 182:  // CommitNonOpaqueTriangleHit
        emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, control(), {});
        return true;
    case 183: {  // CommitProceduralPrimitiveHit(query, t)
        const Operand t = src(arg(2));
        emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, control(), {&t});
        return true;
    }
    case 186: case 187: case 188: case 189: {  // *ToWorld3x4 / *ToObject3x4(query, row, col)
        u32 row = 0, column = 0;
        if (!const_u32(arg(2), row) || !const_u32(arg(3), column) || row > 2 || column > 3) {
            return unimplemented("a RayQuery matrix element not named by constants");
        }
        aux = row | (column << 2);
        break;
    }
    case 193: case 194:  // barycentrics(query, component)
    case 196: case 197: case 205: case 206: case 211: case 212: {  // float3 getters(query, component)
        u32 component = 0;
        const u32 limit = opcode == 193 || opcode == 194 ? 1u : 2u;
        if (!const_u32(arg(2), component) || component > limit) {
            return unimplemented("a RayQuery vector component not named by a constant");
        }
        aux = component;
        break;
    }
    default:
        break;
    }
    const Operand d = dst(inst.result);
    emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, control(), {&d});
    return true;
}

std::pair<u32, u32> Lowerer::rt_companion_of(const Resource& as) {
    // Every element of a bounded acceleration-structure range has its blob
    // view at the same index of a companion array in space kDxilRtSpace
    // (t(kRtCompanionBase + ...), in declaration order; the frontend binds
    // each from its element's VA, CompiledShader::rt_reserved). Made with
    // the resources (add_rq_companions); a megakernel has its own.
    const std::map<u32, u32>& map = rt_ != nullptr ? *rt_->companions : rq_companions_;
    const auto found = map.find(as.id);
    if (found == map.end()) return {~0u, 0u};
    return {found->second, rt_companion_regs_.at(found->second)};
}

Slot Lowerer::rt_word_at(u32 range_id, const Index& at, const Operand& byte_offset) {
    const Slot s = fresh_scalar();
    const Operand d = dst_temp(s.reg, 1u << s.comp);
    const Operand res = resource(Sm4OperandType::Resource, range_id, at, at.relative, 0x00);
    body_.inst(op(Sm4Opcode::LD_RAW), {&d, &byte_offset, &res});
    return s;
}

void Lowerer::add_rq_companions() {
    u32 next = 0;
    std::vector<const Resource*> as_list;
    for (const Resource& r : resources_) {
        if (r.cls == ResourceClass::SRV) next = std::max(next, r.id + 1);
        if (r.cls == ResourceClass::SRV && r.shape == ResourceShape::RTAccelerationStructure &&
            r.upper != kDxbcUnboundedRange) {
            as_list.push_back(&r);
        }
    }
    std::vector<std::pair<u32, Resource>> made;
    u32 reg = kRtCompanionBase;
    for (const Resource* as : as_list) {
        Resource c;
        c.cls = ResourceClass::SRV;
        c.shape = ResourceShape::RawBuffer;
        c.space = kDxilRtSpace;
        c.lower = reg;
        c.upper = reg + (as->upper - as->lower);
        reg = c.upper + 1;
        c.name = "vitreo_rt_blob";
        c.id = next++;
        made.emplace_back(as->id, c);
    }
    for (const auto& [as_id, c] : made) {
        resources_.push_back(c);
        rq_companions_[as_id] = c.id;
        rt_companion_regs_[c.id] = c.lower;
    }
}
