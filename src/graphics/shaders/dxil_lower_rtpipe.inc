// SPDX-License-Identifier: LGPL-2.1-only
// Raytracing pipelines as one compute megakernel (dxil_raytracing.hpp):
// the Lowerer members that lower DXR intrinsics in a function copy, and the
// builder that plans the copies and links them. Included inside
// dxil_lower.cpp's anonymous namespace after the Lowerer class.
//
// Sources: DirectX-Specs Raytracing.md ("TraceRay control flow", "Hit
// groups", "Shader tables", "Addressing calculations within shader
// tables", "System value intrinsics", ReportHit / IgnoreHit /
// AcceptHitAndEndSearch / CallShader); DXIL.rst opcodes 141-161, 213.

namespace rt {
using dxbc_vitreo_ext::RayQueryOp;
constexpr u32 kHitKindFront = 0xFE;  // HIT_KIND_TRIANGLE_FRONT_FACE
constexpr u32 kHitKindBack = 0xFF;   // HIT_KIND_TRIANGLE_BACK_FACE
constexpr u32 kRaySkipClosestHit = 0x8;  // RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
/// Byte of the identifier index inside a 32-byte shader identifier.
constexpr u32 kIdentifierIndexByte = 8;
}  // namespace rt

Slot Lowerer::rt_query(u32 query, dxbc_vitreo_ext::RayQueryOp o, u32 aux) {
    const Slot s = fresh_scalar();
    const Operand d = dst_temp(s.reg, 1u << s.comp);
    emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery,
             static_cast<u32>(o) | (query << 8) | (aux << 16), {&d});
    return s;
}

void Lowerer::rt_query_op(u32 query, dxbc_vitreo_ext::RayQueryOp o,
                          std::initializer_list<const Operand*> ops) {
    emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, static_cast<u32>(o) | (query << 8), ops);
}

Slot Lowerer::rt_word(u32 range_id, u32 reg, const Operand& byte_offset) {
    const Slot s = fresh_scalar();
    const Operand d = dst_temp(s.reg, 1u << s.comp);
    const Operand res = resource(Sm4OperandType::Resource, range_id, Index{false, reg, 0, 0}, false, 0x00);
    body_.inst(op(Sm4Opcode::LD_RAW), {&d, &byte_offset, &res});
    return s;
}

Operand Lowerer::rt_const(u32 word) {
    return cbuffer(rt_->const_id, Index{false, 0, 0, 0}, Index{false, word / 4u, 0, 0}, false,
                   swizzle_of(word % 4u));
}

void Lowerer::rt_copy(const Pointer& p, u32 array, u32 count, bool into_array) {
    for (u32 i = 0; i < count; ++i) {
        const Index at{p.has_reg, p.constant + i, p.reg.reg, p.reg.comp};
        if (into_array) {
            const Operand d = indexable(array, Index{false, i, 0, 0}, 0x1, true);
            const Operand s = indexable(p.object, at, 0x00, false);
            body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        } else {
            const Operand d = indexable(p.object, at, 0x1, true);
            const Operand s = indexable(array, Index{false, i, 0, 0}, 0x00, false);
            body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        }
    }
}

u32 Lowerer::rt_pointee_words(u32 pointer_value) {
    const u32 t = m_.type_of(fn_, pointer_value);
    if (t >= m_.types.size() || type(t).kind != DxilTypeKind::Pointer) return 0;
    return scalar_count(m_, type(t).element);
}

void Lowerer::rt_call_switch(const Slot& id, const std::vector<std::pair<u32, u32>>& cases) {
    // switch (id) { case index: call label; break; } -- an index no case
    // names (a NULL or foreign identifier) runs nothing.
    if (cases.empty()) return;
    const Operand sel = src_temp_select(id.reg, id.comp);
    body_.inst(op(Sm4Opcode::SWITCH), {&sel});
    for (const auto& [index, label] : cases) {
        const Operand c = imm1(index);
        body_.inst(op(Sm4Opcode::CASE), {&c});
        const Operand l = label_operand(label);
        body_.inst(op(Sm4Opcode::CALL), {&l});
        body_.inst(op(Sm4Opcode::BREAK), {});
    }
    body_.inst(op(Sm4Opcode::DEFAULT), {});
    body_.inst(op(Sm4Opcode::BREAK), {});
    body_.inst(op(Sm4Opcode::ENDSWITCH), {});
}

Slot Lowerer::rt_record_id(u32 table, const Slot& index, u32 stride_word, u32 count_word, const Slot& rec) {
    // rec = index * stride; id = the record's identifier index, ~0 when the
    // index is past the table (Raytracing.md: out-of-bounds indexing is
    // undefined; it runs nothing here).
    const Operand nul = null_operand();
    const Operand rd = dst_temp(rec.reg, 1u << rec.comp), is = src_temp_scalar(index.reg, index.comp);
    const Operand stride = rt_const(stride_word);
    body_.inst(op(Sm4Opcode::IMUL), {&nul, &rd, &is, &stride});
    const Slot id = fresh_scalar();
    const Operand idd = dst_temp(id.reg, 1u << id.comp);
    const Operand none = imm(~0u);
    body_.inst(op(Sm4Opcode::MOV), {&idd, &none});
    const Slot ok = fresh_scalar();
    const Operand okd = dst_temp(ok.reg, 1u << ok.comp), count = rt_const(count_word);
    body_.inst(op(Sm4Opcode::ULT), {&okd, &is, &count});
    const Operand oks = src_temp_select(ok.reg, ok.comp);
    body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&oks});
    const Slot at = fresh_scalar();
    const Operand atd = dst_temp(at.reg, 1u << at.comp), rs = src_temp_scalar(rec.reg, rec.comp),
                  eight = imm(rt::kIdentifierIndexByte);
    body_.inst(op(Sm4Opcode::IADD), {&atd, &rs, &eight});
    const Slot w = rt_word(rt_->table_ids[table], table, src_temp_scalar(at.reg, at.comp));
    const Operand ws = src_temp_scalar(w.reg, w.comp);
    body_.inst(op(Sm4Opcode::MOV), {&idd, &ws});
    body_.inst(op(Sm4Opcode::ENDIF), {});
    return id;
}

Slot Lowerer::rt_hit_group(u32 query, bool committed, const Handle& as, const Operand& contribution,
                           const Operand& multiplier) {
    using rt::RayQueryOp;
    // Raytracing.md, "Hit group table indexing": RayContribution +
    // MultiplierForGeometryContribution * GeometryIndex +
    // InstanceContributionToHitGroupIndex.
    const Slot instance = rt_query(query, committed ? RayQueryOp::CommittedInstanceIndex
                                                    : RayQueryOp::CandidateInstanceIndex);
    const Slot geometry = rt_query(query, committed ? RayQueryOp::CommittedGeometryIndex
                                                    : RayQueryOp::CandidateGeometryIndex);
    // The instance's contribution, from the top level's blob (the
    // companion view): count at byte 16, descriptions at 256 + 8 * count,
    // InstanceContributionToHitGroupIndex the low 24 bits of byte 52.
    const auto [comp_id, comp_lower] = rt_companion_of(*as.res);
    const Index at{as.reg.relative, comp_lower + (as.reg.immediate - as.res->lower), as.reg.reg, as.reg.comp};
    const Slot count = rt_word_at(comp_id, at, imm(16));
    const Slot a = fresh_scalar(), b = fresh_scalar();
    const Operand ad = dst_temp(a.reg, 1u << a.comp), as_ = src_temp_scalar(a.reg, a.comp);
    const Operand bd = dst_temp(b.reg, 1u << b.comp), bs = src_temp_scalar(b.reg, b.comp);
    const Operand cs = src_temp_scalar(count.reg, count.comp), is = src_temp_scalar(instance.reg, instance.comp);
    const Operand eight = imm(8), base = imm(256 + 52), sixty_four = imm(64);
    body_.inst(op(Sm4Opcode::IMAD), {&ad, &cs, &eight, &base});
    body_.inst(op(Sm4Opcode::IMAD), {&bd, &is, &sixty_four, &as_});
    const Slot word = rt_word_at(comp_id, at, bs);
    const Operand wd = dst_temp(word.reg, 1u << word.comp), ws = src_temp_scalar(word.reg, word.comp);
    const Operand low24 = imm(0xFFFFFFu);
    body_.inst(op(Sm4Opcode::AND), {&wd, &ws, &low24});
    const Slot hg = fresh_scalar();
    const Operand hd = dst_temp(hg.reg, 1u << hg.comp), hs = src_temp_scalar(hg.reg, hg.comp),
                  gs = src_temp_scalar(geometry.reg, geometry.comp);
    body_.inst(op(Sm4Opcode::IMAD), {&hd, &multiplier, &gs, &contribution});
    body_.inst(op(Sm4Opcode::IADD), {&hd, &hs, &ws});
    return hg;
}

bool Lowerer::emit_trace_ray(const DxilInstruction& inst) {
    using rt::RayQueryOp;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    RtFunctionContext& c = *rt_;
    const u32 q = c.level + 1;
    if (q > c.max_trace) {
        // Past MaxTraceRecursionDepth: undefined (Raytracing.md, "Ray
        // recursion limit"); the payload is left as it is.
        return true;
    }
    const RtLevel& Q = (*c.levels)[q];
    Handle as;
    if (!handle_of(arg(1), as)) return false;
    if (as.res == nullptr || as.res->shape != ResourceShape::RTAccelerationStructure) {
        return malformed("TraceRay on a resource that is not an acceleration structure");
    }
    if (rt_companion_of(*as.res).first == ~0u) {
        return unimplemented("TraceRay on an unbounded array of acceleration structures");
    }
    Pointer payload;
    if (!resolve_pointer(arg(15), payload)) return false;
    const u32 words = std::min(rt_pointee_words(arg(15)), c.payload_words);
    rt_copy(payload, Q.payload, words, true);
    {
        const Operand d = dst_temp(Q.frame, 1u << 2), t = src(arg(14));
        body_.inst(op(Sm4Opcode::MOV), {&d, &t});
    }
    const Slot flags = fresh_scalar();
    const Operand fd = dst_temp(flags.reg, 1u << flags.comp), fs = src_temp_scalar(flags.reg, flags.comp);
    {
        const Operand f = src(arg(2)), pf = imm(c.pipeline_flags);
        body_.inst(op(Sm4Opcode::OR), {&fd, &f, &pf});
        // Raytracing.md (D3D12_RAYTRACING_PIPELINE_FLAGS): the pipeline's
        // skip flags act on the trace but do not show up in RayFlags(),
        // which the level's frame2.z answers.
        const Operand keep = dst_temp(Q.frame2, 1u << 2);
        body_.inst(op(Sm4Opcode::MOV), {&keep, &f});
    }
    {
        const Operand t = resource_src(as);
        Operand s[9];
        for (usize k = 0; k < 4; ++k) s[k] = src(arg(7 + k));      // origin, tMin
        for (usize k = 0; k < 4; ++k) s[4 + k] = src(arg(11 + k)); // direction, tMax
        const Operand mask = src(arg(3));
        emit_ext(body_, dxbc_vitreo_ext::kOpRayQuery, static_cast<u32>(RayQueryOp::TraceRayInline) | (q << 8),
                 {&t, &fs, &mask, &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7]});
    }
    const Operand contribution = src(arg(4)), multiplier = src(arg(5));
    const Operand rec_x = dst_temp(Q.frame, 1u), verdict_d = dst_temp(Q.frame, 1u << 1);
    const Operand zero = imm(0);
    // ---- traversal ----
    body_.inst(op(Sm4Opcode::LOOP), {});
    {
        const Slot more = rt_query(q, RayQueryOp::Proceed);
        const Operand ms = src_temp_select(more.reg, more.comp);
        body_.inst(op(Sm4Opcode::BREAKC), {&ms});
        const Slot hg = rt_hit_group(q, false, as, contribution, multiplier);
        const Slot rec = fresh_scalar();
        const Slot id = rt_record_id(2, hg, kRtConstHitStride, kRtConstHitCount, rec);
        const Operand rs = src_temp_scalar(rec.reg, rec.comp);
        body_.inst(op(Sm4Opcode::MOV), {&rec_x, &rs});
        body_.inst(op(Sm4Opcode::MOV), {&verdict_d, &zero});
        const Slot type = rt_query(q, RayQueryOp::CandidateType);
        const Operand ts = src_temp_select(type.reg, type.comp);
        body_.inst(op(Sm4Opcode::IF), {&ts});  // 0: a non-opaque triangle
        {
            for (u32 k = 0; k < 2; ++k) {
                const Slot b = rt_query(q, RayQueryOp::CandidateTriangleBarycentrics, k);
                const Operand d = indexable(Q.attr, Index{false, k, 0, 0}, 0x1, true), bs = src_temp_scalar(b.reg, b.comp);
                body_.inst(op(Sm4Opcode::MOV), {&d, &bs});
            }
            std::vector<std::pair<u32, u32>> cases;
            for (const RtHitGroup& g : *c.hit_groups) {
                if (g.procedural || !g.any) continue;
                const u32 label = c.label_of(g.index, RtRole::AnyTri, q);
                if (label != ~0u) cases.emplace_back(g.index, label);
            }
            rt_call_switch(id, cases);
            const Slot ignored = fresh_scalar();
            const Operand igd = dst_temp(ignored.reg, 1u << ignored.comp), one = imm(1),
                          vs = src_temp_scalar(Q.frame, 1);
            body_.inst(op(Sm4Opcode::AND), {&igd, &vs, &one});
            const Operand igs = src_temp_select(ignored.reg, ignored.comp);
            body_.inst(op(Sm4Opcode::IF), {&igs});
            rt_query_op(q, RayQueryOp::CommitNonOpaqueTriangleHit, {});
            body_.inst(op(Sm4Opcode::ENDIF), {});
        }
        body_.inst(op(Sm4Opcode::ELSE), {});
        {
            std::vector<std::pair<u32, u32>> cases;
            for (const RtHitGroup& g : *c.hit_groups) {
                if (!g.procedural || !g.intersection) continue;
                const u32 label = c.label_of(g.index, RtRole::Intersection, q);
                if (label != ~0u) cases.emplace_back(g.index, label);
            }
            rt_call_switch(id, cases);
        }
        body_.inst(op(Sm4Opcode::ENDIF), {});
        // AcceptHitAndEndSearch: the query stops.
        const Slot end = fresh_scalar();
        const Operand ed = dst_temp(end.reg, 1u << end.comp), two = imm(2), vs = src_temp_scalar(Q.frame, 1);
        body_.inst(op(Sm4Opcode::AND), {&ed, &vs, &two});
        const Operand es = src_temp_select(end.reg, end.comp);
        body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&es});
        rt_query_op(q, RayQueryOp::Abort, {});
        body_.inst(op(Sm4Opcode::ENDIF), {});
    }
    body_.inst(op(Sm4Opcode::ENDLOOP), {});
    // ---- closest hit or miss ----
    const Slot status = rt_query(q, RayQueryOp::CommittedStatus);
    const Slot run_closest = fresh_scalar();
    {
        const Operand d = dst_temp(run_closest.reg, 1u << run_closest.comp), skip = imm(rt::kRaySkipClosestHit);
        body_.inst(op(Sm4Opcode::AND), {&d, &fs, &skip});
    }
    const Operand ss = src_temp_select(status.reg, status.comp);
    body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&ss});
    {
        const Operand rcs = src_temp_select(run_closest.reg, run_closest.comp);
        body_.inst(op(Sm4Opcode::IF), {&rcs});
        const Slot hg = rt_hit_group(q, true, as, contribution, multiplier);
        const Slot rec = fresh_scalar();
        const Slot id = rt_record_id(2, hg, kRtConstHitStride, kRtConstHitCount, rec);
        const Operand rs = src_temp_scalar(rec.reg, rec.comp);
        body_.inst(op(Sm4Opcode::MOV), {&rec_x, &rs});
        for (u32 k = 0; k < 2; ++k) {
            const Slot b = rt_query(q, RayQueryOp::CommittedTriangleBarycentrics, k);
            const Operand d = indexable(Q.attr, Index{false, k, 0, 0}, 0x1, true), bs = src_temp_scalar(b.reg, b.comp);
            body_.inst(op(Sm4Opcode::MOV), {&d, &bs});
        }
        std::vector<std::pair<u32, u32>> cases;
        for (const RtHitGroup& g : *c.hit_groups) {
            if (!g.closest) continue;
            const u32 label = c.label_of(g.index, g.procedural ? RtRole::ClosestProc : RtRole::ClosestTri, q);
            if (label != ~0u) cases.emplace_back(g.index, label);
        }
        rt_call_switch(id, cases);
        body_.inst(op(Sm4Opcode::ENDIF), {});
    }
    body_.inst(op(Sm4Opcode::ELSE), {});
    {
        const Slot miss = fresh_scalar();
        const Operand md = dst_temp(miss.reg, 1u << miss.comp), mi = src(arg(6));
        body_.inst(op(Sm4Opcode::MOV), {&md, &mi});
        const Slot rec = fresh_scalar();
        const Slot id = rt_record_id(1, miss, kRtConstMissStride, kRtConstMissCount, rec);
        const Operand rs = src_temp_scalar(rec.reg, rec.comp);
        body_.inst(op(Sm4Opcode::MOV), {&rec_x, &rs});
        std::vector<std::pair<u32, u32>> cases;
        for (const u32 index : c.miss_indices) {
            const u32 label = c.label_of(index, RtRole::Miss, q);
            if (label != ~0u) cases.emplace_back(index, label);
        }
        rt_call_switch(id, cases);
    }
    body_.inst(op(Sm4Opcode::ENDIF), {});
    rt_copy(payload, Q.payload, words, false);
    return true;
}

bool Lowerer::emit_report_hit(const DxilInstruction& inst) {
    using rt::RayQueryOp;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    RtFunctionContext& c = *rt_;
    if (c.role != RtRole::Intersection) return malformed("ReportHit outside an intersection shader");
    const u32 k = c.level;
    const RtLevel& L = (*c.levels)[k];
    const Operand d = dst(inst.result), zero = imm(0), ones = imm(~0u);
    body_.inst(op(Sm4Opcode::MOV), {&d, &zero});
    // Raytracing.md (ReportHit): a THit outside [RayTMin, RayTCurrent] is
    // ignored and returns false.
    const Slot tmin = rt_query(k, RayQueryOp::RayTMin);
    const Slot tcur = rt_query(k, RayQueryOp::CommittedRayT);
    const Slot in = fresh_scalar(), in2 = fresh_scalar();
    const Operand ind = dst_temp(in.reg, 1u << in.comp), ins = src_temp_scalar(in.reg, in.comp);
    const Operand in2d = dst_temp(in2.reg, 1u << in2.comp), in2s = src_temp_scalar(in2.reg, in2.comp);
    const Operand t = src(arg(1)), mins = src_temp_scalar(tmin.reg, tmin.comp), curs = src_temp_scalar(tcur.reg, tcur.comp);
    body_.inst(op(Sm4Opcode::GE), {&ind, &t, &mins});
    body_.inst(op(Sm4Opcode::GE), {&in2d, &curs, &t});
    body_.inst(op(Sm4Opcode::AND), {&ind, &ins, &in2s});
    const Operand insel = src_temp_select(in.reg, in.comp);
    body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&insel});
    {
        Pointer attr;
        if (!resolve_pointer(arg(3), attr)) return false;
        const u32 words = std::min(rt_pointee_words(arg(3)), c.attr_words);
        rt_copy(attr, L.attr, words, true);
        const Operand fw = dst_temp(L.frame, 1u << 3), kind = src(arg(2)), kd = dst_temp(L.frame2, 1u),
                      vd = dst_temp(L.frame, 1u << 1);
        body_.inst(op(Sm4Opcode::MOV), {&fw, &t});
        body_.inst(op(Sm4Opcode::MOV), {&kd, &kind});
        body_.inst(op(Sm4Opcode::MOV), {&vd, &zero});
        const RtHitGroup& g = (*c.hit_groups)[c.hit_group];
        const u32 any = g.any ? c.label_of(g.index, RtRole::AnyProc, k) : ~0u;
        if (any != ~0u) {
            // An opaque procedural candidate runs no any-hit shader.
            const Slot nonopaque = rt_query(k, RayQueryOp::CandidateProceduralPrimitiveNonOpaque);
            const Operand ns = src_temp_select(nonopaque.reg, nonopaque.comp);
            body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&ns});
            const Operand l = label_operand(any);
            body_.inst(op(Sm4Opcode::CALL), {&l});
            body_.inst(op(Sm4Opcode::ENDIF), {});
        }
        const Slot ig = fresh_scalar();
        const Operand igd = dst_temp(ig.reg, 1u << ig.comp), one = imm(1), vs = src_temp_scalar(L.frame, 1);
        body_.inst(op(Sm4Opcode::AND), {&igd, &vs, &one});
        const Operand igs = src_temp_select(ig.reg, ig.comp);
        body_.inst(op(Sm4Opcode::IF), {&igs});
        {
            rt_query_op(k, RayQueryOp::CommitProceduralPrimitiveHit, {&t});
            for (u32 e = 0; e < c.attr_words; ++e) {
                const Operand to = indexable(L.attrc, Index{false, e, 0, 0}, 0x1, true);
                const Operand from = indexable(L.attr, Index{false, e, 0, 0}, 0x00, false);
                body_.inst(op(Sm4Opcode::MOV), {&to, &from});
            }
            const Operand kc = dst_temp(L.frame2, 1u << 1);
            body_.inst(op(Sm4Opcode::MOV), {&kc, &kind});
            body_.inst(op(Sm4Opcode::MOV), {&d, &ones});
        }
        body_.inst(op(Sm4Opcode::ENDIF), {});
        // AcceptHitAndEndSearch ends the intersection shader too.
        const Slot end = fresh_scalar();
        const Operand ed = dst_temp(end.reg, 1u << end.comp), two = imm(2);
        body_.inst(op(Sm4Opcode::AND), {&ed, &vs, &two});
        const Operand es = src_temp_select(end.reg, end.comp);
        body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&es});
        rt_query_op(k, RayQueryOp::Abort, {});
        body_.inst(op(Sm4Opcode::RET), {});
        body_.inst(op(Sm4Opcode::ENDIF), {});
    }
    body_.inst(op(Sm4Opcode::ENDIF), {});
    return true;
}

bool Lowerer::emit_call_shader(const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    RtFunctionContext& c = *rt_;
    const u32 q = c.level + 1;
    if (q > c.max_level) return true;  // past the pipeline's stack: undefined
    const RtLevel& Q = (*c.levels)[q];
    Pointer param;
    if (!resolve_pointer(arg(2), param)) return false;
    const u32 words = std::min(rt_pointee_words(arg(2)), c.call_words);
    rt_copy(param, Q.call, words, true);
    const Slot index = fresh_scalar();
    const Operand id_d = dst_temp(index.reg, 1u << index.comp), is = src(arg(1));
    body_.inst(op(Sm4Opcode::MOV), {&id_d, &is});
    const Slot rec = fresh_scalar();
    const Slot id = rt_record_id(3, index, kRtConstCallableStride, kRtConstCallableCount, rec);
    const Operand rx = dst_temp(Q.frame, 1u), rs = src_temp_scalar(rec.reg, rec.comp);
    body_.inst(op(Sm4Opcode::MOV), {&rx, &rs});
    std::vector<std::pair<u32, u32>> cases;
    for (const u32 i : c.callable_indices) {
        const u32 label = c.label_of(i, RtRole::Callable, q);
        if (label != ~0u) cases.emplace_back(i, label);
    }
    rt_call_switch(id, cases);
    rt_copy(param, Q.call, words, false);
    return true;
}

bool Lowerer::emit_rt_op(u32 opcode, const DxilInstruction& inst, bool& handled) {
    using rt::RayQueryOp;
    handled = true;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    RtFunctionContext& c = *rt_;
    const u32 k = c.level;
    const RtLevel& L = (*c.levels)[k];
    const bool candidate = c.role == RtRole::AnyTri || c.role == RtRole::AnyProc || c.role == RtRole::Intersection;
    const auto getter = [&](RayQueryOp cand, RayQueryOp comm, u32 aux = 0) {
        const Slot s = rt_query(k, candidate ? cand : comm, aux);
        const Operand d = dst(inst.result), v = src_temp_scalar(s.reg, s.comp);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    };
    const auto component = [&](u32 limit, u32& out) {
        return const_u32(arg(1), out) && out <= limit;
    };
    u32 comp = 0;
    switch (opcode) {
    case 145: {  // DispatchRaysIndex(col)
        if (!component(2, comp)) return unimplemented("a dynamic DispatchRaysIndex component");
        system_inputs_.insert(static_cast<u32>(Sm4OperandType::InputThreadId));
        const Operand d = dst(inst.result), s = special_src(Sm4OperandType::InputThreadId, swizzle_of(comp));
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return true;
    }
    case 146: {  // DispatchRaysDimensions(col)
        if (!component(2, comp)) return unimplemented("a dynamic DispatchRaysDimensions component");
        const Operand d = dst(inst.result), s = rt_const(kRtConstWidth + comp);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return true;
    }
    case 141: return getter(RayQueryOp::CandidateInstanceID, RayQueryOp::CommittedInstanceID);
    case 142: return getter(RayQueryOp::CandidateInstanceIndex, RayQueryOp::CommittedInstanceIndex);
    case 161: return getter(RayQueryOp::CandidatePrimitiveIndex, RayQueryOp::CommittedPrimitiveIndex);
    case 213: return getter(RayQueryOp::CandidateGeometryIndex, RayQueryOp::CommittedGeometryIndex);
    case 143: {  // HitKind
        const Operand d = dst(inst.result);
        if (c.role == RtRole::AnyProc || c.role == RtRole::ClosestProc) {
            const Operand s = src_temp_scalar(L.frame2, c.role == RtRole::AnyProc ? 0u : 1u);
            body_.inst(op(Sm4Opcode::MOV), {&d, &s});
            return true;
        }
        const Slot front = rt_query(k, c.role == RtRole::AnyTri ? RayQueryOp::CandidateTriangleFrontFace
                                                                 : RayQueryOp::CommittedTriangleFrontFace);
        const Operand fs = src_temp_scalar(front.reg, front.comp), f = imm(rt::kHitKindFront), b = imm(rt::kHitKindBack);
        body_.inst(op(Sm4Opcode::MOVC), {&d, &fs, &f, &b});
        return true;
    }
    case 144: {  // RayFlags(): the TraceRay's own flags (emit_trace_ray)
        const Operand d = dst(inst.result), v = src_temp_scalar(L.frame2, 2);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case 147:
        if (!component(2, comp)) return unimplemented("a dynamic WorldRayOrigin component");
        return getter(RayQueryOp::WorldRayOrigin, RayQueryOp::WorldRayOrigin, comp);
    case 148:
        if (!component(2, comp)) return unimplemented("a dynamic WorldRayDirection component");
        return getter(RayQueryOp::WorldRayDirection, RayQueryOp::WorldRayDirection, comp);
    case 149:
        if (!component(2, comp)) return unimplemented("a dynamic ObjectRayOrigin component");
        return getter(RayQueryOp::CandidateObjectRayOrigin, RayQueryOp::CommittedObjectRayOrigin, comp);
    case 150:
        if (!component(2, comp)) return unimplemented("a dynamic ObjectRayDirection component");
        return getter(RayQueryOp::CandidateObjectRayDirection, RayQueryOp::CommittedObjectRayDirection, comp);
    case 151:    // ObjectToWorld(row, col)
    case 152: {  // WorldToObject(row, col)
        u32 row = 0, col = 0;
        if (!const_u32(arg(1), row) || !const_u32(arg(2), col) || row > 2 || col > 3) {
            return unimplemented("a transform element not named by constants");
        }
        return opcode == 151
                   ? getter(RayQueryOp::CandidateObjectToWorld3x4, RayQueryOp::CommittedObjectToWorld3x4,
                            row | (col << 2))
                   : getter(RayQueryOp::CandidateWorldToObject3x4, RayQueryOp::CommittedWorldToObject3x4,
                            row | (col << 2));
    }
    case 153: return getter(RayQueryOp::RayTMin, RayQueryOp::RayTMin);
    case 154: {  // RayTCurrent
        const Operand d = dst(inst.result);
        if (c.role == RtRole::Miss || c.role == RtRole::AnyProc) {
            const Operand s = src_temp_scalar(L.frame, c.role == RtRole::Miss ? 2u : 3u);
            body_.inst(op(Sm4Opcode::MOV), {&d, &s});
            return true;
        }
        if (c.role == RtRole::AnyTri) return getter(RayQueryOp::CandidateTriangleRayT, RayQueryOp::CandidateTriangleRayT);
        const Slot s = rt_query(k, RayQueryOp::CommittedRayT);
        const Operand v = src_temp_scalar(s.reg, s.comp);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case 155:    // IgnoreHit
    case 156: {  // AcceptHitAndEndSearch
        if (c.role != RtRole::AnyTri && c.role != RtRole::AnyProc) {
            return malformed("IgnoreHit / AcceptHitAndEndSearch outside an any-hit shader");
        }
        const Operand d = dst_temp(L.frame, 1u << 1), v = imm(opcode == 155 ? 1u : 2u);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        body_.inst(op(Sm4Opcode::RET), {});
        return true;
    }
    case 157: return emit_trace_ray(inst);
    case 158: return emit_report_hit(inst);
    case 159: return emit_call_shader(inst);
    case 160: {  // CreateHandleForLib(resource): the resource a load named
        const auto found = rt_lib_handles_.find(arg(1));
        if (found == rt_lib_handles_.end()) return unimplemented("CreateHandleForLib of a resource not loaded from its global");
        Handle h = found->second;
        if (!rt_local_handle(h)) return false;
        handles_[inst.result] = h;
        return true;
    }
    case 59: {  // CBufferLoadLegacy of a local root-signature constant buffer
        Handle h;
        if (!handle_of(arg(1), h) || h.res == nullptr || !h.res->rt_local) {
            handled = false;
            return true;
        }
        const u32 r = vec(inst.result);
        const Operand d4 = dst_temp(r, 0xF), zero = imm(0);
        const RtLocal* local = nullptr;
        for (const RtLocal& l : c.local) {
            if (l.space == h.res->space && l.reg == h.res->lower) local = &l;
        }
        if (local == nullptr) {
            // No local root signature of this export binds it: zeros.
            body_.inst(op(Sm4Opcode::MOV), {&d4, &zero});
            return true;
        }
        // Row `row` of the constants: shader record + 32 (the identifier) +
        // the parameter's offset + 16 * row (Raytracing.md, "Shader record").
        const Slot at = fresh_scalar();
        const Operand atd = dst_temp(at.reg, 1u << at.comp), ats = src_temp_scalar(at.reg, at.comp);
        const Operand row = src(arg(2)), sixteen = imm(16), rec = src_temp_scalar(L.frame, 0);
        const Operand base = imm(32 + local->record_offset);
        body_.inst(op(Sm4Opcode::IMAD), {&atd, &row, &sixteen, &rec});
        body_.inst(op(Sm4Opcode::IADD), {&atd, &ats, &base});
        const u32 table = c.table();
        const Operand res = resource(Sm4OperandType::Resource, c.table_ids[table], Index{false, table, 0, 0}, false, 0xE4);
        body_.inst(op(Sm4Opcode::LD_RAW), {&d4, &ats, &res});
        return true;
    }
    default:
        handled = false;
        return true;
    }
}

namespace rt {
using Root = DxilRtProgramDesc::Root;
/// Whether root parameter @p p (or range) binds register @p reg of class
/// @p cls in @p space.
inline bool range_holds(const Root::Range& r, u32 cls, u32 space, u32 reg) {
    return r.cls == cls && r.space == space && reg >= r.base && (r.count == ~0u || reg - r.base < r.count);
}
inline bool param_holds(const Root::Param& p, u32 cls, u32 space, u32 reg) {
    switch (p.kind) {
    case Root::Constants:
    case Root::CBV: return cls == 2 && p.space == space && p.reg == reg;
    case Root::SRV: return cls == 0 && p.space == space && p.reg == reg;
    case Root::UAV: return cls == 1 && p.space == space && p.reg == reg;
    default:
        for (const auto& r : p.ranges) {
            if (range_holds(r, cls, space, reg)) return true;
        }
        return false;
    }
}
/// Whether @p root binds every register of [lower, upper] (~0u: unbounded)
/// of class @p cls in @p space.
inline bool root_covers(const Root& root, u32 cls, u32 space, u32 lower, u32 upper) {
    u64 at = lower;
    const u64 end = upper == kDxbcUnboundedRange ? 0xFFFFFFFFull : u64{upper};
    while (at <= end) {
        u64 next = at;
        if (cls == 3) {
            for (const auto& smp : root.samplers) {
                if (smp.space == space && smp.reg == at) next = at + 1;
            }
        }
        for (const auto& p : root.params) {
            if (p.kind != Root::Table) {
                if (param_holds(p, cls, space, static_cast<u32>(at))) next = std::max(next, at + 1);
                continue;
            }
            for (const auto& r : p.ranges) {
                if (!range_holds(r, cls, space, static_cast<u32>(at))) continue;
                next = std::max<u64>(next, r.count == ~0u ? 0x100000000ull : u64{r.base} + r.count);
            }
        }
        if (next == at) return false;
        at = next;
    }
    return true;
}
}  // namespace rt

bool Lowerer::rt_local_handle(Handle& h) {
    using rt::Root;
    const RtFunctionContext& c = *rt_;
    if (c.local_root == nullptr || h.res == nullptr) return true;
    const Resource& r = *h.res;
    const u32 cls = static_cast<u32>(r.cls);
    // The register this handle names (an element of an array range).
    const u32 reg = h.reg.immediate;
    const Root& root = *c.local_root;
    const RtLevel& L = (*c.levels)[c.level];
    const u32 table = c.table();
    if (r.cls == ResourceClass::Sampler) {
        for (const auto& smp : root.samplers) {
            if (smp.space != r.space || smp.reg < r.lower || smp.reg > r.upper) continue;
            if (h.reg.relative || reg != smp.reg) {
                if (r.lower == r.upper) continue;
                return unimplemented("a sampler array element bound by a local static sampler");
            }
            const auto found = c.samplers->find(smp.reserved);
            if (found == c.samplers->end()) return malformed("a local static sampler without its declaration");
            h = Handle{&found->second, Index{false, found->second.lower, 0, 0}, false};
            return true;
        }
    }
    for (const auto& p : root.params) {
        if (p.kind == Root::Constants) continue;  // read from the record (CBufferLoadLegacy)
        if (p.kind != Root::Table) {
            if (!rt::param_holds(p, cls, r.space, p.reg) || p.reg < r.lower || p.reg > r.upper) continue;
            if (r.lower != r.upper) {
                return unimplemented("a resource array bound by local root descriptors");
            }
            const auto k = c.view_arrays->find({cls, r.id});
            if (k == c.view_arrays->end()) return malformed("a local root descriptor without its view array");
            const Resource& view = (*c.views)[k->second];
            // j = translation[(record base + record index) * stride + k],
            // the record index being the record's offset / its stride (0:
            // the one record).
            const u32 base_word = table == 0 ? kRtConstRecordBaseRaygen
                                : table == 1 ? kRtConstRecordBaseMiss
                                : table == 2 ? kRtConstRecordBaseHit
                                             : kRtConstRecordBaseCallable;
            const Slot index = fresh_scalar();
            const Operand idd = dst_temp(index.reg, 1u << index.comp), ids = src_temp_scalar(index.reg, index.comp);
            const Operand nul = null_operand();
            const Operand rec = src_temp_scalar(L.frame, 0);
            if (table == 0) {
                const Operand zero = imm(0);
                body_.inst(op(Sm4Opcode::MOV), {&idd, &zero});
            } else {
                const u32 stride_word = table == 1 ? kRtConstMissStride
                                      : table == 2 ? kRtConstHitStride
                                                   : kRtConstCallableStride;
                const Operand stride = rt_const(stride_word), zero = imm(0);
                body_.inst(op(Sm4Opcode::UDIV), {&idd, &nul, &rec, &stride});
                body_.inst(op(Sm4Opcode::MOVC), {&idd, &stride, &ids, &zero});
            }
            const Operand rbase = rt_const(base_word), kk = imm(k->second * 4u);
            const Operand four = imm(4 * c.view_stride);
            body_.inst(op(Sm4Opcode::IADD), {&idd, &ids, &rbase});
            body_.inst(op(Sm4Opcode::IMAD), {&idd, &ids, &four, &kk});
            const Slot j = rt_word(c.translation_id, kRtTranslationRegister, ids);
            h = Handle{&view, Index{true, view.lower, j.reg, j.comp}, true};
            return true;
        }
        for (const auto& range : p.ranges) {
            if (!rt::range_holds(range, cls, r.space, reg)) continue;
            if (r.shape == ResourceShape::RTAccelerationStructure) {
                return unimplemented("an acceleration structure in a local descriptor table");
            }
            if (r.counter) return unimplemented("a UAV with a counter in a local descriptor table");
            // slot = (handle - heap start) / increment + the range's table
            // offset + the register's offset in the range (+ the index).
            const Slot handle = rt_word(c.table_ids[table], table, [&] {
                const Slot at = fresh_scalar();
                const Operand atd = dst_temp(at.reg, 1u << at.comp), rec = src_temp_scalar(L.frame, 0),
                              off = imm(32 + p.record_offset);
                body_.inst(op(Sm4Opcode::IADD), {&atd, &rec, &off});
                return src_temp_scalar(at.reg, at.comp);
            }());
            const bool sampler = r.cls == ResourceClass::Sampler;
            const Slot slot = fresh_scalar();
            const Operand sd = dst_temp(slot.reg, 1u << slot.comp), ss = src_temp_scalar(slot.reg, slot.comp);
            const Operand hs = src_temp_scalar(handle.reg, handle.comp), nul = null_operand();
            const Operand start = rt_const(sampler ? kRtConstSamplerHeap : kRtConstViewHeap);
            const Operand inc = rt_const(sampler ? kRtConstSamplerIncrement : kRtConstViewIncrement);
            body_.inst(op(Sm4Opcode::INEG), {&sd, &start});
            body_.inst(op(Sm4Opcode::IADD), {&sd, &ss, &hs});
            body_.inst(op(Sm4Opcode::UDIV), {&sd, &nul, &ss, &inc});
            const Operand add = imm(range.table_offset + (reg - range.base));
            body_.inst(op(Sm4Opcode::IADD), {&sd, &ss, &add});
            if (h.reg.relative) {
                const Operand dyn = src_temp_scalar(h.reg.reg, h.reg.comp);
                body_.inst(op(Sm4Opcode::IADD), {&sd, &ss, &dyn});
            }
            const Resource* record = heap_record(r, sampler);
            h = Handle{record, Index{true, record->lower, slot.reg, slot.comp}, true};
            return true;
        }
    }
    return true;
}

/// The library resources @p fn reads (directly or through the functions it
/// calls): every resource global it loads or indexes.
void rt_used_resources(const DxilModule& m, const DxilFunction* fn, const std::vector<Resource>& resources,
                       std::set<const Resource*>& out, std::set<const DxilFunction*>& seen) {
    if (fn == nullptr || !seen.insert(fn).second) return;
    const auto global_of = [&](u32 id) -> const Resource* {
        const DxilValue* v = m.value(fn, id);
        if (v != nullptr && v->kind == DxilValueKind::Global) {
            for (const Resource& r : resources) {
                if (r.global == id) return &r;
            }
        }
        if (const DxilConstant* ce = m.constant(fn, id);
            ce != nullptr && ce->kind == DxilConstantKind::GepExpr && !ce->elements.empty()) {
            for (const Resource& r : resources) {
                if (r.global == static_cast<u32>(ce->elements[0])) return &r;
            }
        }
        return nullptr;
    };
    for (const DxilInstruction& inst : fn->instructions) {
        if ((inst.op == DxilOpcode::Load || inst.op == DxilOpcode::Gep) && !inst.operands.empty()) {
            if (const Resource* r = global_of(inst.operands[0])) out.insert(r);
        } else if (inst.op == DxilOpcode::Call && !inst.operands.empty()) {
            const DxilFunction* callee = m.function_of(inst.operands[0]);
            if (callee != nullptr && !callee->is_declaration && callee->name.rfind("dx.op.", 0) != 0) {
                rt_used_resources(m, callee, resources, out, seen);
            }
        }
    }
}

bool Lowerer::rt_memory(const DxilInstruction& inst, bool& handled) {
    // A library reads its resources by loading their globals (then
    // CreateHandleForLib); a GEP into a resource-array global names an
    // element.
    handled = false;
    const auto resource_global = [&](u32 id) -> const Resource* {
        const DxilValue* v = m_.value(fn_, id);
        if (v == nullptr || v->kind != DxilValueKind::Global) return nullptr;
        for (const Resource& r : resources_) {
            if (r.global == id) return &r;
        }
        return nullptr;
    };
    if (inst.op == DxilOpcode::Gep && !inst.operands.empty()) {
        const Resource* r = resource_global(inst.operands[0]);
        if (r == nullptr) return true;
        handled = true;
        Handle h{r, Index{false, r->lower, 0, 0}, false};
        if (inst.operands.size() > 2) h.reg = index_of(inst.operands.back(), r->lower);
        rt_gep_handles_[inst.result] = h;
        return true;
    }
    if (inst.op != DxilOpcode::Load || inst.operands.empty()) return true;
    const u32 ptr = inst.operands[0];
    if (const auto found = rt_gep_handles_.find(ptr); found != rt_gep_handles_.end()) {
        handled = true;
        rt_lib_handles_[inst.result] = found->second;
        return true;
    }
    if (const Resource* r = resource_global(ptr)) {
        handled = true;
        rt_lib_handles_[inst.result] = Handle{r, Index{false, r->lower, 0, 0}, false};
        return true;
    }
    if (const DxilConstant* ce = m_.constant(fn_, ptr);
        ce != nullptr && ce->kind == DxilConstantKind::GepExpr && !ce->elements.empty()) {
        if (const Resource* r = resource_global(static_cast<u32>(ce->elements[0]))) {
            handled = true;
            u32 index = 0;
            if (ce->elements.size() > 2) (void)const_u32(static_cast<u32>(ce->elements.back()), index);
            rt_lib_handles_[inst.result] = Handle{r, Index{false, r->lower + index, 0, 0}, false};
            return true;
        }
    }
    return true;
}

bool Lowerer::lower_function_at(u32 base) {
    if (!build_cfg()) return false;
    brk_reg_ = base;
    next_reg_ = base;
    next_comp_ = 1;
    {
        const Operand d = dst_temp(base, 1);
        const Operand z = imm(0);
        body_.inst(op(Sm4Opcode::MOV), {&d, &z});
    }
    do_tree(0);
    if (failed_) return false;
    if (!body_.last_was_jump()) body_.inst(op(Sm4Opcode::RET), {});
    return true;
}

// =============================================================================
// The builder
// =============================================================================

/// "\x01?name@@..." (the MSVC-mangled names DXC gives library functions)
/// -> name; any other name as it is.
std::string rt_demangle(const std::string& name) {
    if (name.size() > 2 && name[0] == '\x01' && name[1] == '?') {
        const usize at = name.find('@', 2);
        return name.substr(2, at == std::string::npos ? std::string::npos : at - 2);
    }
    return name;
}

struct RtLibrary {
    DxilContainer container;
    DxilModule module;
    const u8* source{nullptr};
    /// Resources as read from dx.resources, ids rewritten to the program's.
    std::vector<Resource> resources;
};

struct RtExport {
    u32 library{0};
    const DxilFunction* fn{nullptr};
    u32 kind{0};
};

/// The exports (demangled names) and kinds of @p lib's dx.entryPoints.
bool rt_entry_points(const DxilModule& m, std::map<std::string, std::pair<const DxilFunction*, u32>>& out) {
    const DxilNamedMetadata* eps = m.named("dx.entryPoints");
    if (eps == nullptr) return false;
    for (u32 node : eps->nodes) {
        const DxilMetadata* ep = m.md(node);
        if (ep == nullptr || ep->operands.size() < 5) continue;
        const DxilMetadata* fv = m.md(ep->operands[0]);
        if (fv == nullptr || fv->kind != DxilMetadata::Kind::Value) continue;
        const DxilFunction* fn = m.function_of(fv->value);
        if (fn == nullptr || fn->is_declaration) continue;
        u32 kind = 0;
        if (const DxilMetadata* props = m.md(ep->operands[4])) {
            for (usize i = 0; i + 1 < props->operands.size(); i += 2) {
                u64 tag = 0, v = 0;
                if (m.md_int(props->operands[i], tag) && tag == 8 && m.md_int(props->operands[i + 1], v)) {
                    kind = static_cast<u32>(v);
                }
            }
        }
        out[rt_demangle(fn->name)] = {fn, kind};
    }
    return true;
}

bool rt_calls(const DxilModule& m, const DxilFunction* fn, u32 opcode) {
    for (const DxilInstruction& inst : fn->instructions) {
        if (inst.op != DxilOpcode::Call || inst.operands.size() < 2) continue;
        const DxilFunction* callee = m.function_of(inst.operands[0]);
        if (callee == nullptr || callee->name.rfind("dx.op.", 0) != 0) continue;
        u64 v = 0;
        if (m.constant_int(fn, inst.operands[1], v) && v == opcode) return true;
    }
    return false;
}

DxilRtProgram build_rt_program(const DxilRtProgramDesc& desc, bool validate_only) {
    using Root = DxilRtProgramDesc::Root;
    DxilRtProgram out;
    const auto fail = [&](const std::string& why, bool unimpl) {
        out.ok = false;
        out.failure = why;
        out.unimplemented = unimpl;
        return out;
    };
    std::deque<RtLibrary> libs;
    std::map<std::string, RtExport> exports;
    for (u32 li = 0; li < desc.libraries.size(); ++li) {
        RtLibrary& lib = libs.emplace_back();
        lib.source = desc.libraries[li].data;
        lib.container = parse_dxil_container(lib.source, desc.libraries[li].size);
        if (!lib.container.ok || lib.container.program_kind() != 6) return fail("not a DXIL library", false);
        lib.module = read_dxil_module(lib.container.bitcode, lib.container.bitcode_size);
        if (!lib.module.ok) return fail("DXIL bitcode: " + lib.module.error, false);
        std::map<std::string, std::pair<const DxilFunction*, u32>> entries;
        if (!rt_entry_points(lib.module, entries)) return fail("DXIL library without dx.entryPoints", false);
        if (desc.libraries[li].exports.empty()) {
            for (const auto& [name, e] : entries) {
                if (e.second >= 7 && e.second <= 12) exports[name] = {li, e.first, e.second};
            }
        } else {
            for (const auto& [name, source] : desc.libraries[li].exports) {
                const auto found = entries.find(source);
                if (found == entries.end()) return fail("export " + source + " is not in its library", false);
                exports[name] = {li, found->second.first, found->second.second};
            }
        }
    }
    for (const auto& [name, e] : exports) out.kinds[name] = e.kind;
    // Identifier indices: raygen, miss, callable exports, then hit groups.
    std::map<u32, std::string> by_index;
    std::vector<u32> raygens, misses, callables;
    u32 next_index = 0;
    for (const auto& [name, index] : desc.indices) next_index = std::max(next_index, index + 1);
    const auto index_of_name = [&](const std::string& name) {
        const auto found = desc.indices.find(name);
        const u32 index = found != desc.indices.end() ? found->second : next_index++;
        out.indices[name] = index;
        by_index[index] = name;
        return index;
    };
    for (const auto& [name, e] : exports) {
        if (e.kind != 7 && e.kind != 11 && e.kind != 12) continue;
        const u32 index = index_of_name(name);
        (e.kind == 7 ? raygens : e.kind == 11 ? misses : callables).push_back(index);
    }
    std::vector<RtHitGroup> groups;
    std::vector<const DxilRtProgramDesc::HitGroup*> group_desc;
    const auto kind_of = [&](const std::string& name, u32 kind) -> const RtExport* {
        const auto found = exports.find(name);
        return found != exports.end() && found->second.kind == kind ? &found->second : nullptr;
    };
    for (const auto& g : desc.hit_groups) {
        RtHitGroup h;
        h.index = index_of_name(g.name);
        h.procedural = g.procedural;
        h.any = !g.any_hit.empty();
        h.closest = !g.closest_hit.empty();
        h.intersection = !g.intersection.empty();
        if ((h.any && kind_of(g.any_hit, 9) == nullptr) || (h.closest && kind_of(g.closest_hit, 10) == nullptr) ||
            (h.intersection && kind_of(g.intersection, 8) == nullptr) || (h.procedural != h.intersection)) {
            return fail("hit group " + g.name + " names a missing or mistyped shader", false);
        }
        groups.push_back(h);
        group_desc.push_back(&g);
    }

    // ---- resources: one range ID per distinct range, over all libraries ----
    std::set<std::pair<u32, u32>> local_ranges;  // (space, register) of local root constants
    for (const auto& [owner, root] : desc.local_roots) {
        (void)owner;
        for (const auto& p : root.params) {
            if (p.kind == Root::Constants) local_ranges.insert({p.space, p.reg});
        }
    }
    std::vector<Resource> merged;
    std::map<std::tuple<u32, u32, u32, u32>, u32> ids;  // (class, space, lower, upper) -> id
    std::array<u32, 4> next_id{};
    for (RtLibrary& lib : libs) {
        Lowerer reader(lib.module, lib.container);
        reader.source_ = lib.source;
        const DxilNamedMetadata* res = lib.module.named("dx.resources");
        if (res != nullptr && !res->nodes.empty() && !reader.read_resources(res->nodes[0])) {
            return fail(reader.out_.failure, reader.out_.unimplemented);
        }
        for (Resource r : reader.resources_) {
            const auto key = std::make_tuple(static_cast<u32>(r.cls), r.space, r.lower, r.upper);
            auto found = ids.find(key);
            r.rt_local = r.cls == ResourceClass::CBV && local_ranges.contains({r.space, r.lower});
            if (found == ids.end()) {
                found = ids.emplace(key, next_id[static_cast<u32>(r.cls)]++).first;
                Resource g = r;
                g.id = found->second;
                if (!g.rt_local) merged.push_back(g);
            }
            r.id = found->second;
            lib.resources.push_back(r);
        }
    }
    // ---- root-signature coverage (Raytracing.md, "Subobject association
    // requirements"; measured, d3d12_a48 local_rules): each resource a
    // shader export reads is bound by its global root signature or its
    // local one, not both and not neither. A hit group's shaders are
    // checked with the hit group's local root signature. ----
    if (desc.validate_bindings) {
        std::map<std::string, std::string> local_owner;  // function export -> the root's owner
        for (const auto& g : desc.hit_groups) {
            for (const std::string* part : {&g.any_hit, &g.closest_hit, &g.intersection}) {
                if (!part->empty()) local_owner[*part] = g.name;
            }
        }
        for (const auto& [name, e] : exports) {
            if (e.kind < 7 || e.kind > 12) continue;
            std::set<const Resource*> used;
            std::set<const DxilFunction*> seen;
            rt_used_resources(libs[e.library].module, e.fn, libs[e.library].resources, used, seen);
            const Root* global = nullptr;
            if (const auto g = desc.global_roots.find(name); g != desc.global_roots.end()) global = &g->second;
            const Root* local = nullptr;
            const auto owner = local_owner.find(name);
            if (const auto l = desc.local_roots.find(owner != local_owner.end() ? owner->second : name);
                l != desc.local_roots.end()) {
                local = &l->second;
            }
            for (const Resource* r : used) {
                const u32 cls = static_cast<u32>(r->cls);
                const bool in_global = global != nullptr && rt::root_covers(*global, cls, r->space, r->lower, r->upper);
                const bool in_local = local != nullptr && rt::root_covers(*local, cls, r->space, r->lower, r->upper);
                if (in_global == in_local) {
                    return fail(name + ": " + (r->name.empty() ? std::string("a resource") : r->name) +
                                    (in_global ? " is bound by both its global and its local root signature"
                                               : " is bound by neither its global nor its local root signature"),
                                false);
                }
            }
        }
    }
    // ---- payload and attribute sizes (Raytracing.md,
    // D3D12_RAYTRACING_SHADER_CONFIG: scalars counted 4 bytes each, at most
    // MaxPayloadSizeInBytes / MaxAttributeSizeInBytes; a triangle hit's
    // attributes are BuiltInTriangleIntersectionAttributes, 8 bytes;
    // measured, d3d12_a48: a 4-byte attribute config with a triangle closest
    // hit is E_INVALIDARG) ----
    if (desc.check_sizes) {
        for (const auto& [name, e] : exports) {
            const DxilModule& m = libs[e.library].module;
            const DxilType& ft = m.types[e.fn->type];
            const auto param_bytes = [&](usize k) -> u32 {
                if (ft.members.size() <= 1 + k || m.types[ft.members[1 + k]].kind != DxilTypeKind::Pointer) return 0;
                return 4 * scalar_count(m, m.types[ft.members[1 + k]].element);
            };
            u32 payload = 0, attributes = 0;
            if (e.kind == 11 || e.kind == 10 || e.kind == 9) payload = param_bytes(0);
            if (e.kind == 10 || e.kind == 9) attributes = param_bytes(1);
            // TraceRay's payload (dx.op 157, operand 15) and ReportHit's
            // attributes (158, operand 3) where a shader makes them.
            for (const DxilInstruction& inst : e.fn->instructions) {
                if (inst.op != DxilOpcode::Call || inst.operands.size() < 2) continue;
                const DxilFunction* callee = m.function_of(inst.operands[0]);
                u64 op = 0;
                if (callee == nullptr || callee->name.rfind("dx.op.", 0) != 0 ||
                    !m.constant_int(e.fn, inst.operands[1], op)) {
                    continue;
                }
                const usize at = op == 157 ? 16u : op == 158 ? 4u : 0u;
                if (at == 0 || inst.operands.size() <= at) continue;
                const u32 t = m.type_of(e.fn, inst.operands[at]);
                if (t >= m.types.size() || m.types[t].kind != DxilTypeKind::Pointer) continue;
                (op == 157 ? payload : attributes) =
                    std::max(op == 157 ? payload : attributes, 4 * scalar_count(m, m.types[t].element));
            }
            if (payload > desc.max_payload_bytes || attributes > desc.max_attribute_bytes) {
                return fail(name + ": a payload or attributes larger than the shader config allows", false);
            }
        }
    }
    if (validate_only) {
        out.ok = true;
        return out;
    }
    // Reserved: the four tables, the dispatch constants, one companion per
    // element of a bounded acceleration-structure range.
    const auto reserve = [&](ResourceClass cls, ResourceShape shape, u32 reg, u32 cb_bytes, const char* name) {
        Resource r;
        r.cls = cls;
        r.shape = shape;
        r.space = kDxilRtSpace;
        r.lower = r.upper = reg;
        r.cb_bytes = cb_bytes;
        r.name = name;
        r.id = next_id[static_cast<u32>(cls)]++;
        merged.push_back(r);
        return r.id;
    };
    std::array<u32, 4> table_ids{};
    static constexpr const char* kTableNames[4] = {"vitreo_rt_raygen", "vitreo_rt_miss", "vitreo_rt_hit",
                                                   "vitreo_rt_callable"};
    for (u32 t = 0; t < 4; ++t) table_ids[t] = reserve(ResourceClass::SRV, ResourceShape::RawBuffer, t, 0, kTableNames[t]);
    const u32 const_id = reserve(ResourceClass::CBV, ResourceShape::TypedBuffer, 0, kRtConstWords * 4, "vitreo_rt_dispatch");
    // ---- local root descriptors: one view array per library resource
    // they bind (dxil_raytracing.hpp), static samplers, the translation
    // buffer ----
    std::map<std::pair<u32, u32>, u32> view_arrays;  // (class, range id) -> k
    std::vector<Resource> views;
    std::map<u32, Resource> local_samplers;
    for (const auto& [owner, root] : desc.local_roots) {
        for (const auto& p : root.params) {
            if (p.kind != Root::CBV && p.kind != Root::SRV && p.kind != Root::UAV) continue;
            const u32 cls = p.kind == Root::CBV ? 2u : p.kind == Root::SRV ? 0u : 1u;
            for (const RtLibrary& lib : libs) {
                for (const Resource& r : lib.resources) {
                    if (static_cast<u32>(r.cls) != cls || r.space != p.space || p.reg < r.lower || p.reg > r.upper) {
                        continue;
                    }
                    auto [it, fresh] = view_arrays.emplace(std::make_pair(cls, r.id), static_cast<u32>(views.size()));
                    if (fresh) {
                        Resource v = r;
                        v.space = kDxilRtSpace;
                        v.lower = kRtLocalViewBase + it->second * kRtLocalViewSlots;
                        v.upper = v.lower + kRtLocalViewSlots - 1u;
                        v.global = ~0u;
                        v.rt_local = false;
                        v.name = "vitreo_rt_local";
                        views.push_back(v);
                        out.local_views.push_back({p.kind, r.shape == ResourceShape::RTAccelerationStructure});
                    }
                    auto& list = out.owner_views[owner];
                    const std::pair<u32, u32> entry{it->second, p.record_offset};
                    if (std::find(list.begin(), list.end(), entry) == list.end()) list.push_back(entry);
                }
            }
        }
        for (const auto& smp : root.samplers) {
            for (const RtLibrary& lib : libs) {
                for (const Resource& r : lib.resources) {
                    if (r.cls != ResourceClass::Sampler || r.space != smp.space || smp.reg < r.lower ||
                        smp.reg > r.upper || local_samplers.contains(smp.reserved)) {
                        continue;
                    }
                    Resource v = r;
                    v.space = kDxilRtSpace;
                    v.lower = v.upper = kRtLocalSamplerBase + smp.reserved;
                    v.global = ~0u;
                    v.name = "vitreo_rt_static";
                    local_samplers[smp.reserved] = v;
                }
            }
        }
    }
    for (Resource& v : views) {
        v.id = next_id[static_cast<u32>(v.cls)]++;
        merged.push_back(v);
    }
    for (auto& [n, v] : local_samplers) {
        (void)n;
        v.id = next_id[3]++;
        merged.push_back(v);
    }
    const u32 translation_id = views.empty() ? ~0u
        : reserve(ResourceClass::SRV, ResourceShape::RawBuffer, kRtTranslationRegister, 0, "vitreo_rt_translation");
    std::map<u32, u32> companions;
    std::map<u32, u32> companion_regs;
    {
        std::vector<Resource> as_list;
        for (const Resource& r : merged) {
            if (r.cls == ResourceClass::SRV && r.shape == ResourceShape::RTAccelerationStructure &&
                r.upper != kDxbcUnboundedRange) {
                as_list.push_back(r);
            }
        }
        u32 reg = kRtCompanionBase;
        for (const Resource& r : as_list) {
            const u32 id = reserve(ResourceClass::SRV, ResourceShape::RawBuffer, reg, 0, "vitreo_rt_blob");
            merged.back().upper = reg + (r.upper - r.lower);
            companions[r.id] = id;
            companion_regs[id] = reg;
            reg += r.upper - r.lower + 1u;
        }
    }
    // SM 6.6 heap-indexed resources (and local descriptor tables): one list
    // for every copy, ids after everything declared.
    RtHeapResources heap;
    heap.base = next_id;

    // ---- plan: every (identifier index, role, level) a dispatch can reach ----
    const u32 max_trace = std::max(desc.max_recursion, 1u);
    const u32 max_level = max_trace + 1;
    struct Copy {
        u32 index;
        RtRole role;
        u32 level;
        const RtExport* e;
        std::string owner;  // the export or hit group whose local root signature applies
    };
    std::vector<Copy> copies;
    std::map<std::tuple<u32, u32, u32>, u32> labels;
    const auto add = [&](u32 index, RtRole role, u32 level, const RtExport* e, const std::string& owner) {
        const auto key = std::make_tuple(index, static_cast<u32>(role), level);
        if (labels.contains(key)) return;
        labels[key] = static_cast<u32>(copies.size());
        copies.push_back({index, role, level, e, owner});
    };
    for (const u32 r : raygens) add(r, RtRole::Raygen, 0, &exports[by_index[r]], by_index[r]);
    for (usize at = 0; at < copies.size(); ++at) {
        const Copy c = copies[at];
        const DxilModule& m = libs[c.e->library].module;
        if (c.level < max_trace && rt_calls(m, c.e->fn, 157)) {
            const u32 q = c.level + 1;
            for (usize g = 0; g < groups.size(); ++g) {
                const auto& d = *group_desc[g];
                const std::string& owner = d.name;
                if (groups[g].any) add(groups[g].index, groups[g].procedural ? RtRole::AnyProc : RtRole::AnyTri, q, &exports[d.any_hit], owner);
                if (groups[g].intersection) add(groups[g].index, RtRole::Intersection, q, &exports[d.intersection], owner);
                if (groups[g].closest) add(groups[g].index, groups[g].procedural ? RtRole::ClosestProc : RtRole::ClosestTri, q, &exports[d.closest_hit], owner);
            }
            for (const u32 mi : misses) add(mi, RtRole::Miss, q, &exports[by_index[mi]], by_index[mi]);
        }
        if (c.level < max_level && rt_calls(m, c.e->fn, 159)) {
            for (const u32 ci : callables) add(ci, RtRole::Callable, c.level + 1, &exports[by_index[ci]], by_index[ci]);
        }
    }

    // ---- registers and arrays ----
    const u32 payload_words = std::max(desc.max_payload_bytes / 4u, 1u);
    const u32 attr_words = std::max(desc.max_attribute_bytes / 4u, 2u);
    u32 call_words = 1;
    for (const u32 ci : callables) {
        const RtExport& e = exports[by_index[ci]];
        const DxilModule& m = libs[e.library].module;
        const DxilType& ft = m.types[e.fn->type];
        if (ft.members.size() >= 2 && m.types[ft.members[1]].kind == DxilTypeKind::Pointer) {
            call_words = std::max(call_words, scalar_count(m, m.types[ft.members[1]].element));
        }
    }
    std::vector<u32> indexables;
    std::vector<RtLevel> levels(max_level + 1);
    u32 next_reg = 0;
    for (RtLevel& l : levels) {
        l.frame = next_reg++;
        l.frame2 = next_reg++;
        l.payload = static_cast<u32>(indexables.size());
        indexables.push_back(payload_words);
        l.attr = static_cast<u32>(indexables.size());
        indexables.push_back(attr_words);
        l.attrc = static_cast<u32>(indexables.size());
        indexables.push_back(attr_words);
        l.call = static_cast<u32>(indexables.size());
        indexables.push_back(call_words);
    }
    std::vector<u32> miss_indices(misses.begin(), misses.end());
    std::vector<u32> callable_indices(callables.begin(), callables.end());
    const auto label_of = [&](u32 index, RtRole role, u32 level) -> u32 {
        const auto found = labels.find(std::make_tuple(index, static_cast<u32>(role), level));
        return found == labels.end() ? ~0u : found->second;
    };

    // ---- lower every copy as subroutine l<copy> ----
    Emitter subroutines;
    std::set<u32> system_inputs;
    u32 high_water = next_reg + 8;  // main's registers follow the frames
    const u32 main_base = next_reg;
    next_reg = high_water;
    for (u32 ci = 0; ci < copies.size(); ++ci) {
        const Copy& copy = copies[ci];
        RtLibrary& lib = libs[copy.e->library];
        Lowerer l(lib.module, lib.container);
        l.source_ = lib.source;
        l.host_ = make_ext_host(l);
        l.out_.stage = ShaderStage::Compute;
        l.resources_ = lib.resources;
        l.fn_ = copy.e->fn;
        l.preserve_denormals_ = l.fn_->fp32_denorm_mode == "preserve";
        l.indexable_sizes_ = indexables;
        l.rt_companion_regs_ = companion_regs;
        l.rt_heap_ = &heap;
        RtFunctionContext ctx;
        ctx.role = copy.role;
        ctx.level = copy.level;
        ctx.max_trace = max_trace;
        ctx.max_level = max_level;
        for (u32 g = 0; g < groups.size(); ++g) {
            if (groups[g].index == copy.index) ctx.hit_group = g;
        }
        ctx.levels = &levels;
        ctx.label_of = label_of;
        ctx.hit_groups = &groups;
        ctx.miss_indices = miss_indices;
        ctx.callable_indices = callable_indices;
        for (u32 t = 0; t < 4; ++t) ctx.table_ids[t] = table_ids[t];
        ctx.const_id = const_id;
        ctx.companions = &companions;
        ctx.pipeline_flags = desc.pipeline_flags & 0x300u;
        ctx.payload_words = payload_words;
        ctx.attr_words = attr_words;
        ctx.call_words = call_words;
        if (const auto found = desc.local_roots.find(copy.owner); found != desc.local_roots.end()) {
            ctx.local_root = &found->second;
            for (const auto& p : found->second.params) {
                if (p.kind == Root::Constants) ctx.local.push_back({p.space, p.reg, p.record_offset, p.words});
            }
        }
        ctx.view_arrays = &view_arrays;
        ctx.views = &views;
        ctx.samplers = &local_samplers;
        ctx.translation_id = translation_id;
        ctx.view_stride = static_cast<u32>(views.size());
        l.rt_ = &ctx;
        // Pointer parameters: the level's payload / attributes / parameter.
        const RtLevel& L = levels[copy.level];
        const u32 first_arg = l.fn_->first_local;
        const auto bind_arg = [&](u32 k, u32 array) {
            l.pointers_[first_arg + k] = Pointer{false, array, 0, false, {}};
        };
        switch (copy.role) {
        case RtRole::Miss: bind_arg(0, L.payload); break;
        case RtRole::ClosestTri: case RtRole::AnyTri: case RtRole::AnyProc:
            bind_arg(0, L.payload);
            bind_arg(1, L.attr);
            break;
        case RtRole::ClosestProc:
            bind_arg(0, L.payload);
            bind_arg(1, L.attrc);
            break;
        case RtRole::Callable: bind_arg(0, L.call); break;
        default: break;
        }
        if (!l.lower_function_at(next_reg)) {
            return fail(by_index[copy.index] + ": " + l.out_.failure, l.out_.unimplemented);
        }
        const Operand label = label_operand(ci);
        subroutines.inst(op(Sm4Opcode::LABEL), {&label});
        for (u32 token : l.prologue_.code()) subroutines.raw(token);
        for (u32 token : l.body_.code()) subroutines.raw(token);
        // A body that ends in a jump (its last block returns early) still
        // needs the closing ret of the subroutine.
        subroutines.inst(op(Sm4Opcode::RET), {});
        next_reg = l.next_reg_ + 1;
        indexables = l.indexable_sizes_;
        for (u32 s : l.system_inputs_) system_inputs.insert(s);
        if (!l.tgsm_sizes_.empty()) return fail("groupshared memory in a raytracing shader", false);
    }

    // ---- main: bounds, the raygen record, its shader ----
    RtLibrary& first = libs[0];
    Lowerer main(first.module, first.container);
    main.source_ = first.source;
    main.out_.stage = ShaderStage::Compute;
    main.resources_ = merged;
    main.heap_resources_ = heap.list;
    main.threads_[0] = 8;
    main.threads_[1] = 8;
    main.threads_[2] = 1;
    main.next_reg_ = main_base;
    main.next_comp_ = 4;
    RtFunctionContext main_ctx;
    main_ctx.const_id = const_id;
    for (u32 t = 0; t < 4; ++t) main_ctx.table_ids[t] = table_ids[t];
    main.rt_ = &main_ctx;
    system_inputs.insert(static_cast<u32>(Sm4OperandType::InputThreadId));
    {
        Emitter& b = main.body_;
        const Slot inside = main.fresh_scalar(), tmp = main.fresh_scalar();
        const Operand id_ = dst_temp(inside.reg, 1u << inside.comp), is = src_temp_scalar(inside.reg, inside.comp);
        const Operand td = dst_temp(tmp.reg, 1u << tmp.comp), tsrc = src_temp_scalar(tmp.reg, tmp.comp);
        for (u32 k = 0; k < 3; ++k) {
            const Operand v = special_src(Sm4OperandType::InputThreadId, swizzle_of(k));
            const Operand dim = main.rt_const(kRtConstWidth + k);
            b.inst(op(Sm4Opcode::ULT), {k == 0 ? &id_ : &td, &v, &dim});
            if (k != 0) b.inst(op(Sm4Opcode::AND), {&id_, &is, &tsrc});
        }
        const Operand sel = src_temp_select(inside.reg, inside.comp);
        b.inst(op(Sm4Opcode::IF), {&sel});
        b.inst(op(Sm4Opcode::RET), {});
        b.inst(op(Sm4Opcode::ENDIF), {});
        const Operand rec = dst_temp(levels[0].frame, 1u), zero = imm(0);
        b.inst(op(Sm4Opcode::MOV), {&rec, &zero});
        const Slot id = main.rt_word(table_ids[0], 0, imm(rt::kIdentifierIndexByte));
        std::vector<std::pair<u32, u32>> cases;
        for (const u32 r : raygens) cases.emplace_back(r, label_of(r, RtRole::Raygen, 0));
        main.rt_call_switch(id, cases);
        b.inst(op(Sm4Opcode::RET), {});
        for (u32 token : subroutines.code()) b.raw(token);
    }
    main.next_reg_ = std::max(next_reg, main.next_reg_ + 1);
    main.indexable_sizes_ = indexables;
    main.system_inputs_ = system_inputs;
    if (!main.declare()) return fail(main.out_.failure, main.out_.unimplemented);
    out.container = main.write_container();
    out.ok = true;
    return out;
}
