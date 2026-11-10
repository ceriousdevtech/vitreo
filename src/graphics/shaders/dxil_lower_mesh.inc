// SPDX-License-Identifier: LGPL-2.1-only
// dxil_lower_mesh.inc -- part of dxil_lower.cpp's Lowerer (included at the
// end of its anonymous namespace; see dxil_lower_members.inc): mesh and
// amplification shaders (shader model 6.5).
//
// Sources: DirectX-Specs "Mesh Shader" (v0.86) and DXIL.rst (opcodes
// 168-173; entry properties kDxilMSStateTag 9 = {numthreads,
// maxVertexCount, maxPrimitiveCount, outputTopology, payloadSizeInBytes}
// and kDxilASStateTag 10 = {numthreads, payloadSizeInBytes}, read off dxc
// 1.6 listings in tests/oracle_data/d3d12_mesh/*.dxil.txt).
//
// SM 5.1 has no mesh program, so the lowered container is a COMPUTE
// program (dcl_thread_group from numthreads) with a VXMS chunk naming the
// stage, and every mesh-specific operation is a MeshOp extension
// instruction (dxbc_vitreo_ext.hpp). Vertex outputs keep their OSG1
// registers (written as OSGN), primitive outputs their PSG1 registers
// (PCSG); the translator stages them in groupshared memory and the MSL
// mesh compiler hands them to the metal::mesh (spirv_to_msl.cpp).

namespace mesh_limits {
// DirectX-Specs "Mesh Shader": numthreads X*Y*Z <= 128 (MS and AS),
// at most 256 output vertices and 256 output primitives, a payload of at
// most 16k bytes; groupshared (payload included) <= 28k for a mesh shader
// and <= 32k for an amplification shader.
constexpr u32 kMaxThreads = 128;
constexpr u32 kMaxVertices = 256;
constexpr u32 kMaxPrimitives = 256;
constexpr u32 kMaxPayloadBytes = 16384;
constexpr u32 kMeshGroupsharedBytes = 28672;
constexpr u32 kAmplificationGroupsharedBytes = 32768;
// DXIL semantic kind of SV_CullPrimitive (DXIL.rst "Semantic kind": the
// 31st row, value 30; dx.entryPoints element operand 3).
constexpr u32 kKindCullPrimitive = 30;
}  // namespace mesh_limits

bool Lowerer::read_mesh_property(u64 tag, i64 value) {
    const DxilMetadata* n = m_.md(value);
    const auto field = [&](u32 k) {
        u64 v = 0;
        if (n != nullptr && k < n->operands.size()) m_.md_int(n->operands[k], v);
        return static_cast<u32>(v);
    };
    const auto numthreads = [&]() {
        const DxilMetadata* t = n != nullptr && !n->operands.empty() ? m_.md(n->operands[0]) : nullptr;
        if (t == nullptr || t->operands.size() != 3) return false;
        for (u32 k = 0; k < 3; ++k) {
            u64 v = 0;
            m_.md_int(t->operands[k], v);
            threads_[k] = static_cast<u32>(v);
        }
        return true;
    };
    if (tag == 9) {  // kDxilMSStateTag
        if (out_.stage != ShaderStage::Mesh || n == nullptr || n->operands.size() < 5 || !numthreads()) {
            return malformed("malformed mesh shader state");
        }
        mesh_.have = true;
        mesh_.max_vertices = field(1);
        mesh_.max_primitives = field(2);
        mesh_.topology = field(3);
        mesh_.payload_bytes = field(4);
    } else {  // kDxilASStateTag
        if (out_.stage != ShaderStage::Amplification || n == nullptr || n->operands.size() < 2 || !numthreads()) {
            return malformed("malformed amplification shader state");
        }
        mesh_.have = true;
        mesh_.payload_bytes = field(1);
    }
    return true;
}

bool Lowerer::check_mesh_program() {
    using namespace mesh_limits;
    const bool ms = out_.stage == ShaderStage::Mesh;
    if (!ms && out_.stage != ShaderStage::Amplification) return true;
    if (!mesh_.have) return malformed(ms ? "mesh shader without its state" : "amplification shader without its state");
    const u64 threads = static_cast<u64>(threads_[0]) * threads_[1] * threads_[2];
    if (threads == 0 || threads > kMaxThreads) return malformed("numthreads X*Y*Z is not 1..128");
    if (mesh_.payload_bytes > kMaxPayloadBytes) return malformed("payload larger than 16k bytes");
    u64 groupshared = 0;
    for (u32 words : tgsm_sizes_) groupshared += static_cast<u64>(words) * 4u;
    if (ms) {
        if (mesh_.max_vertices > kMaxVertices || mesh_.max_primitives > kMaxPrimitives) {
            return malformed("more than 256 output vertices or primitives");
        }
        if (mesh_.topology != 1 && mesh_.topology != 2) return malformed("outputtopology is not line or triangle");
        if (groupshared + mesh_.payload_bytes > kMeshGroupsharedBytes) {
            return malformed("groupshared plus payload above 28k bytes");
        }
    } else {
        if (!mesh_.dispatched) return malformed("an amplification shader that never calls DispatchMesh");
        if (groupshared > kAmplificationGroupsharedBytes) return malformed("groupshared above 32k bytes");
        if (mesh_.payload_bytes == 0) return malformed("an amplification shader without a payload");
    }
    mesh_.groupshared_bytes = static_cast<u32>(groupshared);
    return true;
}

std::vector<u8> Lowerer::write_mesh_chunk() {
    const u32 w[kDxbcMeshStateWords] = {
        out_.stage == ShaderStage::Mesh ? u32{DxbcMeshState::Mesh} : u32{DxbcMeshState::Amplification},
        threads_[0], threads_[1], threads_[2],
        mesh_.topology, mesh_.max_vertices, mesh_.max_primitives,
        mesh_.payload_bytes, mesh_.payload_words, mesh_.groupshared_bytes, static_cast<u32>(mesh_.payload_layout.size())};
    std::vector<u8> out(sizeof(w) + mesh_.payload_layout.size() * 4u);
    std::memcpy(out.data(), w, sizeof(w));
    if (!mesh_.payload_layout.empty()) {
        std::memcpy(out.data() + sizeof(w), mesh_.payload_layout.data(), mesh_.payload_layout.size() * 4u);
    }
    return out;
}

bool Lowerer::emit_payload_load(const Pointer& p, const Operand& d, u32 kind) {
    // The payload moves as its byte image (byte_layout): PayloadLoad reads
    // the 32-bit word holding byte p; an i16 is its 16 bits there
    // zero-extended, a half the float32 of its 16 bits.
    using namespace dxbc_vitreo_ext;
    const auto scalar = [&](const Slot& s) { return src_temp_scalar(s.reg, s.comp); };
    Operand word_index = imm(p.constant >> 2);
    Operand shift = imm((p.constant & 3u) * 8u);
    if (p.has_reg) {
        const Operand byte = pointer_index_src(p);
        const Slot w = fresh_scalar();
        const Operand wd = dst_temp(w.reg, 1u << w.comp);
        const Operand two = imm(2);
        body_.inst(op(Sm4Opcode::USHR), {&wd, &byte, &two});
        word_index = scalar(w);
        if (kind != kDxbcPayloadWord) {
            const Slot b = fresh_scalar();
            const Operand bd = dst_temp(b.reg, 1u << b.comp);
            const Operand three = imm(3);
            body_.inst(op(Sm4Opcode::AND), {&bd, &byte, &three});
            const Operand bs = scalar(b);
            body_.inst(op(Sm4Opcode::ISHL), {&bd, &bs, &three});
            shift = scalar(b);
        }
    }
    if (kind == kDxbcPayloadWord) {
        emit_ext(body_, kOpMesh, mesh_control(MeshOp::PayloadLoad), {&d, &word_index});
        return true;
    }
    const Slot w = fresh_scalar();
    const Operand wd = dst_temp(w.reg, 1u << w.comp);
    emit_ext(body_, kOpMesh, mesh_control(MeshOp::PayloadLoad), {&wd, &word_index});
    const Operand ws = scalar(w);
    const Operand sixteen = imm(16);
    if (kind == kDxbcPayloadU16) {
        body_.inst(op(Sm4Opcode::UBFE), {&d, &sixteen, &shift, &ws});
    } else {
        body_.inst(op(Sm4Opcode::UBFE), {&wd, &sixteen, &shift, &ws});
        body_.inst(op(Sm4Opcode::F16TOF32), {&d, &ws});
    }
    return true;
}

bool Lowerer::emit_mesh_store(bool primitive, const DxilInstruction& inst) {
    // Store{Vertex,Primitive}Output(outputSigId, rowIndex, colIndex, value,
    // vertexIndex / primitiveIndex)
    using namespace dxbc_vitreo_ext;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    u32 id = 0, col = 0;
    if (!const_u32(arg(1), id) || !const_u32(arg(3), col)) return malformed("non-constant mesh output element");
    const std::vector<SigElement>& sig = primitive ? sig_pc_ : sig_out_;
    const SigElement* e = nullptr;
    for (const SigElement& s : sig) {
        if (s.id == id) e = &s;
    }
    if (e == nullptr) return malformed("a mesh output that names no element");
    if (col >= e->cols) return malformed("a mesh output column past its element");
    u32 reg = 0, comp = 0;
    if (e->start_row < 0) {
        // Only SV_CullPrimitive is unpacked (DXIL.rst "NotPacked").
        if (!primitive || e->kind != mesh_limits::kKindCullPrimitive) {
            return unimplemented("a mesh output element without a register (" + e->name + ")");
        }
        reg = kMeshCullPrimitiveRegister;
    } else {
        reg = static_cast<u32>(e->start_row);
        comp = e->start_col + col;
        if (reg + e->rows > 32u || comp > 3u) return malformed("a mesh output register out of range");
    }
    const Operand index = src(arg(5));
    const Operand value = src(arg(4));
    const MeshOp op = primitive ? MeshOp::StorePrimitive : MeshOp::StoreVertex;
    u32 row = 0;
    if (const_u32(arg(2), row)) {
        if (row >= e->rows) return malformed("a mesh output row past its element");
        emit_ext(body_, kOpMesh, mesh_control(op, reg + row, comp, e->rows), {&index, &value});
    } else {
        if (reg == kMeshCullPrimitiveRegister) return malformed("a dynamic row of SV_CullPrimitive");
        const Operand r = src(arg(2));
        emit_ext(body_, kOpMesh, mesh_control(op, reg, comp, e->rows) | kMeshDynamicRow, {&index, &value, &r});
    }
    return true;
}

bool Lowerer::emit_mesh_op(u32 opcode, const DxilInstruction& inst, bool& handled) {
    using namespace dxbc_vitreo_ext;
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const bool ms = out_.stage == ShaderStage::Mesh;
    const bool as = out_.stage == ShaderStage::Amplification;
    handled = true;
    switch (opcode) {
    case 168: {  // SetMeshOutputCounts(numVertices, numPrimitives)
        if (!ms) return malformed("SetMeshOutputCounts outside a mesh shader");
        const Operand v = src(arg(1)), p = src(arg(2));
        emit_ext(body_, kOpMesh, mesh_control(MeshOp::SetCounts), {&v, &p});
        return true;
    }
    case 169: {  // EmitIndices(PrimitiveIndex, VertexIndex0, VertexIndex1, VertexIndex2)
        if (!ms) return malformed("EmitIndices outside a mesh shader");
        const Operand p = src(arg(1)), a = src(arg(2)), b = src(arg(3)), c = src(arg(4));
        emit_ext(body_, kOpMesh, mesh_control(MeshOp::EmitIndices), {&p, &a, &b, &c});
        return true;
    }
    case 170: {  // GetMeshPayload()
        if (!ms) return malformed("GetMeshPayload outside a mesh shader");
        const DxilType& t = type(inst.type);
        if (t.kind != DxilTypeKind::Pointer) return malformed("GetMeshPayload that returns no pointer");
        // The payload is read as its byte image (byte_layout), whatever
        // type the amplification shader wrote it as.
        u32 size = 0, align = 0;
        if (!byte_layout(m_, t.element, size, align) || size == 0) {
            return unimplemented("a mesh payload of " + dxil_type_name(m_, t.element));
        }
        if (size != mesh_.payload_bytes) return malformed("a mesh payload type of another size than the declared one");
        mesh_.payload_words = (size + 3u) / 4u;
        Pointer p{};
        p.payload = true;
        pointers_[inst.result] = p;
        return true;
    }
    case 171:  // StoreVertexOutput
    case 172:  // StorePrimitiveOutput
        if (!ms) return malformed("a mesh output store outside a mesh shader");
        return emit_mesh_store(opcode == 172, inst);
    case 173: {  // DispatchMesh(threadGroupCountX, Y, Z, payload)
        if (!as) return malformed("DispatchMesh outside an amplification shader");
        if (mesh_.dispatched) return malformed("DispatchMesh called more than once");
        Pointer p;
        if (!resolve_pointer(arg(4), p)) return false;
        if (!p.tgsm || p.has_reg || p.constant != 0) return malformed("a DispatchMesh payload that is not a groupshared object");
        mesh_.dispatched = true;
        // The payload leaves as its byte image (byte_layout): the object
        // epilogue packs the groupshared object's scalars into it.
        u32 global = ~0u;
        for (const auto& [g, id] : tgsm_ids_) {
            if (id == p.object) global = g;
        }
        if (global == ~0u) return malformed("a DispatchMesh payload that is not a groupshared object");
        const u32 pt = m_.globals[global].value_type;
        u32 size = 0, align = 0;
        std::vector<u32> layout;
        if (!byte_layout(m_, pt, size, align) || !payload_scalars(m_, pt, 0, layout) ||
            layout.size() != tgsm_sizes_[p.object]) {
            return unimplemented("an amplification payload of " + dxil_type_name(m_, pt));
        }
        if (size != mesh_.payload_bytes) return malformed("a DispatchMesh payload of another size than the declared one");
        mesh_.payload_words = (size + 3u) / 4u;
        bool identity = layout.size() == mesh_.payload_words;
        for (u32 k = 0; identity && k < layout.size(); ++k) identity = layout[k] == 4u * k;
        if (!identity) mesh_.payload_layout = std::move(layout);
        const Operand x = src(arg(1)), y = src(arg(2)), z = src(arg(3));
        emit_ext(body_, kOpMesh, static_cast<u32>(MeshOp::DispatchMesh) | (p.object << 8), {&x, &y, &z});
        // "The DispatchMesh call ... ends the amplification shader group's
        // execution."
        body_.inst(op(Sm4Opcode::RET), {});
        return true;
    }
    default:
        handled = false;
        return true;
    }
}
