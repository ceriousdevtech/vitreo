// SPDX-License-Identifier: LGPL-2.1-only
// dxil_lower_ops.inc -- part of dxil_lower.cpp's Lowerer (included at the
// end of its anonymous namespace; see dxil_lower_members.inc): the dx.op
// coverage of DXIL.rst beyond the core set -- texture sampling with texel
// offsets and LOD clamps, gathers, multisample queries, LOD calculation,
// attribute evaluation, transcendentals, carry / wide integer ops, packed
// 8-bit dot products and pack / unpack, SM 6.6 binding handles -- and the
// ops refused because Vitreo reports their capability absent.
//
// Opcode numbers and argument orders are DXIL.rst's ("Instructions"
// table) and the dxc 1.6 listings of tools/oracle_probe/d3d12_sm6/
// d3d12_sm6_stages.hlsl (*.dxil.txt), which print each call's argument
// names.

namespace ops {

// Tokenized-program opcodes (d3d11TokenizedProgramFormat.hpp and the
// tiled-resources status forms, measured with fxc: dxbc_to_spirv_sm4.hpp).
constexpr u32 kSampleClS = 230, kSampleBClS = 231, kSampleDClS = 232, kSampleCClS = 233;
constexpr u32 kSampleControls = 1;  // D3D10_SB_EXTENDED_OPCODE_SAMPLE_CONTROLS
constexpr u32 kSampleInfoUint = 1u << 11;

// Polynomials fitted for Vitreo (weighted least squares on Chebyshev
// nodes, float64; worst float32 error 9.3e-8 for atan on [0, 1], 2.4e-7
// for acos on [0, 1]):
//   atan(t) = t * P(t^2), t in [0, 1]
//   acos(a) = sqrt(1 - a) * Q(a), a in [0, 1]
constexpr float kAtan[] = {0.999999986482697f, -0.3333309452705392f, 0.19993062185334814f,
                           -0.14207184803986808f, 0.10654848589477443f, -0.07534000436826367f,
                           0.04304286331386968f, -0.01628500459746515f, 0.0029040217649365352f};
constexpr float kAcos[] = {1.5707963049968978f, -0.21459880371072151f, 0.0889790471216302f,
                           -0.05017469727740179f, 0.03089300192830774f, -0.017089738320086895f,
                           0.0066712436976223005f, -0.0012628178820747252f};
constexpr float kHalfPi = 1.57079632679489662f;
constexpr float kPi = 3.14159265358979324f;
constexpr float kLog2E = 1.44269504088896341f;

u32 fbits(float f) {
    u32 b = 0;
    std::memcpy(&b, &f, 4);
    return b;
}

}  // namespace ops

Operand Lowerer::sampler_component(const Handle& h, u32 channel) {
    // A gather's sampler operand selects the channel (s#.select_component):
    // the resource operand's shape with the sampler type and select-1 mode.
    Operand o = resource(Sm4OperandType::Resource, h.res->id, h.reg, h.nonuniform, 0xE4);
    o.tokens[0] &= ~((0xFFu << 12) | (0xFFu << 4) | (3u << 2));
    o.tokens[0] |= (static_cast<u32>(Sm4OperandType::Sampler) << 12) | (2u << 2) | ((channel & 3u) << 4);
    return o;
}

void Lowerer::reduce_programmable_offsets(u32 reg, bool prefix) {
    const Operand d = dst_temp(reg, 0x7);
    const Operand v = src_temp(reg, 0xE4);
    const Operand k = imm(28);
    body_.inst(op(Sm4Opcode::ISHL), {&d, &v, &k});
    body_.inst(op(Sm4Opcode::ISHR), {&d, &v, &k});
    if (prefix) emit_ext(body_, graphics::dxbc_vitreo_ext::kOpTexOffset, 0, {&v});
}

bool Lowerer::texel_offsets(const DxilInstruction& inst, usize first, u32 count, u32& ext, bool& programmable,
                            u32& reg) {
    ext = 0;
    programmable = false;
    for (u32 k = 0; k < count; ++k) {
        const u32 id = inst.operands[first + k];
        if (undef(id)) continue;
        u32 v = 0;
        if (!const_u32(id, v)) {
            programmable = true;
            continue;
        }
        const i32 s = static_cast<i32>(v);
        if (s < -8 || s > 7) return malformed("an immediate texel offset outside [-8, 7]");
        // Sample controls: u bits 9-12, v 13-16, w 17-20 (4-bit signed).
        ext |= (v & 0xFu) << (9 + 4 * k);
    }
    if (programmable) {
        reg = fresh_vector();
        for (u32 k = 0; k < count; ++k) {
            const u32 id = inst.operands[first + k];
            const Operand d = dst_temp(reg, 1u << k);
            const Operand s = undef(id) ? imm(0) : src(id);
            body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        }
        ext = 0;
        return true;
    }
    if (ext != 0) ext |= ops::kSampleControls;
    return true;
}

bool Lowerer::status_used(u32 result) {
    if (status_scan_fn_ != fn_) {
        status_scan_fn_ = fn_;
        status_users_.clear();
        for (const DxilInstruction& i : fn_->instructions) {
            if (i.op == DxilOpcode::ExtractValue && i.indices.size() == 1 && i.indices[0] == 4) {
                status_users_.insert(i.operands[0]);
            }
        }
    }
    return status_users_.contains(result);
}

bool Lowerer::add_status(usize start, u32 result) {
    if (result == ~0u || !status_used(result)) return true;
    std::vector<u32>& code = body_.mutable_code();
    if (start >= code.size()) return malformed("no resource instruction to give a status");
    // The resource instruction is the last one emitted (coordinate moves
    // come first).
    for (usize pos = start; pos < code.size();) {
        const u32 length = (code[pos] >> 24) & 0x7Fu;
        if (length == 0) return malformed("an emitted instruction without a length");
        start = pos;
        pos += length;
    }
    const u32 t0 = code[start];
    const u32 base = t0 & 0x7FFu;
    usize at = start + 1;
    if ((t0 >> 31) != 0) {
        while (at < code.size() && (code[at++] >> 31) != 0) {}
    }
    // The destination: a temp with immediate indices.
    const u32 d0 = code[at];
    usize dlen = 1 + ((d0 >> 31) != 0 ? 1u : 0u);
    const u32 dims = (d0 >> 20) & 3u;
    for (u32 k = 0; k < dims; ++k) {
        if (((d0 >> (22 + 3 * k)) & 7u) != 0) return malformed("a resource destination with a relative index");
        ++dlen;
    }
    const usize after_dst = at + dlen;
    u32 form = 0;
    bool clamp = false, replace_null = false;
    switch (static_cast<Sm4Opcode>(base)) {
    case Sm4Opcode::SAMPLE: form = op(Sm4Opcode::SAMPLE_CL_S); clamp = true; break;
    case Sm4Opcode::SAMPLE_B: form = op(Sm4Opcode::SAMPLE_B_CL_S); clamp = true; break;
    case Sm4Opcode::SAMPLE_D: form = op(Sm4Opcode::SAMPLE_D_CL_S); clamp = true; break;
    case Sm4Opcode::SAMPLE_C: form = op(Sm4Opcode::SAMPLE_C_CL_S); clamp = true; break;
    case Sm4Opcode::SAMPLE_L: form = op(Sm4Opcode::SAMPLE_L_S); break;
    case Sm4Opcode::SAMPLE_C_LZ: form = op(Sm4Opcode::SAMPLE_C_LZ_S); break;
    case Sm4Opcode::LD: form = op(Sm4Opcode::LD_S); break;
    case Sm4Opcode::LD_MS: form = op(Sm4Opcode::LD_MS_S); break;
    case Sm4Opcode::LD_UAV_TYPED: form = op(Sm4Opcode::LD_UAV_TYPED_S); break;
    case Sm4Opcode::LD_RAW: form = op(Sm4Opcode::LD_RAW_S); break;
    case Sm4Opcode::LD_STRUCTURED: form = op(Sm4Opcode::LD_STRUCTURED_S); break;
    case Sm4Opcode::GATHER4: form = op(Sm4Opcode::GATHER4_S); break;
    case Sm4Opcode::GATHER4_C: form = op(Sm4Opcode::GATHER4_C_S); break;
    case Sm4Opcode::GATHER4_PO: form = op(Sm4Opcode::GATHER4_PO_S); break;
    case Sm4Opcode::GATHER4_PO_C: form = op(Sm4Opcode::GATHER4_PO_C_S); break;
    case Sm4Opcode::SAMPLE_CL_S:
    case Sm4Opcode::SAMPLE_B_CL_S:
    case Sm4Opcode::SAMPLE_D_CL_S:
    case Sm4Opcode::SAMPLE_C_CL_S: form = base; replace_null = true; break;
    default: return unimplemented("the resource status of opcode " + std::to_string(base));
    }
    const Slot s = fresh_scalar();
    status_slots_[result] = s;
    const Operand sd = dst_temp(s.reg, 1u << s.comp);
    std::vector<u32> out(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(after_dst));
    out.insert(out.end(), sd.tokens.begin(), sd.tokens.end());
    // A null status operand (one token) is replaced by the real one.
    const usize rest = replace_null ? after_dst + 1 : after_dst;
    out.insert(out.end(), code.begin() + static_cast<std::ptrdiff_t>(rest), code.end());
    if (clamp) {
        // MinLOD -FLT_MAX: the computed LOD is never raised.
        const Operand c = imm(0xFF7FFFFFu);
        out.insert(out.end(), c.tokens.begin(), c.tokens.end());
    }
    const usize length = out.size() - start;
    if (length > 127) return unimplemented("a status form longer than 127 tokens");
    out[start] = (t0 & ~(0x7FFu | (0x7Fu << 24))) | form | (static_cast<u32>(length) << 24);
    code = std::move(out);
    return true;
}

bool Lowerer::emit_sample(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    Handle h, smp;
    if (!handle_of(arg(1), h) || !handle_of(arg(2), smp)) return false;
    if ((opcode == 60 || opcode == 61 || opcode == 64) && out_.stage != ShaderStage::Fragment) {
        return unimplemented("implicit-derivative sampling outside the pixel stage");
    }
    u32 coords = 0;
    coords_vector({arg(3), arg(4), arg(5), arg(6)}, coords);
    u32 ext = 0, offset_reg = 0;
    bool programmable = false;
    if (!texel_offsets(inst, 8, 3, ext, programmable, offset_reg)) return false;
    // SM 6.7: the offsets are values; the sample below takes them.
    if (programmable) reduce_programmable_offsets(offset_reg, true);
    const usize tail = 10;  // the first argument after the offsets
    const auto present = [&](usize k) { return inst.operands.size() > 1 + k && !undef(arg(k)); };
    const u32 r = vec(inst.result);
    const Operand d = dst_temp(r, 0xF);
    const Operand c = src_temp(coords, 0xE4);
    const Operand res = resource_src(h);
    const Operand s = resource_src(smp);
    const Operand nul = null_operand();
    const auto emit = [&](u32 o, std::initializer_list<const Operand*> operands) {
        if (ext != 0) body_.inst_ext(o, ext, operands);
        else body_.inst(o, operands);
    };
    switch (opcode) {
    case 60:  // Sample(..., clamp)
        if (present(tail)) {
            const Operand cl = src(arg(tail));
            emit(ops::kSampleClS, {&d, &nul, &c, &res, &s, &cl});
        } else {
            emit(op(Sm4Opcode::SAMPLE), {&d, &c, &res, &s});
        }
        return true;
    case 61: {  // SampleBias(..., bias, clamp)
        const Operand b = src(arg(tail));
        if (present(tail + 1)) {
            const Operand cl = src(arg(tail + 1));
            emit(ops::kSampleBClS, {&d, &nul, &c, &res, &s, &b, &cl});
        } else {
            emit(op(Sm4Opcode::SAMPLE_B), {&d, &c, &res, &s, &b});
        }
        return true;
    }
    case 62: {  // SampleLevel(..., lod)
        const Operand l = src(arg(tail));
        emit(op(Sm4Opcode::SAMPLE_L), {&d, &c, &res, &s, &l});
        return true;
    }
    case 63: {  // SampleGrad(..., ddx0..2, ddy0..2, clamp)
        u32 dx = 0, dy = 0;
        coords_vector({arg(tail), arg(tail + 1), arg(tail + 2)}, dx);
        coords_vector({arg(tail + 3), arg(tail + 4), arg(tail + 5)}, dy);
        const Operand gx = src_temp(dx, 0xE4), gy = src_temp(dy, 0xE4);
        if (present(tail + 6)) {
            const Operand cl = src(arg(tail + 6));
            emit(ops::kSampleDClS, {&d, &nul, &c, &res, &s, &gx, &gy, &cl});
        } else {
            emit(op(Sm4Opcode::SAMPLE_D), {&d, &c, &res, &s, &gx, &gy});
        }
        return true;
    }
    case 64:    // SampleCmp(..., compare, clamp)
    case 65: {  // SampleCmpLevelZero(..., compare)
        const Operand ref = src(arg(tail));
        const Operand dx = dst_temp(r, 0x1);
        const Operand rr = resource_src(h, 0x00);
        if (opcode == 64 && present(tail + 1)) {
            const Operand cl = src(arg(tail + 1));
            emit(ops::kSampleCClS, {&dx, &nul, &c, &rr, &s, &ref, &cl});
        } else {
            emit(op(opcode == 64 ? Sm4Opcode::SAMPLE_C : Sm4Opcode::SAMPLE_C_LZ), {&dx, &c, &rr, &s, &ref});
        }
        return true;
    }
    }
    return malformed("not a sample op");
}

bool Lowerer::emit_gather(u32 opcode, const DxilInstruction& inst) {
    // TextureGather(srv, sampler, c0..c3, o0, o1, channel)
    // TextureGatherCmp(srv, sampler, c0..c3, o0, o1, channel, compare)
    // TextureGatherRaw(srv, sampler, c0..c3, o0, o1) (SM 6.7, 223)
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    Handle h, smp;
    if (!handle_of(arg(1), h) || !handle_of(arg(2), smp)) return false;
    // GatherRaw reads the view's one channel bit for bit (DirectX-Specs
    // "HLSL Advanced Texture Operations", Raw Gather: R16_UINT, R32_UINT,
    // or R32G32_UINT as uint64): a gather of red on the uint view, and of
    // green too for the 64-bit overload.
    const bool raw = opcode == 223;
    std::string overload;
    if (raw) {
        const DxilFunction* callee = m_.function_of(inst.operands[0]);
        overload = callee->name.substr(callee->name.rfind('.') + 1);
        if (overload != "i16" && overload != "i32" && overload != "i64") {
            return malformed("GatherRaw of " + overload);
        }
    }
    u32 channel = 0;
    if (!raw && (!const_u32(arg(9), channel) || channel > 3)) return malformed("non-constant gather channel");
    u32 coords = 0;
    coords_vector({arg(3), arg(4), arg(5), arg(6)}, coords);
    u32 ext = 0, offset_reg = 0;
    bool programmable = false;
    if (!texel_offsets(inst, 8, 2, ext, programmable, offset_reg)) return false;
    const bool cmp = opcode == 74;
    const u32 r = vec(inst.result);
    const Operand d = dst_temp(r, 0xF);
    const Operand c = src_temp(coords, 0xE4);
    const Operand res = resource_src(h);
    // A comparison gather compares the texture's first component whatever
    // channel names (measured, d3d12_sm6_stage_probe cs_tex: GatherCmpGreen
    // of an R32_FLOAT texture equals GatherCmp on WARP12 and the RTX 4080).
    const Operand s = sampler_component(smp, cmp ? 0u : channel);
    // A gather of `ch` (not a comparison) into register `into`.
    const auto gather_channel = [&](u32 into, u32 ch) {
        const Operand gd = dst_temp(into, 0xF);
        const Operand gs = sampler_component(smp, ch);
        if (programmable) {
            const Operand o = src_temp(offset_reg, 0xE4);
            body_.inst(op(Sm4Opcode::GATHER4_PO), {&gd, &c, &o, &res, &gs});
        } else if (ext != 0) {
            body_.inst_ext(op(Sm4Opcode::GATHER4), ext, {&gd, &c, &res, &gs});
        } else {
            body_.inst(op(Sm4Opcode::GATHER4), {&gd, &c, &res, &gs});
        }
    };
    if (raw) {
        if (overload == "i64") {
            // Element i is (red i, green i) of R32G32_UINT, low word first:
            // the aggregate's members are component pairs (dxbc_vitreo_ext
            // register model).
            const u32 hi = fresh_vector();
            gather_channel(r, 0);
            gather_channel(hi, 1);
            const u32 pair0 = fresh_vector(), pair1 = fresh_vector();
            const Operand p0 = dst_temp(pair0, 0xF), p1 = dst_temp(pair1, 0xF);
            const Operand a = src_temp(r, 0x50), b = src_temp(hi, 0x50);  // .xxyy
            const Operand a2 = src_temp(r, 0xFA), b2 = src_temp(hi, 0xFA);  // .zzww
            // (lo.x, lo.x, lo.y, lo.y), then hi into .yw (and .zw alike).
            body_.inst(op(Sm4Opcode::MOV), {&p0, &a});
            const Operand p0yw = dst_temp(pair0, 0xA);
            body_.inst(op(Sm4Opcode::MOV), {&p0yw, &b});
            body_.inst(op(Sm4Opcode::MOV), {&p1, &a2});
            const Operand p1yw = dst_temp(pair1, 0xA);
            body_.inst(op(Sm4Opcode::MOV), {&p1yw, &b2});
            ext_.aggregates[inst.result] = {RegSlot{pair0, 0}, RegSlot{pair0, 2}, RegSlot{pair1, 0},
                                             RegSlot{pair1, 2}};
            return true;
        }
        gather_channel(r, 0);
        if (overload == "i16") {
            ext_.aggregates[inst.result] = {RegSlot{r, 0}, RegSlot{r, 1}, RegSlot{r, 2}, RegSlot{r, 3}};
        }
        return true;
    }
    if (programmable) {
        const Operand o = src_temp(offset_reg, 0xE4);
        if (cmp) {
            const Operand ref = src(arg(10));
            body_.inst(op(Sm4Opcode::GATHER4_PO_C), {&d, &c, &o, &res, &s, &ref});
        } else {
            body_.inst(op(Sm4Opcode::GATHER4_PO), {&d, &c, &o, &res, &s});
        }
        return true;
    }
    if (cmp) {
        const Operand ref = src(arg(10));
        if (ext != 0) body_.inst_ext(op(Sm4Opcode::GATHER4_C), ext, {&d, &c, &res, &s, &ref});
        else body_.inst(op(Sm4Opcode::GATHER4_C), {&d, &c, &res, &s, &ref});
    } else {
        if (ext != 0) body_.inst_ext(op(Sm4Opcode::GATHER4), ext, {&d, &c, &res, &s});
        else body_.inst(op(Sm4Opcode::GATHER4), {&d, &c, &res, &s});
    }
    return true;
}

bool Lowerer::emit_texture_load(const DxilInstruction& inst) {
    // TextureLoad(srv, mipLevelOrSampleIndex, c0, c1, c2, o0, o1, o2)
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    Handle h;
    if (!handle_of(arg(1), h)) return false;
    const bool ms = h.res->shape == ResourceShape::Texture2DMS || h.res->shape == ResourceShape::Texture2DMSArray;
    const bool uav = h.res->cls == ResourceClass::UAV;
    u32 coords = 0;
    coords_vector({arg(3), arg(4), arg(5)}, coords, (ms || uav) ? ~0u : arg(2), 3);
    u32 ext = 0, offset_reg = 0;
    bool programmable = false;
    if (!texel_offsets(inst, 7, 3, ext, programmable, offset_reg)) return false;
    if (programmable && (h.res->cls == ResourceClass::UAV || h.res->shape == ResourceShape::TextureCube ||
                         h.res->shape == ResourceShape::TextureCubeArray)) {
        return malformed("a non-immediate texel offset on a UAV or cube load");
    }
    if (programmable) {
        // SM 6.7: ld's offset is added to the integer address before the
        // fetch (D3D11.3 22.4.6), so a value offset is that addition; the
        // array slice and the mip are not offset.
        reduce_programmable_offsets(offset_reg, false);
        const bool one_d = h.res->shape == ResourceShape::Texture1D || h.res->shape == ResourceShape::Texture1DArray;
        const bool three_d = h.res->shape == ResourceShape::Texture3D;
        const u32 axes = one_d ? 0x1u : three_d ? 0x7u : 0x3u;
        const Operand d = dst_temp(coords, axes);
        const Operand a = src_temp(coords, 0xE4);
        const Operand b = src_temp(offset_reg, 0xE4);
        body_.inst(op(Sm4Opcode::IADD), {&d, &a, &b});
    }
    const u32 r = vec(inst.result);
    const Operand d = dst_temp(r, 0xF);
    const Operand c = src_temp(coords, 0xE4);
    const Operand res = resource_src(h);
    if (uav) {
        if (ext != 0) return malformed("a texel offset on a UAV load");
        body_.inst(op(Sm4Opcode::LD_UAV_TYPED), {&d, &c, &res});
    } else if (ms) {
        const Operand s = src(arg(2));
        if (ext != 0) body_.inst_ext(op(Sm4Opcode::LD_MS), ext, {&d, &c, &res, &s});
        else body_.inst(op(Sm4Opcode::LD_MS), {&d, &c, &res, &s});
    } else if (ext != 0) {
        body_.inst_ext(op(Sm4Opcode::LD), ext, {&d, &c, &res});
    } else {
        body_.inst(op(Sm4Opcode::LD), {&d, &c, &res});
    }
    return true;
}

bool Lowerer::emit_dimensions(const DxilInstruction& inst) {
    // GetDimensions(handle, mip): {width, height, depth / elements, mips};
    // a multisample texture's fourth component is its sample count.
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    Handle h;
    if (!handle_of(arg(1), h)) return false;
    const u32 r = vec(inst.result);
    const Operand d = dst_temp(r, 0xF);
    const Operand res = resource_src(h);
    const ResourceShape s = h.res->shape;
    if (s == ResourceShape::TypedBuffer || s == ResourceShape::RawBuffer || s == ResourceShape::StructuredBuffer) {
        const Operand dx = dst_temp(r, 0x1);
        body_.inst(op(Sm4Opcode::BUFINFO), {&dx, &res});
        return true;
    }
    const Operand mip = undef(arg(2)) ? imm(0) : src(arg(2));
    body_.inst(op(Sm4Opcode::RESINFO) | kResinfoUint, {&d, &mip, &res});
    if (s == ResourceShape::Texture2DMS || s == ResourceShape::Texture2DMSArray) {
        const Operand dw = dst_temp(r, 0x8);
        const Operand rx = resource_src(h, 0x00);
        body_.inst(op(Sm4Opcode::SAMPLEINFO) | ops::kSampleInfoUint, {&dw, &rx});
    }
    return true;
}

bool Lowerer::emit_transcendental(u32 opcode, const DxilInstruction& inst) {
    using namespace ops;
    const Operand d = dst(inst.result);
    const Operand x = src(inst.operands[2]);
    const Operand ax = src(inst.operands[2], 2);  // |x|
    const auto t = [&]() { return tmp(); };
    const auto D = [](Slot s) { return dst_temp(s.reg, 1u << s.comp); };
    const auto S = [](Slot s, u32 mod = 0) { return src_temp_scalar(s.reg, s.comp, mod); };
    const auto horner = [&](const float* c, usize n, const Operand& z, Slot out) {
        const Operand od = D(out), os = S(out);
        const Operand top = imm(fbits(c[n - 1]));
        body_.inst(op(Sm4Opcode::MOV), {&od, &top});
        for (usize k = n - 1; k-- > 0;) {
            const Operand ck = imm(fbits(c[k]));
            body_.inst(op(Sm4Opcode::MAD), {&od, &os, &z, &ck});
        }
    };
    const auto exp_e = [&](const Operand& v, float scale, Slot out) {
        const Operand od = D(out), os = S(out);
        const Operand k = imm(fbits(scale * kLog2E));
        body_.inst(op(Sm4Opcode::MUL), {&od, &v, &k});
        body_.inst(op(Sm4Opcode::EXP), {&od, &os});
    };
    // sinh for |x| < 0.5: x (1 + z/6 + z^2/120 + z^3/5040), z = x^2.
    static constexpr float kSinh[] = {1.0f, 1.0f / 6.0f, 1.0f / 120.0f, 1.0f / 5040.0f};
    const Operand sign = imm(0x80000000u);
    switch (opcode) {
    case 14: {  // Tan = sin / cos
        const Slot s = t(), c = t();
        const Operand sd = D(s), cd = D(c), ss = S(s), cs = S(c);
        body_.inst(op(Sm4Opcode::SINCOS), {&sd, &cd, &x});
        body_.inst(op(Sm4Opcode::DIV), {&d, &ss, &cs});
        return true;
    }
    case 15:    // Acos
    case 16: {  // Asin
        const Slot q = t(), r = t(), neg = t();
        const Operand qd = D(q), qs = S(q), rd = D(r), rs = S(r);
        const Operand one = imm(fbits(1.0f));
        // r = 1 - |x| (add with -|x|)
        const Operand nax = src(inst.operands[2], 3);  // -|x|
        body_.inst(op(Sm4Opcode::ADD), {&rd, &one, &nax});
        body_.inst(op(Sm4Opcode::SQRT), {&rd, &rs});
        horner(kAcos, std::size(kAcos), ax, q);
        body_.inst(op(Sm4Opcode::MUL), {&rd, &rs, &qs});  // acos(|x|)
        const Operand nd = D(neg), ns = S(neg);
        const Operand zero = imm(0);
        body_.inst(op(Sm4Opcode::LT), {&nd, &x, &zero});
        if (opcode == 15) {
            // acos(-a) = pi - acos(a)
            const Operand pi = imm(fbits(kPi));
            const Operand nrs = S(r, 1);
            body_.inst(op(Sm4Opcode::ADD), {&qd, &pi, &nrs});
            body_.inst(op(Sm4Opcode::MOVC), {&d, &ns, &qs, &rs});
        } else {
            // asin(a) = pi/2 - acos(a), odd
            const Operand hp = imm(fbits(kHalfPi));
            const Operand nrs = S(r, 1);
            body_.inst(op(Sm4Opcode::ADD), {&qd, &hp, &nrs});
            body_.inst(op(Sm4Opcode::AND), {&nd, &x, &sign});
            body_.inst(op(Sm4Opcode::OR), {&d, &qs, &ns});
        }
        return true;
    }
    case 17: {  // Atan
        const Slot big = t(), inv = t(), tt = t(), z = t(), p = t(), s = t();
        const Operand one = imm(fbits(1.0f));
        const Operand bd = D(big), bs = S(big), id = D(inv), is = S(inv), td = D(tt), ts = S(tt);
        const Operand zd = D(z), zs = S(z), pd = D(p), ps = S(p), sd = D(s), ss = S(s);
        body_.inst(op(Sm4Opcode::LT), {&bd, &one, &ax});
        body_.inst(op(Sm4Opcode::DIV), {&id, &one, &ax});
        body_.inst(op(Sm4Opcode::MOVC), {&td, &bs, &is, &ax});
        body_.inst(op(Sm4Opcode::MUL), {&zd, &ts, &ts});
        horner(kAtan, std::size(kAtan), zs, p);
        body_.inst(op(Sm4Opcode::MUL), {&pd, &ps, &ts});
        const Operand hp = imm(fbits(kHalfPi));
        const Operand nps = S(p, 1);
        body_.inst(op(Sm4Opcode::ADD), {&sd, &hp, &nps});
        body_.inst(op(Sm4Opcode::MOVC), {&pd, &bs, &ss, &ps});
        body_.inst(op(Sm4Opcode::AND), {&sd, &x, &sign});
        body_.inst(op(Sm4Opcode::OR), {&d, &ps, &ss});
        return true;
    }
    case 18:    // Hcos
    case 19: {  // Hsin
        const Slot e = t(), en = t(), r = t();
        const Operand es = S(e), ens = S(en), nens = S(en, 1), rd = D(r), rs = S(r);
        exp_e(x, 1.0f, e);
        exp_e(x, -1.0f, en);
        const Operand half = imm(fbits(0.5f));
        body_.inst(op(Sm4Opcode::ADD), {&rd, &es, opcode == 18 ? &ens : &nens});
        if (opcode == 18) {
            body_.inst(op(Sm4Opcode::MUL), {&d, &rs, &half});
            return true;
        }
        body_.inst(op(Sm4Opcode::MUL), {&rd, &rs, &half});
        const Slot z = t(), p = t(), small = t();
        const Operand zd = D(z), zs = S(z), pd = D(p), ps = S(p), sd = D(small), ss = S(small);
        body_.inst(op(Sm4Opcode::MUL), {&zd, &x, &x});
        horner(kSinh, std::size(kSinh), zs, p);
        body_.inst(op(Sm4Opcode::MUL), {&pd, &ps, &x});
        const Operand lim = imm(fbits(0.5f));
        body_.inst(op(Sm4Opcode::LT), {&sd, &ax, &lim});
        body_.inst(op(Sm4Opcode::MOVC), {&d, &ss, &ps, &rs});
        return true;
    }
    case 20: {  // Htan: tanh(|x|) with x's sign
        const Slot e2 = t(), big = t(), z = t(), p = t(), e = t(), en = t(), small = t(), sel = t();
        const Operand e2d = D(e2), e2s = S(e2), bd = D(big), bs = S(big);
        const Operand one = imm(fbits(1.0f)), two = imm(fbits(2.0f));
        // big = 1 - 2 / (e^(2|x|) + 1)
        exp_e(ax, 2.0f, e2);
        body_.inst(op(Sm4Opcode::ADD), {&e2d, &e2s, &one});
        body_.inst(op(Sm4Opcode::DIV), {&bd, &two, &e2s});
        const Operand nbs = S(big, 1);
        body_.inst(op(Sm4Opcode::ADD), {&bd, &one, &nbs});
        // small = sinh(|x|) / cosh(|x|) from the series
        const Operand zd = D(z), zs = S(z), pd = D(p), ps = S(p);
        body_.inst(op(Sm4Opcode::MUL), {&zd, &ax, &ax});
        horner(kSinh, std::size(kSinh), zs, p);
        body_.inst(op(Sm4Opcode::MUL), {&pd, &ps, &ax});
        exp_e(ax, 1.0f, e);
        exp_e(ax, -1.0f, en);
        const Operand ed = D(e), es = S(e), ens = S(en);
        const Operand half = imm(fbits(0.5f));
        body_.inst(op(Sm4Opcode::ADD), {&ed, &es, &ens});
        body_.inst(op(Sm4Opcode::MUL), {&ed, &es, &half});
        const Operand smd = D(small), sms = S(small);
        body_.inst(op(Sm4Opcode::DIV), {&smd, &ps, &es});
        const Operand seld = D(sel), sels = S(sel);
        const Operand lim = imm(fbits(0.5f));
        body_.inst(op(Sm4Opcode::LT), {&seld, &ax, &lim});
        body_.inst(op(Sm4Opcode::MOVC), {&bd, &sels, &sms, &bs});
        body_.inst(op(Sm4Opcode::AND), {&seld, &x, &sign});
        body_.inst(op(Sm4Opcode::OR), {&d, &bs, &sels});
        return true;
    }
    }
    return malformed("not a transcendental");
}

bool Lowerer::emit_op_ext(u32 opcode, const DxilInstruction& inst, bool& handled) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    if (emit_stage_op(opcode, inst, handled) == false) return false;
    if (handled) return true;
    handled = true;
    const ShaderStage st = out_.stage;
    const Operand nul = null_operand();
    switch (opcode) {
    case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        return emit_transcendental(opcode, inst);
    case 41:    // IMul(a, b) = {hi, lo}
    case 42:    // UMul(a, b) = {hi, lo}
    case 43:    // UDiv(a, b) = {quotient, remainder}
    case 44:    // UAddc(a, b) = {sum, carry}
    case 45: {  // USubb(a, b) = {difference, borrow}
        const u32 r = vec(inst.result);
        const Operand dx = dst_temp(r, 0x1), dy = dst_temp(r, 0x2);
        const Operand a = src(arg(1)), b = src(arg(2));
        const Sm4Opcode o = opcode == 41 ? Sm4Opcode::IMUL
                            : opcode == 42 ? Sm4Opcode::UMUL
                            : opcode == 43 ? Sm4Opcode::UDIV
                            : opcode == 44 ? Sm4Opcode::UADDC
                                           : Sm4Opcode::USUBB;
        body_.inst(op(o), {&dx, &dy, &a, &b});
        return true;
    }
    case 50: {  // Msad(ref, src, accum)
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1)), b = src(arg(2)), c = src(arg(3));
        body_.inst(op(Sm4Opcode::MSAD), {&d, &a, &b, &c});
        return true;
    }
    case 60: case 61: case 62: case 63: case 64: case 65: {
        const usize at = body_.size();
        if (st == ShaderStage::Compute && (opcode == 60 || opcode == 61 || opcode == 64)) {
            bool status_done = false;
            if (!emit_quad_sample(opcode, inst, status_done)) return false;
            return status_done || add_status(at, inst.result);
        }
        return emit_sample(opcode, inst) && add_status(at, inst.result);
    }
    case 224: case 254: case 255:  // SampleCmpLevel, SampleCmpGrad, SampleCmpBias
        return emit_sample_cmp_ext(opcode, inst);
    case 66: {
        const usize at = body_.size();
        return emit_texture_load(inst) && add_status(at, inst.result);
    }
    case 71: {  // CheckAccessFullyMapped(status)
        const Operand d = dst(inst.result), s = src(arg(1));
        body_.inst(op(Sm4Opcode::CHECK_ACCESS_FULLY_MAPPED), {&d, &s});
        return true;
    }
    case 72: return emit_dimensions(inst);
    case 73: case 74: case 223: {
        const usize at = body_.size();
        return emit_gather(opcode, inst) && add_status(at, inst.result);
    }
    case 75: {  // Texture2DMSGetSamplePosition(srv, index) = {x, y}
        Handle h;
        if (!handle_of(arg(1), h)) return false;
        const u32 r = vec(inst.result);
        const Operand d = dst_temp(r, 0x3);
        const Operand res = resource_src(h);
        const Operand i = src(arg(2));
        body_.inst(op(Sm4Opcode::SAMPLEPOS), {&d, &res, &i});
        return true;
    }
    case 76: {  // RenderTargetGetSamplePosition(index) = {x, y}
        if (st != ShaderStage::Fragment) return malformed("RenderTargetGetSamplePosition outside the pixel stage");
        const u32 r = vec(inst.result);
        const Operand d = dst_temp(r, 0x3);
        const Operand ras = special_src(Sm4OperandType::Rasterizer, 0xE4);
        const Operand i = src(arg(1));
        body_.inst(op(Sm4Opcode::SAMPLEPOS), {&d, &ras, &i});
        return true;
    }
    case 77: {  // RenderTargetGetSampleCount
        if (st != ShaderStage::Fragment) return malformed("RenderTargetGetSampleCount outside the pixel stage");
        const Operand d = dst(inst.result);
        const Operand ras = special_src(Sm4OperandType::Rasterizer, 0x00);
        body_.inst(op(Sm4Opcode::SAMPLEINFO) | ops::kSampleInfoUint, {&d, &ras});
        return true;
    }
    case 81: {  // CalculateLOD(handle, sampler, c0, c1, c2, clamped)
        if (st == ShaderStage::Compute) {
            bool status_done = false;
            return emit_quad_sample(opcode, inst, status_done);
        }
        if (st != ShaderStage::Fragment) return unimplemented("CalculateLOD outside the pixel and compute stages");
        Handle h, smp;
        if (!handle_of(arg(1), h) || !handle_of(arg(2), smp)) return false;
        u32 clamped = 0;
        if (!const_u32(arg(6), clamped)) return malformed("non-constant CalculateLOD clamp");
        u32 coords = 0;
        coords_vector({arg(3), arg(4), arg(5)}, coords);
        const u32 r = fresh_vector();
        const Operand rd = dst_temp(r, 0x3);
        const Operand c = src_temp(coords, 0xE4);
        const Operand res = resource_src(h);
        const Operand s = resource_src(smp);
        body_.inst(op(Sm4Opcode::LOD), {&rd, &c, &res, &s});
        // (clamped, unclamped): the i1 is 0 / ~0 here.
        slots_[inst.result] = Slot{r, clamped != 0 ? 0u : 1u};
        return true;
    }
    case 87:    // EvalSnapped(sigId, row, col, offsetX, offsetY)
    case 88:    // EvalSampleIndex(sigId, row, col, sampleIndex)
    case 89: {  // EvalCentroid(sigId, row, col)
        if (st != ShaderStage::Fragment) return malformed("attribute evaluation outside the pixel stage");
        u32 id = 0;
        if (!const_u32(arg(1), id)) return malformed("non-constant signature element");
        Operand extra = imm(0);
        if (opcode == 87) {
            u32 offset = 0;
            coords_vector({arg(4), arg(5)}, offset);
            extra = src_temp(offset, 0xE4);
        } else if (opcode == 88) {
            extra = src(arg(4));
        }
        const std::function<void(const Operand&, const Operand&)> emit = [&](const Operand& d, const Operand& v) {
            if (opcode == 89) body_.inst(op(Sm4Opcode::EVAL_CENTROID), {&d, &v});
            else body_.inst(op(opcode == 87 ? Sm4Opcode::EVAL_SNAPPED : Sm4Opcode::EVAL_SAMPLE_INDEX), {&d, &v, &extra});
        };
        return load_signature(sig_in_, Sm4OperandType::Input, id, arg(2), arg(3), nullptr, inst.result, &emit);
    }
    case 90: {  // SampleIndex: an SM 5.1 register (dcl_input_ps_sgv sampleIndex)
        if (st != ShaderStage::Fragment) return malformed("SampleIndex outside the pixel stage");
        SigEntry* e = nullptr;
        for (SigEntry& s : entries_in_) {
            if (s.system_value == kNameSampleIndex) e = &s;
        }
        if (e == nullptr) {
            entries_in_.push_back(SigEntry{"SV_SampleIndex", 0, kNameSampleIndex, 1, ~0u, 0, 0, 0, 0});
            e = &entries_in_.back();
        }
        if (e->reg == ~0u) {
            u32 next = 0;
            for (const SigEntry& s : entries_in_) {
                if (s.reg != ~0u) next = std::max(next, s.reg + 1);
            }
            e->reg = next;
            e->mask = 0x1;
            e->rw_mask = 0x1;
        }
        const Operand d = dst(inst.result);
        const Operand v = stages::reg1(Sm4OperandType::Input, Index{false, e->reg, 0, 0}, 0x00, false);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case 91: {  // Coverage
        if (st != ShaderStage::Fragment) return malformed("Coverage outside the pixel stage");
        system_inputs_.insert(static_cast<u32>(Sm4OperandType::InputCoverageMask));
        const Operand d = dst(inst.result);
        const Operand v = stages::scalar_input(Sm4OperandType::InputCoverageMask);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case 92: {  // InnerCoverage: vInnerCoverage (conservative rasterization tier 3)
        if (st != ShaderStage::Fragment) return malformed("InnerCoverage outside the pixel stage");
        system_inputs_.insert(static_cast<u32>(Sm4OperandType::InnerCoverage));
        const Operand d = dst(inst.result);
        const Operand v = stages::scalar_input(Sm4OperandType::InnerCoverage);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case 109:
        return unimplemented("CycleCounterLegacy");
    case 225:  // TextureStoreSample(uav, c0..c2, v0..v3, mask, sampleIdx) (SM 6.7)
        // RWTexture2DMS / RWTexture2DMSArray writes. Metal's multisampled
        // textures are read-only in every shader (MSL specification 2.9:
        // texture2d_ms / texture2d_ms_array take access::read only, and 6.12
        // gives them read functions alone), so a sample cannot be stored;
        // WriteableMSAATexturesSupported stays FALSE (d3d12_caps.cpp).
        return unimplemented("TextureStoreSample (SM 6.7 writable MSAA textures: Metal multisampled textures are "
                             "read-only)");
    case 256:    // StartVertexLocation() (SM 6.8)
    case 257: {  // StartInstanceLocation()
        if (st != ShaderStage::Vertex) return malformed("StartVertexLocation / StartInstanceLocation outside the vertex stage");
        const Operand d = dst(inst.result);
        emit_ext(body_, graphics::dxbc_vitreo_ext::kOpDrawParam, opcode == 256 ? 0u : 1u, {&d});
        return true;
    }
    case 137: {  // AttributeAtVertex(inputSigId, row, col, VertexID)
        // The attribute at vertex VertexID (0-2, the SV_Barycentrics order)
        // of the pixel's triangle: VX_BARY AttributeAtVertex reads it from
        // the per-vertex copies the pass-through before the pixel shader
        // delivers (dxbc_to_spirv_vitreo_bary.cpp). The vertex index may be
        // a value.
        if (st != ShaderStage::Fragment) return malformed("AttributeAtVertex outside the pixel stage");
        u32 id = 0;
        if (!const_u32(arg(1), id)) return malformed("non-constant signature element");
        const Operand vertex = src(arg(4));
        const std::function<void(const Operand&, const Operand&)> emit = [&](const Operand& d, const Operand& v) {
            emit_ext(body_, graphics::dxbc_vitreo_ext::kOpBary,
                     static_cast<u32>(graphics::dxbc_vitreo_ext::BaryOp::AttributeAtVertex), {&d, &v, &vertex});
        };
        return load_signature(sig_in_, Sm4OperandType::Input, id, arg(2), arg(3), nullptr, inst.result, &emit);
    }
    case 138: {  // ViewID(): the current view instance (SM 6.1)
        if (st == ShaderStage::Compute) return malformed("ViewID in a compute shader");
        const Operand d = dst(inst.result);
        emit_ext(body_, graphics::dxbc_vitreo_ext::kOpViewId, 0, {&d});
        return true;
    }
    case 162: {  // Dot2AddHalf(acc, ax, ay, bx, by): acc + ax * bx + ay * by
        // A half product is exact in float32 (11 + 11 significand bits), so
        // each mad rounds only its sum; the operation is not fused (SM 6.4:
        // fp16 overflow of the products need not produce INF).
        const Slot t = tmp();
        const Operand td = dst_temp(t.reg, 1u << t.comp), ts = src_temp_scalar(t.reg, t.comp);
        const Operand acc = src(arg(1)), ax = src(arg(2)), ay = src(arg(3)), bx = src(arg(4)), by = src(arg(5));
        body_.inst(op(Sm4Opcode::MAD), {&td, &ax, &bx, &acc});
        const Operand d = dst(inst.result);
        body_.inst(op(Sm4Opcode::MAD), {&d, &ay, &by, &ts});
        return true;
    }
    case 163:    // Dot4AddI8Packed(acc, a, b)
    case 164: {  // Dot4AddU8Packed(acc, a, b)
        const bool sgn = opcode == 163;
        const Operand a0 = src(arg(1)), a = src(arg(2)), b = src(arg(3));
        const Operand d = dst(inst.result);
        emit_ext(body_, graphics::dxbc_vitreo_ext::kOpPackedDot,
                 sgn ? 0u : 1u, {&d, &a0, &a, &b});
        return true;
    }
    case 219: {  // Unpack4x8: unsigned or signed bytes, to i32 or native i16
        u32 mode = 0;
        if (!const_u32(arg(1), mode) || mode > 1) return malformed("bad unpack mode");
        const auto& result_type = type(inst.type);
        if (result_type.kind != DxilTypeKind::Struct || result_type.members.size() != 4)
            return malformed("bad unpack result type");
        const bool narrow = is_int(result_type.members[0], 16);
        for (u32 member : result_type.members) {
            if (!is_int(member, narrow ? 16u : 32u)) return malformed("bad unpack lane type");
        }
        const Operand d = dst_temp(vec(inst.result), 0xFu), p = src(arg(2));
        emit_ext(body_, graphics::dxbc_vitreo_ext::kOpUnpackBytes,
                 mode | (narrow ? 0x10u : 0u), {&d, &p});
        return true;
    }
    case 220: {  // Pack4x8: truncate or clamp signed input, from i32 or i16
        u32 mode = 0;
        if (!const_u32(arg(1), mode) || mode > 2) return malformed("bad pack mode");
        const bool narrow = is_int(m_.type_of(fn_, arg(2)), 16);
        for (u32 k = 2; k < 6; ++k) {
            if (!is_int(m_.type_of(fn_, arg(k)), narrow ? 16u : 32u))
                return malformed("bad pack lane type");
        }
        const Operand d = dst(inst.result), x = src(arg(2)), y = src(arg(3)),
                      z = src(arg(4)), w = src(arg(5));
        emit_ext(body_, graphics::dxbc_vitreo_ext::kOpPackBytes,
                 mode | (narrow ? 0x10u : 0u), {&d, &x, &y, &z, &w});
        return true;
    }
    case 178: case 179: case 180: case 181: case 182: case 183: case 184: case 185:
    case 186: case 187: case 188: case 189: case 190: case 191: case 192: case 193:
    case 194: case 195: case 196: case 197: case 198: case 199: case 200: case 201:
    case 202: case 203: case 204: case 205: case 206: case 207: case 208: case 209:
    case 210: case 211: case 212: case 214: case 215:
        return emit_ray_query(opcode, inst);
    case 216: {  // AnnotateHandle(handle, properties): the same binding
        if (const auto pending = heap_handles_.find(arg(1)); pending != heap_handles_.end()) {
            // A heap handle's resource type comes only from here.
            const DxilConstant* props = m_.constant(fn_, arg(2));
            u64 p0 = 0, p1 = 0;
            if (props == nullptr || props->kind != DxilConstantKind::Aggregate || props->elements.size() < 2 ||
                !m_.constant_int(fn_, static_cast<u32>(props->elements[0]), p0) ||
                !m_.constant_int(fn_, static_cast<u32>(props->elements[1]), p1)) {
                return malformed("annotateHandle of a heap handle without constant resource properties");
            }
            Handle h;
            if (!heap_resource(pending->second, static_cast<u32>(p0), static_cast<u32>(p1), h)) return false;
            handles_[inst.result] = h;
            return true;
        }
        Handle h;
        if (!handle_of(arg(1), h)) return false;
        handles_[inst.result] = h;
        return true;
    }
    case 218: {  // CreateHandleFromHeap(index, samplerHeap, nonUniformIndex)
        u64 sampler = 0, nonuniform = 0;
        if (!m_.constant_int(fn_, arg(2), sampler) || !m_.constant_int(fn_, arg(3), nonuniform)) {
            return malformed("createHandleFromHeap with a non-constant heap or uniformity flag");
        }
        heap_handles_[inst.result] = HeapHandle{arg(1), (sampler & 1u) != 0, (nonuniform & 1u) != 0};
        return true;
    }
    case 217: {  // CreateHandleFromBinding({lower, upper, space, class}, index, nonUniform)
        const DxilConstant* bind = m_.constant(fn_, arg(1));
        // dxc writes the all-zero binding (SRV t0 space0) as
        // zeroinitializer, a null constant (LLVM LangRef 3.7).
        const bool zero = bind != nullptr && bind->kind == DxilConstantKind::Null;
        if (bind == nullptr || (!zero && (bind->kind != DxilConstantKind::Aggregate || bind->elements.size() < 4))) {
            return malformed("createHandleFromBinding without a constant binding");
        }
        u64 lower = 0, space = 0, cls = 0;
        if (!zero) {
            m_.constant_int(fn_, static_cast<u32>(bind->elements[0]), lower);
            m_.constant_int(fn_, static_cast<u32>(bind->elements[2]), space);
            m_.constant_int(fn_, static_cast<u32>(bind->elements[3]), cls);
        }
        Handle h;
        for (const Resource& r : resources_) {
            if (static_cast<u32>(r.cls) == cls && r.space == space && r.lower == lower) h.res = &r;
        }
        if (h.res == nullptr) return malformed("createHandleFromBinding names no resource record");
        h.reg = index_of(arg(2));
        u64 nonuniform = 0;
        m_.constant_int(fn_, arg(3), nonuniform);
        h.nonuniform = (nonuniform & 1u) != 0;
        handles_[inst.result] = h;
        (void)nul;
        return true;
    }
    default:
        handled = false;
        return true;
    }
}

// SM 6.6 dynamic resources. The resource properties of annotateHandle, as
// dxc 1.6 writes them (DXIL.rst names the operation, not its encoding; read
// from dxc -Fc listings, whose comments spell each resource out, of every
// resource type: tools/oracle_probe/d3d12_dynres):
//   word 0: bits 0-7 the resource kind (DXIL.rst ResourceKind), bit 12 UAV,
//           bit 13 rasterizer ordered, bit 14 globally coherent, bit 15 a
//           UAV's hidden counter or a sampler's comparison mode;
//   word 1: typed views and textures: the component type (bits 0-7) and
//           count (bits 8-15); structured buffers the stride; constant
//           buffers the size in bytes.
bool Lowerer::heap_resource(const HeapHandle& hh, u32 p0, u32 p1, Handle& out) {
    const u32 kind = p0 & 0xFFu;
    Resource r;
    if (kind == 14) {
        if (!hh.sampler) return malformed("a sampler annotated on a CBV/SRV/UAV heap handle");
        r.cls = ResourceClass::Sampler;
        r.sampler_kind = (p0 & (1u << 15)) != 0 ? 1u : 0u;  // D3D10_SB_SAMPLER_MODE_COMPARISON
    } else {
        if (hh.sampler) return malformed("a resource annotated on a sampler heap handle");
        if (kind == 13) {
            r.cls = ResourceClass::CBV;
            r.cb_bytes = p1;
        } else if (kind >= 1 && kind <= 12) {
            r.cls = (p0 & (1u << 12)) != 0 ? ResourceClass::UAV : ResourceClass::SRV;
            r.shape = static_cast<ResourceShape>(kind);
            if (r.shape == ResourceShape::StructuredBuffer) {
                r.stride = p1;
            } else if (r.shape != ResourceShape::RawBuffer) {
                r.element_type = p1 & 0xFFu;
            }
            if (r.cls == ResourceClass::UAV) {
                r.rov = (p0 & (1u << 13)) != 0;
                r.coherent = (p0 & (1u << 14)) != 0;
                r.counter = (p0 & (1u << 15)) != 0;
                if (r.counter) return unimplemented("a heap-indexed UAV with a counter (IncrementCounter)");
            }
        } else {
            // 15 TBuffer, 16 RTAccelerationStructure, 17/18 FeedbackTexture2D[Array].
            return unimplemented("heap-indexed resource kind " + std::to_string(kind));
        }
    }
    const Resource* found = heap_record(r, hh.sampler);
    out.res = found;
    out.reg = index_of(hh.index, found->lower);
    out.nonuniform = hh.nonuniform;
    return true;
}

const Resource* Lowerer::heap_record(Resource r, bool sampler) {
    r.space = sampler ? graphics::kDxbcSamplerHeapSpace : graphics::kDxbcResourceHeapSpace;
    r.upper = kDxbcUnboundedRange;
    r.global = ~0u;
    r.rt_local = false;
    r.name.clear();
    // A megakernel's copies share one list (RtHeapResources).
    std::deque<Resource>& list = rt_heap_ != nullptr ? rt_heap_->list : heap_resources_;
    const Resource* found = nullptr;
    u32 same_class = 0;
    for (const Resource& e : list) {
        if (e.cls != r.cls) continue;
        ++same_class;
        if (e.shape == r.shape && e.element_type == r.element_type && e.stride == r.stride &&
            e.cb_bytes == r.cb_bytes && e.sampler_kind == r.sampler_kind && e.rov == r.rov &&
            e.coherent == r.coherent) {
            found = &e;
        }
    }
    if (found == nullptr) {
        // A range id after every declared one of the class, and its own
        // lower bound in the heap space (graphics::kDxbcResourceHeapSpace:
        // one per resource type; element e is heap slot e).
        u32 id = 0;
        if (rt_heap_ != nullptr) {
            id = rt_heap_->base[static_cast<u32>(r.cls)];
        } else {
            for (const Resource& e : resources_) {
                if (e.cls == r.cls) id = std::max(id, e.id + 1u);
            }
        }
        r.id = id + same_class;
        r.lower = same_class;
        list.push_back(r);
        found = &list.back();
    }
    return found;
}
