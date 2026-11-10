// SPDX-License-Identifier: LGPL-2.1-only
// Compute-shader quads and derivatives (shader model 6.6), part of
// dxil_lower.cpp's Lowerer; declarations in dxil_lower_members.inc.
//
// DirectX-Specs "HLSL Derivative Operations in Compute, Mesh and
// Amplification Shaders" (v1.00): ddx / ddy (coarse and fine), Sample,
// SampleBias, SampleCmp, CalculateLevelOfDetail(Unclamped) "must be
// supported in compute shaders on devices that support D3D_SHADER_MODEL_6_6"
// and operate "exactly as their existing counterparts with the exception of
// using the local quads". The quads are fixed by numthreads (1D: X % 4 == 0,
// Y == Z == 1, SV_GroupIndex & 1 / & 2 the position; 2D: X, Y even,
// SV_GroupThreadID.xy & 1 the position; any other shape is an error), and
// in shader model 6.6 and later the quad reads (QuadReadLaneAt, QuadReadAcross*,
// QuadAny / QuadAll) use the same quads.
//
// The quad of an implicit LOD is the coarse derivative (D3D11.3 7.18.11
// names deriv_rtx_coarse / deriv_rty_coarse); a Metal kernel has no
// implicit derivatives (MSL rejects dfdx in a kernel; sample and
// calculate_*_lod compile but use LOD 0, measured on the M4 Pro), so the
// gradients come from quad reads and the instruction is its explicit-
// gradient form: sample_d for Sample, sample_d with gradients scaled by
// 2^bias for SampleBias (log2(|2^b g|) = b + log2|g|, the anisotropy ratio
// unchanged; the bias clamped to [-16, 15.99] as 7.18.11 bounds srcLODBias),
// VX_TEX SampleCmpGrad for SampleCmp and VX_TEX LodGrad for CalculateLOD.

namespace {

bool is_quad_derivative_op(u32 opcode) {
    // Sample, SampleBias, SampleCmp, CalculateLOD, DerivCoarseX/Y,
    // DerivFineX/Y, SampleCmpBias (DXIL.rst opcode table).
    return opcode == 60 || opcode == 61 || opcode == 64 || opcode == 81 ||
           (opcode >= 83 && opcode <= 86) || opcode == 255;
}

bool is_quad_read_op(u32 opcode) {
    return opcode == 122 || opcode == 123 || opcode == 222;  // QuadReadLaneAt, QuadOp, QuadVote
}

}  // namespace

bool Lowerer::plan_compute_quads() {
    quad_layout_ = QuadLayout::None;
    if (out_.stage != ShaderStage::Compute) return true;
    bool derivatives = false, quads = false;
    for (const DxilInstruction& i : fn_->instructions) {
        if (i.op != DxilOpcode::Call || i.operands.size() < 2) continue;
        const DxilFunction* callee = m_.function_of(i.operands[0]);
        if (callee == nullptr || callee->name.rfind("dx.op.", 0) != 0) continue;
        u32 opcode = 0;
        if (!const_u32(i.operands[1], opcode)) continue;
        derivatives = derivatives || is_quad_derivative_op(opcode);
        quads = quads || is_quad_read_op(opcode);
    }
    const bool sm66 = out_.shader_model_major > 6 ||
                      (out_.shader_model_major == 6 && out_.shader_model_minor >= 6);
    if (derivatives && !sm66) return malformed("derivatives in a compute shader below shader model 6.6");
    const u32 x = threads_[0], y = threads_[1], z = threads_[2];
    const bool linear = x % 4 == 0 && y == 1 && z == 1;
    const bool grid = !linear && x % 2 == 0 && y % 2 == 0;
    if (derivatives && !linear && !grid) {
        return malformed("derivatives in a compute shader whose numthreads fits no quad layout");
    }
    if ((derivatives || quads) && sm66) {
        quad_layout_ = linear ? QuadLayout::Linear : grid ? QuadLayout::Grid : QuadLayout::None;
    }
    return true;
}

void Lowerer::grid_thread_id(u32 which, const Operand& d) {
    const u32 nx = threads_[0], ny = threads_[1], nz = threads_[2];
    const auto s_dst = [](Slot s) { return dst_temp(s.reg, 1u << s.comp); };
    const auto s_src = [](Slot s) { return src_temp_scalar(s.reg, s.comp); };
    const Operand flat = special_src(Sm4OperandType::InputThreadIdInGroupFlattened, swizzle_of(0));
    const Slot quad = tmp(), lane = tmp(), rest = tmp(), qx = tmp(), qy = tmp(), zz = tmp(), bit = tmp();
    const Slot xx = tmp(), yy = tmp();
    {
        const Operand q = s_dst(quad), two = imm(2);
        body_.inst(op(Sm4Opcode::USHR), {&q, &flat, &two});
        const Operand l = s_dst(lane), three = imm(3);
        body_.inst(op(Sm4Opcode::AND), {&l, &flat, &three});
    }
    {
        // quad = qx + (X/2) * (qy + (Y/2) * z)
        const Operand r = s_dst(rest), rx = s_dst(qx), q = s_src(quad), w = imm(nx / 2);
        body_.inst(op(Sm4Opcode::UDIV), {&r, &rx, &q, &w});
        const Operand zd = s_dst(zz), ry = s_dst(qy), rs = s_src(rest), h = imm(ny / 2);
        body_.inst(op(Sm4Opcode::UDIV), {&zd, &ry, &rs, &h});
    }
    {
        const Operand b = s_dst(bit), l = s_src(lane), one = imm(1);
        body_.inst(op(Sm4Opcode::AND), {&b, &l, &one});
        const Operand xd = s_dst(xx), qxs = s_src(qx), two = imm(2), bs = s_src(bit);
        body_.inst(op(Sm4Opcode::IMAD), {&xd, &qxs, &two, &bs});
        body_.inst(op(Sm4Opcode::USHR), {&b, &l, &one});
        const Operand yd = s_dst(yy), qys = s_src(qy);
        body_.inst(op(Sm4Opcode::IMAD), {&yd, &qys, &two, &bs});
    }
    const Slot ids[3] = {xx, yy, zz};
    if (which < 3) {
        const Operand s = s_src(ids[which]);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return;
    }
    if (which == 3) {
        // SV_GroupIndex = x + y * X + z * X * Y (DirectX-Specs: the
        // flattened group thread ID).
        const Slot t = tmp();
        const Operand td = s_dst(t), ys = s_src(yy), w = imm(nx), xs = s_src(xx);
        body_.inst(op(Sm4Opcode::IMAD), {&td, &ys, &w, &xs});
        const Operand zs = s_src(zz), plane = imm(nx * ny), ts = s_src(t);
        body_.inst(op(Sm4Opcode::IMAD), {&d, &zs, &plane, &ts});
        return;
    }
    // SV_DispatchThreadID = SV_GroupID * numthreads + SV_GroupThreadID.
    const u32 c = which - 4;
    const u32 n[3] = {nx, ny, nz};
    const Operand g = special_src(Sm4OperandType::InputThreadGroupId, swizzle_of(c));
    const Operand w = imm(n[c]), is = s_src(ids[c]);
    body_.inst(op(Sm4Opcode::IMAD), {&d, &g, &w, &is});
}

void Lowerer::quad_fsub(const Operand& d, const Operand& a, const Operand& b) {
    Operand nb = b;
    // Negate the operand: flip its source-modifier bits (the extended
    // operand token) the way src(id, 1) does, via a temp copy.
    const Slot t = tmp();
    const Operand td = dst_temp(t.reg, 1u << t.comp);
    body_.inst(op(Sm4Opcode::MOV), {&td, &b});
    nb = src_temp_scalar(t.reg, t.comp, 1);
    if (preserve_denormals_) {
        f32p(dxbc_vitreo_ext::F32POp::Add, d, {&a, &nb});
        return;
    }
    body_.inst(op(Sm4Opcode::ADD), {&d, &a, &nb});
}

void Lowerer::quad_derivative(u32 kind, const Operand& d, const Operand& v) {
    using namespace dxbc_vitreo_ext;
    const auto read = [&](WaveKind k, const Operand& to, std::initializer_list<const Operand*> rest) {
        std::vector<const Operand*> all{&to};
        all.insert(all.end(), rest.begin(), rest.end());
        const Operand c = control_operand(wave_control(k, WaveOp::None, WaveType::U32));
        usize length = 1 + c.tokens.size();
        for (const Operand* x : all) length += x->tokens.size();
        body_.raw(kOpWave | (static_cast<u32>(length) << 24));
        for (u32 t : c.tokens) body_.raw(t);
        for (const Operand* x : all) {
            for (u32 t : x->tokens) body_.raw(t);
        }
    };
    if (kind < 2) {
        // Coarse: the quad's upper-left lane against its right (x) or lower
        // (y) neighbour, the same value in all four lanes.
        const Slot a = tmp(), b = tmp();
        const Operand ad = dst_temp(a.reg, 1u << a.comp), bd = dst_temp(b.reg, 1u << b.comp);
        const Operand far = imm(kind == 0 ? 1u : 2u), near = imm(0);
        read(WaveKind::QuadReadLaneAt, ad, {&v, &far});
        read(WaveKind::QuadReadLaneAt, bd, {&v, &near});
        quad_fsub(d, src_temp_scalar(a.reg, a.comp), src_temp_scalar(b.reg, b.comp));
        return;
    }
    // Fine: each lane against its row (x) or column (y) partner, right minus
    // left / lower minus upper.
    const bool x = kind == 2;
    const Slot other = tmp(), lane = tmp(), diff = tmp();
    const Operand od = dst_temp(other.reg, 1u << other.comp);
    read(x ? WaveKind::QuadAcrossX : WaveKind::QuadAcrossY, od, {&v});
    const Operand ld = dst_temp(lane.reg, 1u << lane.comp);
    read(WaveKind::LaneIndex, ld, {});
    const Operand ls = src_temp_scalar(lane.reg, lane.comp), bit = imm(x ? 1u : 2u);
    body_.inst(op(Sm4Opcode::AND), {&ld, &ls, &bit});
    const Operand dd = dst_temp(diff.reg, 1u << diff.comp);
    quad_fsub(dd, v, src_temp_scalar(other.reg, other.comp));
    const Operand pos = src_temp_scalar(diff.reg, diff.comp), neg = src_temp_scalar(diff.reg, diff.comp, 1);
    body_.inst(op(Sm4Opcode::MOVC), {&d, &ls, &pos, &neg});
}

void Lowerer::quad_gradients(const std::vector<u32>& ids, u32& gx, u32& gy) {
    gx = fresh_vector();
    gy = fresh_vector();
    for (u32 k = 0; k < 4; ++k) {
        const Operand dx = dst_temp(gx, 1u << k), dy = dst_temp(gy, 1u << k);
        if (k < ids.size() && !undef(ids[k])) {
            const Operand v = src(ids[k]);
            quad_derivative(0, dx, v);
            quad_derivative(1, dy, v);
        } else {
            const Operand zero = imm(0);
            body_.inst(op(Sm4Opcode::MOV), {&dx, &zero});
            body_.inst(op(Sm4Opcode::MOV), {&dy, &zero});
        }
    }
}

bool Lowerer::emit_quad_sample(u32 opcode, const DxilInstruction& inst, bool& status_done) {
    using namespace dxbc_vitreo_ext;
    status_done = false;
    if (quad_layout_ == QuadLayout::None) return malformed("derivative sampling without a quad layout");
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const auto present = [&](usize k) { return inst.operands.size() > 1 + k && !undef(arg(k)); };
    Handle h, smp;
    if (!handle_of(arg(1), h) || !handle_of(arg(2), smp)) return false;
    const Operand res = resource_src(h);
    const Operand s = resource_src(smp);
    const Operand nul = null_operand();
    if (opcode == 81) {  // CalculateLOD(handle, sampler, c0, c1, c2, clamped)
        u32 clamped = 0;
        if (!const_u32(arg(6), clamped)) return malformed("non-constant CalculateLOD clamp");
        u32 coords = 0;
        coords_vector({arg(3), arg(4), arg(5)}, coords);
        u32 gx = 0, gy = 0;
        quad_gradients({arg(3), arg(4), arg(5)}, gx, gy);
        const u32 r = fresh_vector();
        const Operand rd = dst_temp(r, 0x3);
        const Operand c = src_temp(coords, 0xE4), x = src_temp(gx, 0xE4), y = src_temp(gy, 0xE4);
        emit_ext(body_, kOpTex, static_cast<u32>(TexOp::LodGrad), {&rd, &c, &res, &s, &x, &y});
        slots_[inst.result] = Slot{r, clamped != 0 ? 0u : 1u};
        status_done = true;
        return true;
    }
    // Sample / SampleBias / SampleCmp(srv, sampler, c0..c3, o0..o2, ...)
    u32 coords = 0;
    coords_vector({arg(3), arg(4), arg(5), arg(6)}, coords);
    u32 ext = 0, offset_reg = 0;
    bool programmable = false;
    if (!texel_offsets(inst, 8, 3, ext, programmable, offset_reg)) return false;
    if (programmable) reduce_programmable_offsets(offset_reg, true);
    const usize tail = 10;
    u32 gx = 0, gy = 0;
    quad_gradients({arg(3), arg(4), arg(5)}, gx, gy);
    const Operand c = src_temp(coords, 0xE4);
    const Operand x = src_temp(gx, 0xE4), y = src_temp(gy, 0xE4);
    const u32 r = vec(inst.result);
    const auto emit = [&](u32 o, std::initializer_list<const Operand*> operands) {
        if (ext != 0) body_.inst_ext(o, ext, operands);
        else body_.inst(o, operands);
    };
    if (opcode == 60 || opcode == 61) {
        usize clamp_at = tail;
        if (opcode == 61) {
            // Scale both gradients by 2^clamp(bias, -16, 15.99).
            const Slot b = tmp();
            const Operand bd = dst_temp(b.reg, 1u << b.comp), bias = src(arg(tail));
            const Operand lo = imm(0xC1800000u), hi = imm(0x417FD70Au);  // -16.0f, 15.99f
            const Operand bs = src_temp_scalar(b.reg, b.comp);
            body_.inst(op(Sm4Opcode::MAX), {&bd, &bias, &lo});
            body_.inst(op(Sm4Opcode::MIN), {&bd, &bs, &hi});
            body_.inst(op(Sm4Opcode::EXP), {&bd, &bs});
            const Operand gxd = dst_temp(gx, 0x7), gyd = dst_temp(gy, 0x7);
            const Operand scale = src_temp_scalar(b.reg, b.comp);
            body_.inst(op(Sm4Opcode::MUL), {&gxd, &x, &scale});
            body_.inst(op(Sm4Opcode::MUL), {&gyd, &y, &scale});
            clamp_at = tail + 1;
        }
        const Operand d = dst_temp(r, 0xF);
        if (present(clamp_at)) {
            const Operand cl = src(arg(clamp_at));
            emit(ops::kSampleDClS, {&d, &nul, &c, &res, &s, &x, &y, &cl});
        } else {
            emit(op(Sm4Opcode::SAMPLE_D), {&d, &c, &res, &s, &x, &y});
        }
        return true;
    }
    // SampleCmp(..., compare, clamp)
    const Operand ref = src(arg(tail));
    const Operand dx = dst_temp(r, 0x1);
    const Operand rr = resource_src(h, 0x00);
    const Operand status = tex_status(inst.result);
    u32 control = static_cast<u32>(TexOp::SampleCmpGrad) | (ext & kTexOffsetMask);
    if (present(tail + 1)) {
        control |= kTexClamp;
        const Operand cl = src(arg(tail + 1));
        emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &x, &y, &cl});
    } else {
        emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &x, &y});
    }
    status_done = true;
    return true;
}

Operand Lowerer::tex_status(u32 result) {
    if (result == ~0u || !status_used(result)) return null_operand();
    const Slot st = fresh_scalar();
    status_slots_[result] = st;
    return dst_temp(st.reg, 1u << st.comp);
}

bool Lowerer::emit_sample_cmp_ext(u32 opcode, const DxilInstruction& inst) {
    // dxc 1.9 listings (tools/oracle_probe/d3d12_sm68/cs_cmp.dxil.txt):
    //   SampleCmpLevel(srv, sampler, coord0-3, offset0-2, compareValue, lod)
    //   SampleCmpGrad(srv, sampler, coord0-3, offset0-2, compareValue,
    //                 ddx0-2, ddy0-2, clamp)
    //   SampleCmpBias(srv, sampler, coord0-3, offset0-2, compareValue, bias,
    //                 clamp)
    using namespace dxbc_vitreo_ext;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const auto present = [&](usize k) { return inst.operands.size() > 1 + k && !undef(arg(k)); };
    Handle h, smp;
    if (!handle_of(arg(1), h) || !handle_of(arg(2), smp)) return false;
    u32 coords = 0;
    coords_vector({arg(3), arg(4), arg(5), arg(6)}, coords);
    u32 ext = 0, offset_reg = 0;
    bool programmable = false;
    if (!texel_offsets(inst, 8, 3, ext, programmable, offset_reg)) return false;
    if (programmable) reduce_programmable_offsets(offset_reg, true);
    const usize tail = 10;
    const u32 r = vec(inst.result);
    const Operand dx = dst_temp(r, 0x1);
    const Operand c = src_temp(coords, 0xE4);
    const Operand rr = resource_src(h, 0x00);
    const Operand s = resource_src(smp);
    const Operand ref = src(arg(tail));
    const Operand status = tex_status(inst.result);
    const u32 offsets = ext & kTexOffsetMask;
    if (opcode == 224) {
        const Operand lod = src(arg(tail + 1));
        emit_ext(body_, kOpTex, static_cast<u32>(TexOp::SampleCmpLevel) | offsets,
                 {&dx, &status, &c, &rr, &s, &ref, &lod});
        return true;
    }
    u32 gx = 0, gy = 0;
    usize clamp_at = 0;
    if (opcode == 254) {
        coords_vector({arg(tail + 1), arg(tail + 2), arg(tail + 3)}, gx);
        coords_vector({arg(tail + 4), arg(tail + 5), arg(tail + 6)}, gy);
        clamp_at = tail + 7;
    } else if (out_.stage == ShaderStage::Fragment) {
        const Operand bias = src(arg(tail + 1));
        u32 control = static_cast<u32>(TexOp::SampleCmpBias) | offsets;
        if (present(tail + 2)) {
            control |= kTexClamp;
            const Operand cl = src(arg(tail + 2));
            emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &bias, &cl});
        } else {
            emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &bias});
        }
        return true;
    } else if (out_.stage == ShaderStage::Compute) {
        // Compute quads: the implicit gradients scaled by 2^clamp(bias).
        if (quad_layout_ == QuadLayout::None) return malformed("derivative sampling without a quad layout");
        quad_gradients({arg(3), arg(4), arg(5)}, gx, gy);
        const Slot b = tmp();
        const Operand bd = dst_temp(b.reg, 1u << b.comp), bias = src(arg(tail + 1));
        const Operand lo = imm(0xC1800000u), hi = imm(0x417FD70Au);  // -16.0f, 15.99f
        const Operand bs = src_temp_scalar(b.reg, b.comp);
        body_.inst(op(Sm4Opcode::MAX), {&bd, &bias, &lo});
        body_.inst(op(Sm4Opcode::MIN), {&bd, &bs, &hi});
        body_.inst(op(Sm4Opcode::EXP), {&bd, &bs});
        const Operand gxd = dst_temp(gx, 0x7), gyd = dst_temp(gy, 0x7);
        const Operand gxs = src_temp(gx, 0xE4), gys = src_temp(gy, 0xE4);
        body_.inst(op(Sm4Opcode::MUL), {&gxd, &gxs, &bs});
        body_.inst(op(Sm4Opcode::MUL), {&gyd, &gys, &bs});
        clamp_at = tail + 2;
    } else {
        return unimplemented("SampleCmpBias outside the pixel and compute stages");
    }
    const Operand x = src_temp(gx, 0xE4), y = src_temp(gy, 0xE4);
    u32 control = static_cast<u32>(TexOp::SampleCmpGrad) | offsets;
    if (present(clamp_at)) {
        control |= kTexClamp;
        const Operand cl = src(arg(clamp_at));
        emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &x, &y, &cl});
    } else {
        emit_ext(body_, kOpTex, control, {&dx, &status, &c, &rr, &s, &ref, &x, &y});
    }
    return true;
}
