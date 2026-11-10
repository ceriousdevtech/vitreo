// SPDX-License-Identifier: LGPL-2.1-only
// dxil_lower_stages.inc -- part of dxil_lower.cpp's Lowerer (included at the
// end of its anonymous namespace; see dxil_lower_members.inc): the geometry,
// hull and domain stages, signature access (arrayed, dynamically indexed),
// stage system values and the RDEF chunk.
//
// Shapes follow what fxc 10.1 emits for the same HLSL as shader model 5.1
// (tools/oracle_probe/d3d12_sm6/d3d12_sm6_stages.hlsl compiled both ways):
//  - geometry: dcl_gsinstances, dcl_inputprimitive, dcl_stream m#, the
//    stream's outputs, dcl_outputtopology, dcl_maxout; inputs v[n][r];
//  - hull: hs_decls with the control point counts and tessellator state,
//    then hs_control_point_phase (inputs v[n][r], outputs o#) and the
//    patch-constant function as one hs_fork_phase (inputs vicp[n][r] and
//    vocp[n][r], the patch constants written to o#);
//  - domain: dcl_input_control_point_count, dcl_tessellator_domain, inputs
//    vicp[n][r], vpc# and vDomain.
// Enum values (DXIL.rst InputPrimitive, PrimitiveTopology,
// TessellatorDomain / Partitioning / OutputPrimitive) are the D3D10/11
// tokenized-program values one for one.

namespace stages {

// Declaration and stream opcodes (D3D10_SB_OPCODE_TYPE / D3D11).
constexpr u32 kDclIndexRange = 91, kDclGsOutputTopology = 92, kDclGsInputPrimitive = 93,
              kDclMaxOutputVertexCount = 94, kEmitStream = 117, kCutStream = 118,
              kEmitThenCutStream = 119, kHsDecls = 113, kHsControlPointPhase = 114,
              kHsForkPhase = 115, kDclStream = 143, kDclInputCpCount = 147, kDclOutputCpCount = 148,
              kDclTessDomain = 149, kDclTessPartitioning = 150, kDclTessOutputPrimitive = 151,
              kDclHsMaxTessFactor = 152, kDclGsInstanceCount = 206, kDclOutputSgv = 102;
// D3D_NAME values of the tessellation factors (finalQuadEdge 11 ..
// finalLineDensity 16).
constexpr u32 kNameFirstTessFactor = 11, kNameLastTessFactor = 16;

void put_index(Operand& o, u32 dim, const Index& index) {
    if (!index.relative) {
        o.tokens.push_back(index.immediate);
        return;
    }
    // D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE, then r#.c (select-1).
    o.tokens[0] |= 3u << (22 + 3 * dim);
    o.tokens.push_back(index.immediate);
    o.tokens.push_back(2u | (2u << 2) | (index.comp << 4) | (1u << 20));
    o.tokens.push_back(index.reg);
}

/// A 4-component register operand with one index ([reg], maybe relative):
/// a mask (destination / declaration) or a swizzle.
Operand reg1(Sm4OperandType type, const Index& reg, u32 bits, bool destination) {
    Operand o;
    o.tokens.push_back(2u | ((destination ? 0u : 1u) << 2) | (bits << 4) |
                       (static_cast<u32>(type) << 12) | (1u << 20));
    put_index(o, 0, reg);
    return o;
}

/// v[vertex][reg], vicp[cp][reg], vocp[cp][reg].
Operand reg2(Sm4OperandType type, const Index& first, u32 reg, u32 bits, bool destination) {
    Operand o;
    o.tokens.push_back(2u | ((destination ? 0u : 1u) << 2) | (bits << 4) |
                       (static_cast<u32>(type) << 12) | (2u << 20));
    put_index(o, 0, first);
    o.tokens.push_back(reg);
    return o;
}

/// A one-component register with no index (vPrim, vGSInstanceID,
/// vOutputControlPointID, vCoverage).
Operand scalar_input(Sm4OperandType type) {
    Operand o;
    o.tokens.push_back(1u | (static_cast<u32>(type) << 12));
    return o;
}

Operand stream_operand(u32 stream) {
    Operand o;
    o.tokens.push_back((static_cast<u32>(Sm4OperandType::Stream) << 12) | (1u << 20));
    o.tokens.push_back(stream);
    return o;
}

bool is_tess_factor(u32 name) { return name >= kNameFirstTessFactor && name <= kNameLastTessFactor; }

}  // namespace stages

bool Lowerer::read_stage_property(u64 tag, i64 value) {
    const DxilMetadata* n = m_.md(value);
    const auto field = [&](u32 k) {
        u64 v = 0;
        if (n != nullptr && k < n->operands.size()) m_.md_int(n->operands[k], v);
        return static_cast<u32>(v);
    };
    if (tag == 1) {  // kDxilGSStateTag
        if (n == nullptr || n->operands.size() < 5) return malformed("malformed geometry shader state");
        stage_.have_gs = true;
        stage_.gs_input_primitive = field(0);
        stage_.gs_max_vertices = field(1);
        stage_.gs_stream_mask = field(2);
        stage_.gs_topology = field(3);
        stage_.gs_instances = field(4);
    } else if (tag == 2) {  // kDxilDSStateTag
        if (n == nullptr || n->operands.size() < 2) return malformed("malformed domain shader state");
        stage_.have_ds = true;
        stage_.domain = field(0);
        stage_.input_cp = field(1);
    } else if (tag == 3) {  // kDxilHSStateTag
        if (n == nullptr || n->operands.size() < 7) return malformed("malformed hull shader state");
        stage_.have_hs = true;
        const DxilMetadata* f = m_.md(n->operands[0]);
        if (f == nullptr || f->kind != DxilMetadata::Kind::Value) {
            return malformed("hull shader without a patch-constant function");
        }
        stage_.patch_constant_fn = m_.function_of(f->value);
        stage_.input_cp = field(1);
        stage_.output_cp = field(2);
        stage_.domain = field(3);
        stage_.partitioning = field(4);
        stage_.output_primitive = field(5);
        const DxilMetadata* mf = m_.md(n->operands[6]);
        if (mf != nullptr && mf->kind == DxilMetadata::Kind::Value) {
            if (const DxilConstant* c = m_.constant(nullptr, mf->value)) {
                stage_.max_tess_factor_bits = static_cast<u32>(c->bits);
            }
        }
    }
    return true;
}

bool Lowerer::check_stage_state() {
    switch (out_.stage) {
    case ShaderStage::Geometry:
        if (!stage_.have_gs) return malformed("geometry shader without its state");
        if (stage_.gs_instances == 0 || stage_.gs_instances > 32) {
            return malformed("geometry instance count out of range");
        }
        if (stage_.gs_stream_mask == 0) stage_.gs_stream_mask = 1;
        return true;
    case ShaderStage::Domain:
        if (!stage_.have_ds) return malformed("domain shader without its state");
        return true;
    case ShaderStage::Hull:
        if (!stage_.have_hs) return malformed("hull shader without its state");
        if (stage_.patch_constant_fn == nullptr || stage_.patch_constant_fn->is_declaration) {
            return malformed("hull shader patch-constant function has no body");
        }
        return true;
    default:
        return true;
    }
}

u32 Lowerer::gs_input_vertices() const {
    // D3D10_SB_PRIMITIVE: point 1, line 2, triangle 3, line_adj 6,
    // triangle_adj 7, patch k (k = 1..32) 7 + k.
    const u32 p = stage_.gs_input_primitive;
    switch (p) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 6: return 4;
    case 7: return 6;
    default: return p >= 8 && p <= 39 ? p - 7 : 0;
    }
}

bool Lowerer::declare_stage() {
    using namespace stages;
    if (out_.stage == ShaderStage::Geometry) {
        if (gs_input_vertices() == 0) {
            return malformed("geometry input primitive " + std::to_string(stage_.gs_input_primitive));
        }
        if (stage_.gs_instances > 1) decls_.inst(kDclGsInstanceCount, {}, {stage_.gs_instances});
        decls_.inst(kDclGsInputPrimitive | (stage_.gs_input_primitive << 11), {});
        if (stage_.gs_topology != 0) decls_.inst(kDclGsOutputTopology | (stage_.gs_topology << 11), {});
        decls_.inst(kDclMaxOutputVertexCount, {}, {stage_.gs_max_vertices});
    } else if (out_.stage == ShaderStage::Domain) {
        decls_.inst(kDclInputCpCount | (stage_.input_cp << 11), {});
        decls_.inst(kDclTessDomain | (stage_.domain << 11), {});
    }
    return true;
}

bool Lowerer::declare_stage_signatures() {
    using namespace stages;
    const auto input_decl = [&](const Operand& v, u32 name) {
        if (name == 0) {
            decls_.inst(kDclInput, {&v});
        } else {
            decls_.inst(kDclInputSiv, {&v}, {name});
        }
    };
    const auto output_decl = [&](const SigEntry& e) {
        const Operand o = stages::reg1(Sm4OperandType::Output, Index{false, e.reg, 0, 0}, e.mask, true);
        if (e.system_value == 0) {
            decls_.inst(kDclOutput, {&o});
        } else if (e.system_value == kNamePrimitiveId) {
            decls_.inst(kDclOutputSgv, {&o}, {e.system_value});
        } else {
            decls_.inst(kDclOutputSiv, {&o}, {e.system_value});
        }
    };
    switch (out_.stage) {
    case ShaderStage::Geometry: {
        const u32 n = gs_input_vertices();
        for (const SigEntry& e : entries_in_) {
            if (e.reg == ~0u) continue;
            input_decl(reg2(Sm4OperandType::Input, Index{false, n, 0, 0}, e.reg, e.mask, true), e.system_value);
        }
        // Each stream's outputs follow its dcl_stream (fxc order).
        for (u32 s = 0; s < 4; ++s) {
            if ((stage_.gs_stream_mask & (1u << s)) == 0) continue;
            const Operand m = stream_operand(s);
            decls_.inst(kDclStream, {&m});
            for (const SigEntry& e : entries_out_) {
                if (e.stream == s && e.reg != ~0u) output_decl(e);
            }
        }
        return true;
    }
    case ShaderStage::Hull:
        if (!stage_.fork) {
            for (const SigEntry& e : entries_in_) {
                if (e.reg == ~0u) continue;
                input_decl(reg2(Sm4OperandType::Input, Index{false, stage_.input_cp, 0, 0}, e.reg, e.mask, true),
                           e.system_value);
            }
            for (const SigEntry& e : entries_out_) {
                if (e.reg != ~0u) output_decl(e);
            }
            return true;
        }
        for (const SigEntry& e : entries_in_) {
            if (e.reg == ~0u) continue;
            const Operand v = reg2(Sm4OperandType::InputControlPoint, Index{false, stage_.input_cp, 0, 0}, e.reg,
                                   e.mask, true);
            decls_.inst(kDclInput, {&v});
        }
        if (stage_.output_cp != 0) {
            for (const SigEntry& e : entries_out_) {
                if (e.reg == ~0u) continue;
                const Operand v = reg2(Sm4OperandType::OutputControlPoint, Index{false, stage_.output_cp, 0, 0},
                                       e.reg, e.mask, true);
                decls_.inst(kDclInput, {&v});
            }
        }
        for (const SigEntry& e : entries_pc_) {
            if (e.reg != ~0u) output_decl(e);
        }
        return true;
    case ShaderStage::Domain:
        for (const SigEntry& e : entries_in_) {
            if (e.reg == ~0u) continue;
            const Operand v = reg2(Sm4OperandType::InputControlPoint, Index{false, stage_.input_cp, 0, 0}, e.reg,
                                   e.mask, true);
            decls_.inst(kDclInput, {&v});
        }
        for (const SigEntry& e : entries_pc_) {
            if (e.reg == ~0u) continue;
            const Operand v = reg1(Sm4OperandType::InputPatchConstant, Index{false, e.reg, 0, 0}, e.mask, true);
            input_decl(v, is_tess_factor(e.system_value) ? e.system_value : 0u);
        }
        for (const SigEntry& e : entries_out_) {
            if (e.reg != ~0u) output_decl(e);
        }
        return true;
    default:
        return true;
    }
}

bool Lowerer::load_signature(const std::vector<SigElement>& sig, Sm4OperandType type, u32 id, u32 row_id,
                             u32 col_id, const Index* vertex, u32 result,
                             const std::function<void(const Operand&, const Operand&)>* emit) {
    const SigElement* e = nullptr;
    for (const SigElement& s : sig) {
        if (s.id == id) e = &s;
    }
    if (e == nullptr) return malformed("a signature access names no element");
    if (e->start_row < 0) return unimplemented("a signature element with no register (" + e->name + ")");
    u32 col = 0;
    if (!const_u32(col_id, col)) return unimplemented("a dynamically indexed signature column");
    const u32 comp = e->start_col + col;
    const u32 base = static_cast<u32>(e->start_row);
    const auto operand = [&](u32 reg) {
        if (vertex != nullptr) return stages::reg2(type, *vertex, reg, swizzle_of(comp), false);
        return stages::reg1(type, Index{false, reg, 0, 0}, swizzle_of(comp), false);
    };
    const auto move = [&](const Operand& d, const Operand& s) {
        if (emit != nullptr) (*emit)(d, s);
        else body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    };
    const Operand d = dst(result);
    if (auto row = constant_bits(row_id)) {
        if (*row >= e->rows) return malformed("signature row out of range");
        move(d, operand(base + *row));
        return true;
    }
    // A dynamic row: the element's rows, selected (rows outside the
    // element read row 0, which D3D leaves undefined).
    const Slot r = slot(row_id);
    const Slot v = tmp(), t = tmp();
    const Operand vd = dst_temp(v.reg, 1u << v.comp), vs = src_temp_scalar(v.reg, v.comp);
    const Operand td = dst_temp(t.reg, 1u << t.comp), ts = src_temp_scalar(t.reg, t.comp);
    const Operand rs = src_temp_scalar(r.reg, r.comp);
    move(d, operand(base));
    for (u32 k = 1; k < e->rows; ++k) {
        move(vd, operand(base + k));
        const Operand kk = imm(k);
        body_.inst(op(Sm4Opcode::IEQ), {&td, &rs, &kk});
        const Operand cur = src(result);
        body_.inst(op(Sm4Opcode::MOVC), {&d, &ts, &vs, &cur});
    }
    return true;
}

bool Lowerer::emit_load_input_ext(const DxilInstruction& inst) {
    // LoadInput(inputSigId, rowIndex, colIndex, gsVertexAxis)
    u32 id = 0;
    if (!const_u32(inst.operands[2], id)) return malformed("non-constant loadInput element");
    const bool has_vertex = !undef(inst.operands[5]);
    Sm4OperandType type = Sm4OperandType::Input;
    bool arrayed = false;
    switch (out_.stage) {
    case ShaderStage::Geometry: arrayed = true; break;
    case ShaderStage::Hull:
        arrayed = true;
        if (stage_.fork) type = Sm4OperandType::InputControlPoint;
        break;
    case ShaderStage::Domain:
        arrayed = true;
        type = Sm4OperandType::InputControlPoint;
        break;
    default: break;
    }
    if (arrayed != has_vertex) return malformed("loadInput vertex axis does not match the stage");
    const Index vertex = has_vertex ? index_of(inst.operands[5]) : Index{};
    return load_signature(sig_in_, type, id, inst.operands[3], inst.operands[4], has_vertex ? &vertex : nullptr,
                          inst.result);
}

bool Lowerer::emit_store_output_ext(const DxilInstruction& inst) {
    // StoreOutput / StorePatchConstant(sigId, row, col, value)
    u32 opcode = 0, id = 0, col = 0;
    const_u32(inst.operands[1], opcode);
    if (!const_u32(inst.operands[2], id)) return malformed("non-constant signature element");
    if (!const_u32(inst.operands[4], col)) return unimplemented("a dynamically indexed output column");
    const std::vector<SigElement>& sig = opcode == 106 ? sig_pc_ : sig_out_;
    const SigElement* e = nullptr;
    for (const SigElement& s : sig) {
        if (s.id == id) e = &s;
    }
    if (e == nullptr) return malformed("a signature store names no element");
    if (e->start_row < 0) return unimplemented("an output element with no register (" + e->name + ")");
    const u32 comp = e->start_col + col;
    const Operand v = src(inst.operands[5]);
    const Operand o = stages::reg1(Sm4OperandType::Output, index_of(inst.operands[3], static_cast<u32>(e->start_row)),
                                   1u << comp, true);
    body_.inst(op(Sm4Opcode::MOV), {&o, &v});
    return true;
}

bool Lowerer::emit_system_value(Sm4OperandType type, u32 comp, const DxilInstruction& inst) {
    system_inputs_.insert(static_cast<u32>(type));
    const Operand d = dst(inst.result);
    const Operand s = special_src(type, swizzle_of(comp));
    body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    return true;
}

bool Lowerer::emit_stage_op(u32 opcode, const DxilInstruction& inst, bool& handled) {
    using namespace stages;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    handled = true;
    const ShaderStage st = out_.stage;
    switch (opcode) {
    case 97:   // EmitStream(streamId)
    case 98:   // CutStream(streamId)
    case 99: {  // EmitThenCutStream(streamId)
        if (st != ShaderStage::Geometry) return malformed("a stream operation outside the geometry stage");
        u32 s = 0;
        if (!const_u32(arg(1), s) || s >= 4) return malformed("bad stream id");
        if ((stage_.gs_stream_mask & (1u << s)) == 0) return malformed("a stream the shader does not declare");
        const Operand m = stream_operand(s);
        body_.inst(opcode == 97 ? kEmitStream : opcode == 98 ? kCutStream : kEmitThenCutStream, {&m});
        return true;
    }
    case 100:  // GSInstanceID
        if (st != ShaderStage::Geometry) return malformed("GSInstanceID outside the geometry stage");
        return emit_system_value(Sm4OperandType::InputGSInstanceID, 0, inst);
    case 103: {  // LoadOutputControlPoint(sigId, row, col, index)
        if (st != ShaderStage::Hull || !stage_.fork) return malformed("loadOutputControlPoint outside a patch-constant function");
        u32 id = 0;
        if (!const_u32(arg(1), id)) return malformed("non-constant signature element");
        const Index cp = index_of(arg(4));
        return load_signature(sig_out_, Sm4OperandType::OutputControlPoint, id, arg(2), arg(3), &cp, inst.result);
    }
    case 104: {  // LoadPatchConstant(sigId, row, col)
        if (st != ShaderStage::Domain) return malformed("loadPatchConstant outside the domain stage");
        u32 id = 0;
        if (!const_u32(arg(1), id)) return malformed("non-constant signature element");
        return load_signature(sig_pc_, Sm4OperandType::InputPatchConstant, id, arg(2), arg(3), nullptr, inst.result);
    }
    case 105: {  // DomainLocation(component)
        if (st != ShaderStage::Domain) return malformed("DomainLocation outside the domain stage");
        u32 c = 0;
        if (!const_u32(arg(1), c) || c > 2) return malformed("bad domain location component");
        return emit_system_value(Sm4OperandType::InputDomainPoint, c, inst);
    }
    case 106:  // StorePatchConstant(sigId, row, col, value)
        if (st != ShaderStage::Hull || !stage_.fork) return malformed("storePatchConstant outside a patch-constant function");
        return emit_store_output_ext(inst);
    case 107:  // OutputControlPointID
        if (st != ShaderStage::Hull || stage_.fork) return malformed("OutputControlPointID outside a control-point function");
        return emit_system_value(Sm4OperandType::OutputControlPointId, 0, inst);
    case 108:  // PrimitiveID (geometry, hull, domain; a pixel shader loads it)
        if (st != ShaderStage::Geometry && st != ShaderStage::Hull && st != ShaderStage::Domain) {
            return malformed("PrimitiveID in this stage");
        }
        return emit_system_value(Sm4OperandType::InputPrimitiveID, 0, inst);
    default:
        return emit_mesh_op(opcode, inst, handled);
    }
}

void Lowerer::reset_function_state() {
    slots_.clear();
    vec_regs_.clear();
    status_slots_.clear();
    status_scan_fn_ = nullptr;
    handles_.clear();
    pointers_.clear();
    // Immutable globals and their initializer slots belong to the module,
    // shared by the control-point and fork phases.
    prologue_ = Emitter{};
    phi_scratch_.clear();
    local_arrays_.clear();
    indexable_sizes_.clear();
    system_inputs_.clear();
    constructs_.clear();
    body_ = Emitter{};
    next_reg_ = 0;
    next_comp_ = 1;
}

bool Lowerer::lower_function() {
    if (!build_cfg()) return false;
    brk_reg_ = 0;  // r0.x: the multi-level branch target
    next_reg_ = 0;
    next_comp_ = 1;
    {
        const Operand d = dst_temp(0, 1);
        const Operand z = imm(0);
        body_.inst(op(Sm4Opcode::MOV), {&d, &z});
    }
    do_tree(0);
    if (failed_) return false;
    if (!body_.last_was_jump()) body_.inst(op(Sm4Opcode::RET), {});
    return true;
}

DxilToDxbcResult Lowerer::run_hull() {
    using namespace stages;
    // Global part: hs_decls, the control point counts, the tessellator,
    // the flags and every resource.
    decls_.inst(kHsDecls, {});
    decls_.inst(kDclInputCpCount | (stage_.input_cp << 11), {});
    decls_.inst(kDclOutputCpCount | (stage_.output_cp << 11), {});
    decls_.inst(kDclTessDomain | (stage_.domain << 11), {});
    decls_.inst(kDclTessPartitioning | (stage_.partitioning << 11), {});
    decls_.inst(kDclTessOutputPrimitive | (stage_.output_primitive << 11), {});
    decls_.inst(kDclHsMaxTessFactor, {}, {stage_.max_tess_factor_bits});
    decls_.inst(kDclGlobalFlags | (((shader_flags_ >> 1) & 1u) != 0 ? 0u : kRefactoringAllowed), {});
    for (const Resource& r : resources_) {
        if (!declare_resource(r)) return out_;
    }
    for (const Resource& r : heap_resources_) {
        if (!declare_resource(r)) return out_;
    }
    const auto phase = [&](u32 opcode) {
        decls_.inst(opcode, {});
        for (u32 t : system_inputs_) {
            const Operand o = special_dcl(static_cast<Sm4OperandType>(t), 0x7);
            decls_.inst(kDclInput, {&o});
        }
        if (!declare_stage_signatures()) return false;
        decls_.inst(kDclTemps, {}, {next_reg_ + 1});
        for (usize i = 0; i < indexable_sizes_.size(); ++i) {
            decls_.inst(kDclIndexableTemp, {}, {static_cast<u32>(i), indexable_sizes_[i], 4});
        }
        for (u32 token : prologue_.code()) decls_.raw(token);
        for (u32 token : body_.code()) decls_.raw(token);
        return true;
    };
    stage_.fork = false;
    if (!lower_function() || !phase(kHsControlPointPhase)) return out_;
    reset_function_state();
    fn_ = stage_.patch_constant_fn;
    stage_.fork = true;
    if (!lower_function() || !phase(kHsForkPhase)) return out_;
    body_ = Emitter{};
    out_.container = write_container();
    out_.ok = true;
    return out_;
}

// ---- RDEF ---------------------------------------------------------------

std::vector<u8> Lowerer::write_rdef() {
    // D3D11 shader reflection, target 5.1 (layout as dxbc_parser.cpp reads
    // it, measured from fxc 10.1): header {cbuffer count, offset, binding
    // count, offset, version token, flags, creator}, the 5.1 header
    // (0x25441313, record sizes 3c 18 28 28 24 0c), 40-byte bindings
    // {name, D3D_SHADER_INPUT_TYPE, return type, dimension, samples, bind
    // point, count, flags, space, range ID}, 24-byte cbuffer records and
    // 40-byte variables.
    std::vector<u8> out(60, 0);
    std::vector<std::pair<usize, std::string>> names;  // where to patch, the string
    const auto put = [&](usize at, u32 v) { std::memcpy(out.data() + at, &v, 4); };
    const auto push = [&](u32 v) {
        const usize at = out.size();
        out.resize(at + 4);
        put(at, v);
        return at;
    };
    const auto name_of = [](const Resource& r) {
        if (!r.name.empty()) return r.name;
        static const char* prefix[] = {"t", "u", "cb", "s"};
        return std::string("$") + prefix[static_cast<u32>(r.cls)] + std::to_string(r.id);
    };
    const u32 kind = out_.stage == ShaderStage::Fragment ? 0xFFFFu
                     : out_.stage == ShaderStage::Vertex ? 0xFFFEu
                     : out_.stage == ShaderStage::Geometry ? 0x4753u
                     : out_.stage == ShaderStage::Hull ? 0x4853u
                     : out_.stage == ShaderStage::Domain ? 0x4453u
                                                         : 0x4353u;
    put(16, (kind << 16) | 0x0501u);
    put(28, 0x25441313u);
    put(32, 0x3c);
    put(36, 0x18);
    put(40, 0x28);
    put(44, 0x28);
    put(48, 0x24);
    put(52, 0x0c);
    // Bindings.
    put(8, static_cast<u32>(resources_.size()));
    put(12, static_cast<u32>(out.size()));
    for (const Resource& r : resources_) {
        u32 type = 0, ret = 0, dim = 0, samples = 0;
        const bool unbounded = r.upper == kDxbcUnboundedRange;
        u32 count = unbounded ? 0u : r.upper - r.lower + 1u;
        switch (r.cls) {
        case ResourceClass::CBV:
            type = 0;
            if (unbounded) count = 0xFFFFFFFFu;
            break;
        case ResourceClass::Sampler: type = 3; break;
        case ResourceClass::SRV:
        case ResourceClass::UAV: {
            const bool uav = r.cls == ResourceClass::UAV;
            if (r.shape == ResourceShape::RTAccelerationStructure) {
                // D3D_SIT_RTACCELERATIONSTRUCTURE, D3D_SRV_DIMENSION_UNKNOWN.
                type = 12;
            } else if (r.shape == ResourceShape::RawBuffer) {
                type = uav ? 8 : 7;
                ret = 6;
                dim = uav ? 1 : 11;
            } else if (r.shape == ResourceShape::StructuredBuffer) {
                type = uav ? (r.counter ? 11 : 6) : 5;
                ret = 6;
                dim = uav ? 1 : 11;
                samples = r.stride;
            } else {
                type = uav ? 4 : 2;
                ret = return_type(r.element_type);
                // D3D_SRV_DIMENSION from the resource shape.
                static constexpr u32 kDim[] = {0, 2, 4, 6, 8, 9, 3, 5, 7, 10, 1};
                const u32 s = static_cast<u32>(r.shape);
                dim = s < std::size(kDim) ? kDim[s] : 0;
                samples = r.shape == ResourceShape::Texture2DMS || r.shape == ResourceShape::Texture2DMSArray
                              ? r.sample_count
                              : 0xFFFFFFFFu;
            }
            break;
        }
        }
        names.push_back({push(0), name_of(r)});
        push(type);
        push(ret);
        push(dim);
        push(samples);
        push(r.lower);
        push(count);
        push(0);
        push(r.space);
        push(r.id);
    }
    // Constant buffers, with their sizes and no variables. Windows creates
    // a DXIL pipeline whose root constants are fewer than its constant
    // buffer's bytes (measured, d3d12_sm6_stage_probe "rdef": cs_three, a
    // cbuffer of three uints, with 2 root constants is S_OK on WARP12 and
    // the RTX 4080), so the frontend's SM 5.1 variable-extent check
    // (d3d12_pipeline.cpp cb_values) must not run for a lowered program:
    // no variables, no check.
    std::vector<const Resource*> cbs;
    for (const Resource& r : resources_) {
        if (r.cls == ResourceClass::CBV) cbs.push_back(&r);
    }
    put(0, static_cast<u32>(cbs.size()));
    put(4, static_cast<u32>(out.size()));
    for (const Resource* r : cbs) {
        names.push_back({push(0), name_of(*r)});
        push(0);
        push(0);
        push(r->cb_bytes);
        push(0);
        push(0);  // D3D_CT_CBUFFER
    }
    const usize creator = out.size();
    for (const char c : std::string("Vitreo DXIL lowering")) out.push_back(static_cast<u8>(c));
    out.push_back(0);
    put(24, static_cast<u32>(creator));
    for (const auto& [at, s] : names) {
        put(at, static_cast<u32>(out.size()));
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    }
    while (out.size() % 4 != 0) out.push_back(0);
    return out;
}
