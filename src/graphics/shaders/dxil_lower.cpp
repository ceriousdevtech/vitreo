// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_lower.cpp
/// @brief DXIL -> shader model 5.1 lowering (see dxil_to_dxbc.hpp).

#include "dxil_to_dxbc.hpp"
#include "compile_profile.hpp"
#include "translation_cache.hpp"

#include "dxbc_parser.hpp"
#include "dxil_container.hpp"
#include "dxbc_vitreo_ext.hpp"
#include "dxil_lower_ext.hpp"
#include <tuple>
#include "dxil_raytracing.hpp"
#include "dxbc_vitreo_ext.hpp"
#include "dxil_lower_internal.hpp"

#include <algorithm>
#include <deque>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace vitreo::graphics {

using namespace dxil_lower;

namespace {

constexpr u32 op(Sm4Opcode o) { return static_cast<u32>(o); }

struct Slot {
    u32 reg{0};
    u32 comp{0};
};

struct Resource {
    ResourceClass cls{ResourceClass::SRV};
    u32 id{0};
    u32 space{0};
    u32 lower{0};
    u32 upper{0};
    ResourceShape shape{ResourceShape::TypedBuffer};
    u32 element_type{0};
    u32 stride{0};
    u32 sample_count{0};
    bool coherent{false};
    bool counter{false};
    bool rov{false};
    u32 cb_bytes{0};
    u32 sampler_kind{0};
    std::string name;  ///< dx.resources field 2 (may be empty)
    /// dx.resources field 1: the resource's global variable (libraries
    /// load it, then CreateHandleForLib), ~0u when none.
    u32 global{~0u};
    /// A constant buffer a raytracing local root signature supplies (read
    /// from the shader record, dxil_lower_rtpipe.inc).
    bool rt_local{false};
};

struct Handle {
    const Resource* res{nullptr};
    Index reg;
    bool nonuniform{false};
};

/// A pointer into thread-local (x#) or groupshared (g#) memory, in scalar
/// elements.
struct Pointer {
    bool tgsm{false};
    u32 object{0};   ///< x# array or g# id
    u32 constant{0};
    bool has_reg{false};
    Slot reg;
    /// A mesh shader's payload (GetMeshPayload): read through
    /// MeshOp::PayloadLoad; `object` is unused.
    bool payload{false};
};

struct Construct {
    bool loop{false};  ///< Loop (continue target) vs one-pass Block
    u32 node{0};       ///< Loop header / the block following the Block
    u32 id{0};
    bool passed{false};
};

#include "dxil_lower_rtctx.inc"

u32 scalar_count(const DxilModule& m, u32 t);

class ExtHost;
class Lowerer;
DxilRtProgram build_rt_program(const DxilRtProgramDesc& desc, bool validate_only);
std::unique_ptr<LowerHost> make_ext_host(Lowerer& l);

class Lowerer {
    friend class ExtHost;  // the wave / 16- / 64-bit lowering (dxil_lower_ext.hpp)
    friend DxilRtProgram build_rt_program(const DxilRtProgramDesc& desc, bool validate_only);

public:
    Lowerer(const DxilModule& m, const DxilContainer& c) : m_(m), c_(c) {}

    DxilToDxbcResult run();
    /// The container bytes the parts of c_ point into.
    const u8* source_{nullptr};

private:
    const DxilModule& m_;
    const DxilContainer& c_;
    const DxilFunction* fn_{nullptr};
    DxilToDxbcResult out_;
    Emitter decls_;
    /// Instructions that run before the body: the initial values of the
    /// constant global arrays (constant_globals_).
    Emitter prologue_;
    Emitter body_;
    /// Constant globals in address space 0 (HLSL static const arrays) ->
    /// the x# array that holds their initializer.
    std::map<u32, u32> constant_globals_;
    std::map<u32, std::vector<u32>> readonly_initializers_;
    std::vector<Resource> resources_;
    /// SM 6.6 dynamic resources (createHandleFromHeap + annotateHandle):
    /// one record per distinct resource type a heap handle is annotated
    /// with, declared as an unbounded range of the heap's register space
    /// (graphics::kDxbcResourceHeapSpace / kDxbcSamplerHeapSpace). A deque:
    /// handles point at them while more are added.
    std::deque<Resource> heap_resources_;
    struct HeapHandle {
        u32 index{0};  ///< the heap slot's value id
        bool sampler{false};
        bool nonuniform{false};
    };
    /// createHandleFromHeap results not yet annotated.
    std::map<u32, HeapHandle> heap_handles_;
    bool heap_resource(const HeapHandle& h, u32 props0, u32 props1, Handle& out);
    std::map<u32, Slot> slots_;
    std::map<u32, u32> vec_regs_;
    std::map<u32, Handle> handles_;
    std::map<u32, Pointer> pointers_;
    std::map<u32, u32> phi_scratch_;
    std::map<u32, u32> local_arrays_;  // alloca value id -> x#
    std::vector<u32> indexable_sizes_;
    std::map<u32, u32> tgsm_ids_;      // global index -> g#
    std::vector<u32> tgsm_sizes_;
    u32 next_reg_{0};
    u32 next_comp_{4};
    u32 brk_reg_{~0u};
    std::set<u32> system_inputs_;
    std::unique_ptr<LowerHost> host_;
    ExtState ext_;
    bool bypass_ext_{false};

    // ---- signatures ----
    /// A dx.entryPoints signature element record (DXIL.rst, "Signature
    /// element record").
    struct SigElement {
        u32 id{0};
        std::string name;
        u32 kind{0};
        u32 interpolation{0};
        u32 rows{1};
        u32 cols{1};
        i64 start_row{-1};
        u32 start_col{0};
    };
    /// One ISG1 / OSG1 / PSG1 entry (the DXBC v1 signature element:
    /// stream, name, index, system value, component type, register, mask,
    /// read-write mask, minimum precision).
    struct SigEntry {
        std::string name;
        u32 index{0}, system_value{0}, component_type{0}, reg{0}, stream{0}, min_precision{0};
        u8 mask{0}, rw_mask{0};
    };
    std::vector<SigElement> sig_in_, sig_out_, sig_pc_;
    std::vector<SigEntry> entries_in_, entries_out_, entries_pc_;
    bool read_signature(i64 list, std::vector<SigElement>& out);
    bool read_signature_part(const char* fourcc, std::vector<SigEntry>& out);
    bool declare_signatures();
    std::vector<u8> write_signature(const std::vector<SigEntry>& entries, bool with_stream);
    bool emit_load_input(const DxilInstruction& inst);
    bool emit_store_output(const DxilInstruction& inst);
    u32 threads_[3]{1, 1, 1};
    u64 shader_flags_{0};
    bool failed_{false};

    // CFG
    std::vector<std::vector<u32>> succ_;
    std::vector<std::vector<u32>> pred_edges_;  // forward in-edge counts per block via list
    std::vector<u32> rpo_;
    std::vector<u32> order_;
    std::vector<u32> idom_;
    std::vector<std::vector<u32>> children_;
    std::vector<bool> loop_header_;
    std::vector<u32> forward_in_;
    /// The loop header whose construct a block is placed after (a loop
    /// exit), ~0u for none.
    std::vector<u32> claimed_;
    std::vector<Construct> constructs_;
    u32 next_construct_{1};

#include "dxil_lower_members.inc"

    bool unimplemented(const std::string& what) {
        if (!failed_) {
            out_.failure = what;
            out_.unimplemented = true;
        }
        failed_ = true;
        return false;
    }
    bool malformed(const std::string& what) {
        if (!failed_) out_.failure = what;
        failed_ = true;
        return false;
    }

    // ---- registers ----
    Slot fresh_scalar() {
        if (next_comp_ >= 4) {
            next_comp_ = 0;
            ++next_reg_;
        }
        return Slot{next_reg_, next_comp_++};
    }
    u32 fresh_vector() {
        next_comp_ = 4;
        return ++next_reg_;
    }
    /// An aligned component pair (.xy or .zw): an i64 value.
    Slot fresh_pair() {
        if (next_comp_ > 2) {
            next_comp_ = 0;
            ++next_reg_;
        }
        next_comp_ = (next_comp_ + 1u) & ~1u;
        const Slot s{next_reg_, next_comp_};
        next_comp_ += 2;
        return s;
    }
    /// i64 and double values occupy an aligned component pair (the low
    /// word first), the layout of SM5's double instructions.
    bool is_i64_value(u32 id) const {
        const u32 t = m_.type_of(fn_, id);
        return is_int(t, 64) || is_f64(t);
    }
    Slot slot(u32 id) {
        auto it = slots_.find(id);
        if (it != slots_.end()) return it->second;
        const Slot s = is_i64_value(id) ? fresh_pair() : fresh_scalar();
        slots_[id] = s;
        return s;
    }
    u32 vec(u32 id) {
        auto it = vec_regs_.find(id);
        if (it != vec_regs_.end()) return it->second;
        const u32 r = fresh_vector();
        vec_regs_[id] = r;
        return r;
    }
    Operand dst(u32 id) {
        const Slot s = slot(id);
        if (is_i64_value(id)) return dst_temp(s.reg, 3u << s.comp);
        return dst_temp(s.reg, 1u << s.comp);
    }

    // ---- types ----
    const DxilType& type(u32 t) const { return m_.types[t]; }
    bool is_int(u32 t, u32 width) const {
        return t < m_.types.size() && type(t).kind == DxilTypeKind::Integer && type(t).width == width;
    }
    /// float, and half: a half is held as the float32 that represents it
    /// (dxbc_vitreo_ext.hpp), so comparisons, selects and moves treat it as
    /// a float; operations producing one round (dxil_lower_wide.cpp).
    bool is_f32(u32 t) const {
        return t < m_.types.size() &&
               (type(t).kind == DxilTypeKind::Float || type(t).kind == DxilTypeKind::Half);
    }
    bool is_f64(u32 t) const { return t < m_.types.size() && type(t).kind == DxilTypeKind::Double; }
    /// A value of one register component or pair (i16 zero-extended, i64
    /// and double a pair; their arithmetic is dxil_lower_wide.cpp's).
    bool scalar32(u32 t) const {
        return is_int(t, 32) || is_int(t, 1) || is_int(t, 16) || is_int(t, 64) || is_f32(t) || is_f64(t);
    }
    bool check_scalar32(u32 t, const char* what) {
        if (scalar32(t)) return true;
        const std::string name = dxil_type_name(m_, t);
        return unimplemented(std::string(what) + " on " + name +
                             " (16- and 64-bit values are not lowered yet)");
    }

    // ---- operands ----
    std::optional<u32> constant_bits(u32 id) {
        const DxilConstant* c = m_.constant(fn_, id);
        if (c == nullptr) return std::nullopt;
        const u32 t = m_.type_of(fn_, id);
        switch (c->kind) {
        case DxilConstantKind::Undef:
        case DxilConstantKind::Null:
            return 0u;
        case DxilConstantKind::Integer:
            if (is_int(t, 1)) return (c->bits & 1u) != 0 ? 0xFFFFFFFFu : 0u;
            if (is_int(t, 16)) return static_cast<u32>(c->bits) & 0xFFFFu;
            return static_cast<u32>(c->bits);
        case DxilConstantKind::Float:
            if (type(t).kind == DxilTypeKind::Half) return half_bits_to_float_bits(static_cast<u32>(c->bits));
            if (is_f32(t)) return static_cast<u32>(c->bits);
            unimplemented("a " + dxil_type_name(m_, t) + " constant");
            return 0u;
        default:
            unimplemented("a constant expression used as a value");
            return 0u;
        }
    }
    Operand src(u32 id, u32 modifier = 0) {
        if (is_i64_value(id)) {
            if (const DxilConstant* c = m_.constant(fn_, id)) {
                const u64 v =
                    c->kind == DxilConstantKind::Integer || c->kind == DxilConstantKind::Float ? c->bits : 0u;
                const u32 lo = static_cast<u32>(v), hi = static_cast<u32>(v >> 32);
                return imm4(lo, hi, lo, hi);
            }
            const Slot s = slot(id);
            return src_temp(s.reg, s.comp == 0 ? 0x44u : 0xEEu, modifier);
        }
        if (auto bits = constant_bits(id)) {
            u32 v = *bits;
            if (modifier == 1) {  // negate an immediate directly
                v = is_f32(m_.type_of(fn_, id)) ? (v ^ 0x80000000u) : static_cast<u32>(-static_cast<i32>(v));
            } else if (modifier == 2) {
                v &= 0x7FFFFFFFu;
            }
            return imm(v);
        }
        const DxilValue* v = m_.value(fn_, id);
        if (v == nullptr || v->kind == DxilValueKind::Argument) {
            malformed("an operand that is not a value");
            return imm(0);
        }
        const Slot s = slot(id);
        return src_temp_scalar(s.reg, s.comp, modifier);
    }
    /// A vector of at most four 32-bit scalars (one register).
    bool vector32(u32 t) const {
        return t < m_.types.size() && type(t).kind == DxilTypeKind::Vector && type(t).count <= 4 &&
               scalar32(type(t).element) && scalar_count(m_, type(t).element) == 1;
    }
    /// Element @p k of vector value @p id as a source.
    Operand vector_element(u32 id, u32 k) {
        if (const DxilConstant* c = m_.constant(fn_, id)) {
            if (c->kind == DxilConstantKind::Aggregate && k < c->elements.size()) {
                return src(static_cast<u32>(c->elements[k]));
            }
            if (c->kind == DxilConstantKind::Data && k < c->elements.size()) {
                return imm(static_cast<u32>(c->elements[k]));
            }
            return imm(0);  // zeroinitializer / undef
        }
        return src_temp_scalar(vec(id), k);
    }
    Index index_of(u32 id, u32 add = 0) {
        if (auto bits = constant_bits(id)) return Index{false, *bits + add, 0, 0};
        const Slot s = slot(id);
        return Index{true, add, s.reg, s.comp};
    }
    bool undef(u32 id) {
        const DxilConstant* c = m_.constant(fn_, id);
        return c != nullptr && c->kind == DxilConstantKind::Undef;
    }
    bool const_u32(u32 id, u32& out) {
        u64 v = 0;
        if (!m_.constant_int(fn_, id, v)) return false;
        out = static_cast<u32>(v);
        return true;
    }

    // ---- program setup ----
    bool read_metadata();
    bool read_resources(i64 list);
    bool declare();
    bool declare_resource(const Resource& r);

    // ---- control flow ----
    bool build_cfg();
    void do_tree(u32 block);
    void node_within(u32 block, std::vector<u32> merges);
    void emit_terminator(u32 block);
    void do_branch(u32 from, u32 to);
    void jump(u32 target_node, bool loop);
    void close_construct();
    void emit_phi_copies(u32 from, u32 to);

    // ---- instructions ----
    void emit_block(u32 block);
    bool emit_instruction(const DxilInstruction& inst);
    bool emit_binop(const DxilInstruction& inst);
    /// "fp32-denorm-mode"="preserve": float32 arithmetic through VX_F32P.
    bool preserve_denormals_{false};
    void f32p(dxbc_vitreo_ext::F32POp o, const Operand& d, std::initializer_list<const Operand*> srcs) {
        const Operand c = control_operand(static_cast<u32>(o));
        usize length = 1 + c.tokens.size() + d.tokens.size();
        for (const Operand* x : srcs) length += x->tokens.size();
        body_.raw(dxbc_vitreo_ext::kOpF32P | (static_cast<u32>(length) << 24));
        for (u32 t : c.tokens) body_.raw(t);
        for (u32 t : d.tokens) body_.raw(t);
        for (const Operand* x : srcs) {
            for (u32 t : x->tokens) body_.raw(t);
        }
    }
    bool emit_cmp(const DxilInstruction& inst);
    bool emit_cast(const DxilInstruction& inst);
    bool emit_call(const DxilInstruction& inst);
    bool emit_memory(const DxilInstruction& inst);
    bool resolve_pointer(u32 id, Pointer& out);
    Operand pointer_index_src(const Pointer& p);
    Index pointer_index(const Pointer& p);
    bool handle_of(u32 id, Handle& out);
    Operand resource_src(const Handle& h, u32 swizzle = 0xE4);
    bool emit_resource_op(u32 opcode, const DxilInstruction& inst);
    bool coords_vector(const std::vector<u32>& ids, u32& reg, u32 extra_id = ~0u, u32 extra_at = 3);
    /// @p first is an index into inst.operands (0 the callee, 1 the opcode).
    void store_values(u32 reg, const DxilInstruction& inst, usize first, u32 mask);
    std::vector<u8> write_container();
};

// =============================================================================
// Metadata
// =============================================================================

bool Lowerer::read_metadata() {
    const DxilNamedMetadata* sm = m_.named("dx.shaderModel");
    const DxilNamedMetadata* eps = m_.named("dx.entryPoints");
    if (sm == nullptr || sm->nodes.empty() || eps == nullptr || eps->nodes.empty()) {
        return malformed("DXIL module without dx.shaderModel / dx.entryPoints");
    }
    const DxilMetadata* smn = m_.md(sm->nodes[0]);
    if (smn == nullptr || smn->operands.size() < 3) return malformed("malformed dx.shaderModel");
    const std::string kind = m_.md_string(smn->operands[0]);
    u64 major = 0, minor = 0;
    m_.md_int(smn->operands[1], major);
    m_.md_int(smn->operands[2], minor);
    out_.shader_model_major = static_cast<u32>(major);
    out_.shader_model_minor = static_cast<u32>(minor);
    if (kind == "cs") out_.stage = ShaderStage::Compute;
    else if (kind == "vs") out_.stage = ShaderStage::Vertex;
    else if (kind == "ps") out_.stage = ShaderStage::Fragment;
    else if (kind == "gs") out_.stage = ShaderStage::Geometry;
    else if (kind == "hs") out_.stage = ShaderStage::Hull;
    else if (kind == "ds") out_.stage = ShaderStage::Domain;
    else if (kind == "ms") out_.stage = ShaderStage::Mesh;
    else if (kind == "as") out_.stage = ShaderStage::Amplification;
    else if (kind == "lib") return unimplemented("DXIL libraries (raytracing, work graphs)");
    else return malformed("unknown shader kind " + kind);
    if (eps->nodes.size() != 1) return malformed("more than one entry point");
    const DxilMetadata* ep = m_.md(eps->nodes[0]);
    if (ep == nullptr || ep->operands.size() < 5) return malformed("malformed entry point");
    const DxilMetadata* fv = m_.md(ep->operands[0]);
    if (fv == nullptr || fv->kind != DxilMetadata::Kind::Value) return malformed("entry point names no function");
    fn_ = m_.function_of(fv->value);
    if (fn_ == nullptr || fn_->is_declaration) return malformed("entry point function has no body");
    // SM 6.2 "fp32-denorm-mode"="preserve" (dxc -denorm preserve): the
    // Apple GPU flushes float32 denormals in every Metal math mode, so the
    // float32 add / subtract / multiply / divide, multiply-add, dot
    // products, comparisons, min / max and saturate of such a function
    // lower to VX_F32P (dxbc_vitreo_ext.hpp), which keeps them.
    preserve_denormals_ = fn_->fp32_denorm_mode == "preserve";
    if (!read_resources(ep->operands[3])) return false;
    add_rq_companions();
    // Properties: tag / value pairs.
    if (const DxilMetadata* props = m_.md(ep->operands[4])) {
        for (usize i = 0; i + 1 < props->operands.size(); i += 2) {
            u64 tag = 0;
            if (!m_.md_int(props->operands[i], tag)) continue;
            if (tag == 0) {
                m_.md_int(props->operands[i + 1], shader_flags_);
            } else if (tag == 4) {
                const DxilMetadata* n = m_.md(props->operands[i + 1]);
                if (n == nullptr || n->operands.size() != 3) return malformed("malformed numthreads");
                for (u32 k = 0; k < 3; ++k) {
                    u64 v = 0;
                    m_.md_int(n->operands[k], v);
                    threads_[k] = static_cast<u32>(v);
                }
            } else if (tag >= 1 && tag <= 3) {
                if (!read_stage_property(tag, props->operands[i + 1])) return false;
            } else if (tag == 9 || tag == 10) {
                if (!read_mesh_property(tag, props->operands[i + 1])) return false;
            } else if (tag == 11) {
                // kDxilWaveSizeTag: an MD list of one i32 lane count
                // (DXIL.rst; dxc 1.9 writes !{i32 11, !{i32 32}}).
                const DxilMetadata* n = m_.md(props->operands[i + 1]);
                u64 lanes = 0;
                if (n == nullptr || n->operands.size() != 1 || !m_.md_int(n->operands[0], lanes) || lanes == 0) {
                    return malformed("malformed WaveSize");
                }
                out_.wave_size_min = out_.wave_size_max = static_cast<u32>(lanes);
            } else if (tag == 23) {
                // kDxilRangedWaveSizeTag: (min, max, preferred); max 0 encodes
                // min == max (DXIL.rst SM.WAVESIZEMAXANDPREFERREDZEROWHENNORANGE).
                const DxilMetadata* n = m_.md(props->operands[i + 1]);
                u64 v[3] = {};
                if (n == nullptr || n->operands.size() != 3) return malformed("malformed WaveSize range");
                for (u32 k = 0; k < 3; ++k) {
                    if (!m_.md_int(n->operands[k], v[k])) return malformed("malformed WaveSize range");
                }
                out_.wave_size_min = static_cast<u32>(v[0]);
                out_.wave_size_max = static_cast<u32>(v[1] != 0 ? v[1] : v[0]);
            }
        }
    }
    if (!check_stage_state() || !plan_compute_quads()) return false;
    if (const DxilMetadata* sigs = m_.md(ep->operands[2])) {
        if (sigs->operands.size() < 3) return malformed("malformed signature list");
        if (!read_signature(sigs->operands[0], sig_in_) || !read_signature(sigs->operands[1], sig_out_) ||
            !read_signature(sigs->operands[2], sig_pc_)) {
            return false;
        }
    }
    if (!read_signature_part("ISG1", entries_in_) || !read_signature_part("OSG1", entries_out_) ||
        !read_signature_part("PSG1", entries_pc_)) {
        return false;
    }
    return true;
}

bool Lowerer::read_signature(i64 list, std::vector<SigElement>& out) {
    const DxilMetadata* l = m_.md(list);
    if (l == nullptr) return true;
    for (i64 rec_index : l->operands) {
        const DxilMetadata* rec = m_.md(rec_index);
        if (rec == nullptr || rec->operands.size() < 10) return malformed("malformed signature element");
        SigElement e;
        u64 v = 0;
        m_.md_int(rec->operands[0], v);
        e.id = static_cast<u32>(v);
        e.name = m_.md_string(rec->operands[1]);
        m_.md_int(rec->operands[3], v);
        e.kind = static_cast<u32>(v);
        m_.md_int(rec->operands[5], v);
        e.interpolation = static_cast<u32>(v);
        m_.md_int(rec->operands[6], v);
        e.rows = static_cast<u32>(v);
        m_.md_int(rec->operands[7], v);
        e.cols = static_cast<u32>(v);
        m_.md_int(rec->operands[8], v);
        e.start_row = static_cast<i32>(static_cast<u32>(v));
        m_.md_int(rec->operands[9], v);
        e.start_col = static_cast<u32>(v);
        out.push_back(std::move(e));
    }
    return true;
}

bool Lowerer::read_signature_part(const char* fourcc, std::vector<SigEntry>& out) {
    const DxilContainerPart* p = c_.part(fourcc);
    if (p == nullptr) return true;
    const u8* d = source_ + p->offset;
    const u32 size = p->size;
    const auto rd = [&](u32 at) {
        u32 v = 0;
        if (at + 4 <= size) std::memcpy(&v, d + at, 4);
        return v;
    };
    const u32 count = rd(0);
    const u32 first = rd(4);
    for (u32 i = 0; i < count; ++i) {
        const u32 at = first + i * 32u;
        if (at + 32u > size) return malformed(std::string(fourcc) + " element past its part");
        SigEntry e;
        e.stream = rd(at);
        const u32 name_at = rd(at + 4);
        for (u32 k = name_at; k < size && d[k] != 0; ++k) e.name.push_back(static_cast<char>(d[k]));
        e.index = rd(at + 8);
        e.system_value = rd(at + 12);
        e.component_type = rd(at + 16);
        e.reg = rd(at + 20);
        e.mask = d[at + 24];
        e.rw_mask = d[at + 25];
        e.min_precision = rd(at + 28);
        out.push_back(std::move(e));
    }
    return true;
}

bool Lowerer::read_resources(i64 list) {
    const DxilMetadata* lists = m_.md(list);
    if (lists == nullptr) return true;  // no resources
    if (lists->operands.size() != 4) return malformed("malformed resource lists");
    for (u32 cls = 0; cls < 4; ++cls) {
        const DxilMetadata* records = m_.md(lists->operands[cls]);
        if (records == nullptr) continue;
        for (i64 rec_index : records->operands) {
            const DxilMetadata* rec = m_.md(rec_index);
            if (rec == nullptr || rec->operands.size() < 7) return malformed("malformed resource record");
            Resource r;
            r.cls = static_cast<ResourceClass>(cls);
            r.name = m_.md_string(rec->operands[2]);
            if (const DxilMetadata* gv = m_.md(rec->operands[1]);
                gv != nullptr && gv->kind == DxilMetadata::Kind::Value) {
                r.global = gv->value;
            }
            u64 v = 0;
            m_.md_int(rec->operands[0], v);
            r.id = static_cast<u32>(v);
            m_.md_int(rec->operands[3], v);
            r.space = static_cast<u32>(v);
            m_.md_int(rec->operands[4], v);
            r.lower = static_cast<u32>(v);
            m_.md_int(rec->operands[5], v);
            const u32 size = static_cast<u32>(v);
            r.upper = size == 0xFFFFFFFFu ? kDxbcUnboundedRange : r.lower + size - 1u;
            m_.md_int(rec->operands[6], v);
            i64 tags = -1;
            if (r.cls == ResourceClass::SRV || r.cls == ResourceClass::UAV) {
                r.shape = static_cast<ResourceShape>(v);
                if (r.cls == ResourceClass::SRV) {
                    if (rec->operands.size() < 9) return malformed("short SRV record");
                    m_.md_int(rec->operands[7], v);
                    r.sample_count = static_cast<u32>(v);
                    tags = rec->operands[8];
                } else {
                    if (rec->operands.size() < 11) return malformed("short UAV record");
                    m_.md_int(rec->operands[7], v);
                    r.coherent = v != 0;
                    m_.md_int(rec->operands[8], v);
                    r.counter = v != 0;
                    m_.md_int(rec->operands[9], v);
                    r.rov = v != 0;
                    tags = rec->operands[10];
                }
                if (const DxilMetadata* t = m_.md(tags)) {
                    for (usize i = 0; i + 1 < t->operands.size(); i += 2) {
                        u64 tag = 0, value = 0;
                        m_.md_int(t->operands[i], tag);
                        m_.md_int(t->operands[i + 1], value);
                        if (tag == 0) r.element_type = static_cast<u32>(value);
                        if (tag == 1) r.stride = static_cast<u32>(value);
                    }
                }
            } else if (r.cls == ResourceClass::CBV) {
                r.cb_bytes = static_cast<u32>(v);
            } else {
                r.sampler_kind = static_cast<u32>(v);
            }
            resources_.push_back(r);
        }
    }
    return true;
}

// =============================================================================
// Declarations
// =============================================================================

u32 dxbc_dimension(ResourceShape s) {
    switch (s) {
    case ResourceShape::TypedBuffer: return 1;
    case ResourceShape::Texture1D: return 2;
    case ResourceShape::Texture2D: return 3;
    case ResourceShape::Texture2DMS: return 4;
    case ResourceShape::Texture3D: return 5;
    case ResourceShape::TextureCube: return 6;
    case ResourceShape::Texture1DArray: return 7;
    case ResourceShape::Texture2DArray: return 8;
    case ResourceShape::Texture2DMSArray: return 9;
    case ResourceShape::TextureCubeArray: return 10;
    default: return 0;
    }
}

/// D3D10_SB_RESOURCE_RETURN_TYPE of a DXIL element type (0: none).
u32 return_type(u32 element) {
    switch (static_cast<ComponentType>(element)) {
    case ComponentType::UNormF32: return 1;
    case ComponentType::SNormF32: return 2;
    case ComponentType::I32:
    case ComponentType::I16: return 3;  // int16 typed views read as sint
    case ComponentType::U32:
    case ComponentType::U16: return 4;
    case ComponentType::F32: return 5;
    case ComponentType::F16: return 5;  // min16float reads as float
    default: return 0;
    }
}

bool Lowerer::declare_resource(const Resource& r) {
    switch (r.cls) {
    case ResourceClass::CBV: {
        const Operand o = range_dcl(Sm4OperandType::ConstantBuffer, r.id, r.lower, r.upper);
        // Dynamically indexed: rows may be addressed by a register.
        decls_.inst(kDclConstantBuffer | (1u << 11), {&o}, {(r.cb_bytes + 15u) / 16u, r.space});
        return true;
    }
    case ResourceClass::Sampler: {
        const Operand o = range_dcl(Sm4OperandType::Sampler, r.id, r.lower, r.upper);
        decls_.inst(kDclSampler | (r.sampler_kind << 11), {&o}, {r.space});
        return true;
    }
    case ResourceClass::SRV:
    case ResourceClass::UAV: {
        const bool uav = r.cls == ResourceClass::UAV;
        const Sm4OperandType type =
            uav ? Sm4OperandType::UnorderedAccessView : Sm4OperandType::Resource;
        const Operand o = range_dcl(type, r.id, r.lower, r.upper);
        u32 flags = 0;
        if (uav) {
            if (r.coherent) flags |= kGloballyCoherent;
            if (r.rov) flags |= kRasterizerOrdered;
            if (r.counter) flags |= kUavHasCounter;
        }
        if (r.shape == ResourceShape::RTAccelerationStructure) {
            if (uav) return malformed("an acceleration structure UAV");
            decls_.inst(dxbc_vitreo_ext::kOpDclAccelerationStructure, {&o}, {r.space});
            return true;
        }
        // A feedback map is Vitreo's opaque storage, a raw buffer
        // (sampler_feedback_layout.hpp, dxil_lower_feedback.cpp).
        const bool feedback = uav && (r.shape == ResourceShape::FeedbackTexture2D ||
                                      r.shape == ResourceShape::FeedbackTexture2DArray);
        if (r.shape == ResourceShape::RawBuffer || feedback) {
            decls_.inst((uav ? kDclUavRaw : kDclResourceRaw) | flags, {&o}, {r.space});
            return true;
        }
        if (r.shape == ResourceShape::StructuredBuffer) {
            decls_.inst((uav ? kDclUavStructured : kDclResourceStructured) | flags, {&o},
                        {r.stride, r.space});
            return true;
        }
        const u32 dim = dxbc_dimension(r.shape);
        const u32 ret = return_type(r.element_type);
        if (dim == 0) return unimplemented("resource shape " + std::to_string(static_cast<u32>(r.shape)));
        if (ret == 0) {
            return unimplemented("resource element type " + std::to_string(r.element_type));
        }
        const u32 packed = ret | (ret << 4) | (ret << 8) | (ret << 12);
        if (uav) {
            decls_.inst(kDclUavTyped | (dim << 11) | flags, {&o}, {packed, r.space});
        } else {
            decls_.inst(kDclResource | (dim << 11) | ((r.sample_count & 0x7Fu) << 16), {&o},
                        {packed, r.space});
        }
        return true;
    }
    }
    return true;
}

bool Lowerer::declare() {
    // Disable-math-refactoring is shader flag bit 1 (DXIL.rst, Shader
    // Flags); otherwise refactoring is allowed and each instruction without
    // fast-math flags carries `precise`.
    decls_.inst(kDclGlobalFlags | (((shader_flags_ >> 1) & 1u) != 0 ? 0u : kRefactoringAllowed), {});
    for (const Resource& r : resources_) {
        if (!declare_resource(r)) return false;
    }
    for (const Resource& r : heap_resources_) {
        if (!declare_resource(r)) return false;
    }
    if (!declare_stage()) return false;
    const bool mesh_stage = out_.stage == ShaderStage::Mesh || out_.stage == ShaderStage::Amplification;
    for (u32 type : system_inputs_) {
        const Operand o = special_dcl(static_cast<Sm4OperandType>(type), 0x7);
        decls_.inst(kDclInput, {&o});
    }
    // A mesh shader's outputs go through MeshOp stores, not o# registers;
    // its signatures still travel as OSGN / PCSG.
    if (!mesh_stage && !declare_signatures()) return false;
    decls_.inst(kDclTemps, {}, {next_reg_ + 1});
    for (usize i = 0; i < indexable_sizes_.size(); ++i) {
        decls_.inst(kDclIndexableTemp, {}, {static_cast<u32>(i), indexable_sizes_[i], 4});
    }
    for (usize i = 0; i < tgsm_sizes_.size(); ++i) {
        Operand g;
        g.tokens.push_back((static_cast<u32>(Sm4OperandType::ThreadGroupSharedMemory) << 12) | (1u << 20));
        g.tokens.push_back(static_cast<u32>(i));
        decls_.inst(kDclTgsmStructured, {&g}, {4u, tgsm_sizes_[i]});
    }
    if (out_.stage == ShaderStage::Compute || mesh_stage) {
        decls_.inst(kDclThreadGroup, {}, {threads_[0], threads_[1], threads_[2]});
    }
    return true;
}

// =============================================================================
// Control flow (Ramsey, "Beyond Relooper")
// =============================================================================

bool Lowerer::build_cfg() {
    const usize n = fn_->blocks.size();
    succ_.assign(n, {});
    for (usize b = 0; b < n; ++b) {
        if (fn_->blocks[b].instructions.empty()) return malformed("empty basic block");
        const DxilInstruction& t = fn_->instructions[fn_->blocks[b].instructions.back()];
        if (t.op == DxilOpcode::Br || t.op == DxilOpcode::Switch) succ_[b] = t.blocks;
    }
    // Reverse postorder from the entry.
    rpo_.assign(n, ~0u);
    std::vector<u32> post;
    std::vector<u8> state(n, 0);
    std::vector<std::pair<u32, usize>> stack{{0, 0}};
    state[0] = 1;
    while (!stack.empty()) {
        auto& [b, i] = stack.back();
        if (i < succ_[b].size()) {
            const u32 s = succ_[b][i++];
            if (state[s] == 0) {
                state[s] = 1;
                stack.push_back({s, 0});
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    order_.assign(post.rbegin(), post.rend());
    for (usize i = 0; i < order_.size(); ++i) rpo_[order_[i]] = static_cast<u32>(i);
    // Dominators (Cooper, Harvey, Kennedy).
    idom_.assign(n, ~0u);
    idom_[0] = 0;
    std::vector<std::vector<u32>> preds(n);
    for (usize b = 0; b < n; ++b) {
        if (rpo_[b] == ~0u) continue;
        for (u32 s : succ_[b]) preds[s].push_back(static_cast<u32>(b));
    }
    const auto intersect = [&](u32 a, u32 b) {
        while (a != b) {
            while (rpo_[a] > rpo_[b]) a = idom_[a];
            while (rpo_[b] > rpo_[a]) b = idom_[b];
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b : order_) {
            if (b == 0) continue;
            u32 nd = ~0u;
            for (u32 p : preds[b]) {
                if (idom_[p] == ~0u) continue;
                nd = nd == ~0u ? p : intersect(p, nd);
            }
            if (nd != idom_[b]) {
                idom_[b] = nd;
                changed = true;
            }
        }
    }
    children_.assign(n, {});
    for (u32 b : order_) {
        if (b != 0) children_[idom_[b]].push_back(b);
    }
    loop_header_.assign(n, false);
    forward_in_.assign(n, 0);
    for (u32 b : order_) {
        for (u32 s : succ_[b]) {
            if (rpo_[s] <= rpo_[b]) {
                // A retreating edge must be a back edge (reducible CFG).
                u32 d = b;
                while (d != s && d != 0) d = idom_[d];
                if (d != s) return malformed("irreducible control flow");
                loop_header_[s] = true;
            } else {
                ++forward_in_[s];
            }
        }
    }
    // Natural loop bodies: the header and every block that reaches one of
    // its back edges without passing through it.
    std::vector<std::vector<bool>> body(n);
    std::vector<usize> body_size(n, 0);
    for (u32 h : order_) {
        if (!loop_header_[h]) continue;
        body[h].assign(n, false);
        body[h][h] = true;
        std::vector<u32> work;
        for (u32 p : preds[h]) {
            if (rpo_[p] >= rpo_[h] && !body[h][p]) {
                body[h][p] = true;
                work.push_back(p);
            }
        }
        while (!work.empty()) {
            const u32 b = work.back();
            work.pop_back();
            for (u32 p : preds[b]) {
                if (!body[h][p]) {
                    body[h][p] = true;
                    work.push_back(p);
                }
            }
        }
        for (bool in : body[h]) body_size[h] += in ? 1u : 0u;
    }
    // A block outside a loop whose immediate dominator is inside it is
    // that loop's exit; the outermost such loop places it.
    claimed_.assign(n, ~0u);
    for (u32 v : order_) {
        if (v == 0) continue;
        const u32 d = idom_[v];
        for (u32 h : order_) {
            if (!loop_header_[h] || !body[h][d] || body[h][v]) continue;
            if (claimed_[v] == ~0u || body_size[h] > body_size[claimed_[v]]) claimed_[v] = h;
        }
    }
    return true;
}

void Lowerer::do_tree(u32 block) {
    std::vector<u32> merges;
    for (u32 c : children_[block]) {
        if (forward_in_[c] >= 2 && claimed_[c] == ~0u) merges.push_back(c);
    }
    // The outermost Block ends just before the latest merge node.
    const auto latest_first = [&](u32 a, u32 b) { return rpo_[a] > rpo_[b]; };
    std::sort(merges.begin(), merges.end(), latest_first);
    if (!loop_header_[block]) {
        node_within(block, merges);
        return;
    }
    // A loop's exits are placed after the loop, never inside it: lanes
    // leave the loop in different iterations, and the code they reach --
    // a barrier, a derivative, a wave operation -- must run once they have
    // reconverged, as it does in the DXIL. Each exit gets a Block around
    // the Loop (latest outermost), and its code follows that Block.
    std::vector<u32> exits;
    for (usize v = 0; v < claimed_.size(); ++v) {
        if (claimed_[v] == block) exits.push_back(static_cast<u32>(v));
    }
    std::sort(exits.begin(), exits.end(), latest_first);
    std::function<void(usize)> around = [&](usize k) {
        if (failed_) return;
        if (k == exits.size()) {
            constructs_.push_back(Construct{true, block, next_construct_++, false});
            body_.inst(op(Sm4Opcode::LOOP), {});
            node_within(block, merges);
            close_construct();
            return;
        }
        constructs_.push_back(Construct{false, exits[k], next_construct_++, false});
        const Operand selector = imm1(0);
        body_.inst(op(Sm4Opcode::SWITCH), {&selector});
        body_.inst(op(Sm4Opcode::DEFAULT), {});
        around(k + 1);
        close_construct();
        do_tree(exits[k]);
    };
    around(0);
}

void Lowerer::node_within(u32 block, std::vector<u32> merges) {
    if (failed_) return;
    if (merges.empty()) {
        emit_block(block);
        emit_terminator(block);
        return;
    }
    const u32 follow = merges.front();
    merges.erase(merges.begin());
    constructs_.push_back(Construct{false, follow, next_construct_++, false});
    // A forward merge needs a breakable scope, not a backedge. Encoding
    // it as LOOP creates false loop-carried dependencies (and a runtime
    // iteration guard) downstream. A single-default switch runs once and
    // preserves the nearest-construct BREAK used by jump(). Real loops
    // above remain LOOP, including their guard and CONTINUE target.
    const Operand selector = imm1(0);
    body_.inst(op(Sm4Opcode::SWITCH), {&selector});
    body_.inst(op(Sm4Opcode::DEFAULT), {});
    node_within(block, merges);
    close_construct();
    do_tree(follow);
}

void Lowerer::close_construct() {
    const Construct c = constructs_.back();
    constructs_.pop_back();
    // Falling off the end of either construct leaves it.
    if (!body_.last_was_jump()) body_.inst(op(Sm4Opcode::BREAK), {});
    body_.inst(op(c.loop ? Sm4Opcode::ENDLOOP : Sm4Opcode::ENDSWITCH), {});
    if (!c.passed || constructs_.empty()) return;
    // A branch from inside `c` to a construct further out set brk to its
    // id and broke out of `c`. If that is the parent, finish it here;
    // otherwise keep breaking outward.
    const Construct& parent = constructs_.back();
    const Slot t = fresh_scalar();
    const Operand td = dst_temp(t.reg, 1u << t.comp);
    const Operand b = src_temp_scalar(brk_reg_, 0);
    const Operand id = imm(parent.id);
    body_.inst(op(Sm4Opcode::IEQ), {&td, &b, &id});
    const Operand ts = src_temp_select(t.reg, t.comp);
    body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&ts});
    const Operand bd = dst_temp(brk_reg_, 1);
    const Operand zero = imm(0);
    body_.inst(op(Sm4Opcode::MOV), {&bd, &zero});
    body_.inst(op(parent.loop ? Sm4Opcode::CONTINUE : Sm4Opcode::BREAK), {});
    body_.inst(op(Sm4Opcode::ENDIF), {});
    if (parent.passed) {
        const Operand bs = src_temp_select(brk_reg_, 0);
        body_.inst(op(Sm4Opcode::BREAKC) | kTestNonZero, {&bs});
    }
}

void Lowerer::jump(u32 target_node, bool loop) {
    i64 t = -1;
    for (i64 i = static_cast<i64>(constructs_.size()) - 1; i >= 0; --i) {
        if (constructs_[i].loop == loop && constructs_[i].node == target_node) {
            t = i;
            break;
        }
    }
    if (t < 0) {
        malformed("branch target outside every construct");
        return;
    }
    if (t == static_cast<i64>(constructs_.size()) - 1) {
        body_.inst(op(loop ? Sm4Opcode::CONTINUE : Sm4Opcode::BREAK), {});
        return;
    }
    for (i64 i = t + 1; i < static_cast<i64>(constructs_.size()); ++i) constructs_[i].passed = true;
    const Operand bd = dst_temp(brk_reg_, 1);
    const Operand id = imm(constructs_[t].id);
    body_.inst(op(Sm4Opcode::MOV), {&bd, &id});
    body_.inst(op(Sm4Opcode::BREAK), {});
}

void Lowerer::emit_phi_copies(u32 from, u32 to) {
    std::vector<std::pair<u32, u32>> copies;  // phi result, incoming
    for (u32 index : fn_->blocks[to].instructions) {
        const DxilInstruction& inst = fn_->instructions[index];
        if (inst.op != DxilOpcode::Phi) break;
        for (usize k = 0; k < inst.blocks.size(); ++k) {
            if (inst.blocks[k] == from) {
                copies.push_back({inst.result, inst.operands[k]});
                break;
            }
        }
    }
    if (copies.empty()) return;
    if (copies.size() == 1) {
        const Operand d = dst(copies[0].first);
        const Operand s = src(copies[0].second);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return;
    }
    // A parallel copy: read every incoming value before writing any phi.
    std::vector<Slot> scratch;
    for (const auto& [phi, value] : copies) {
        auto it = phi_scratch_.find(phi);
        const bool pair = is_i64_value(phi);
        if (it == phi_scratch_.end()) {
            const Slot s = pair ? fresh_pair() : fresh_scalar();
            it = phi_scratch_.emplace(phi, s.reg * 4 + s.comp).first;
        }
        const Slot s{it->second / 4, it->second % 4};
        scratch.push_back(s);
        const Operand d = dst_temp(s.reg, (pair ? 3u : 1u) << s.comp);
        const Operand v = src(value);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
    }
    for (usize i = 0; i < copies.size(); ++i) {
        const Operand d = dst(copies[i].first);
        const Operand s = is_i64_value(copies[i].first)
                              ? src_temp(scratch[i].reg, scratch[i].comp == 0 ? 0x44u : 0xEEu)
                              : src_temp_scalar(scratch[i].reg, scratch[i].comp);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    }
}

void Lowerer::do_branch(u32 from, u32 to) {
    if (failed_) return;
    emit_phi_copies(from, to);
    if (rpo_[to] <= rpo_[from]) {
        jump(to, true);
    } else if (forward_in_[to] >= 2 || claimed_[to] != ~0u) {
        jump(to, false);
    } else {
        do_tree(to);
    }
}

void Lowerer::emit_terminator(u32 block) {
    if (failed_) return;
    const DxilInstruction& t = fn_->instructions[fn_->blocks[block].instructions.back()];
    switch (t.op) {
    case DxilOpcode::Ret:
    case DxilOpcode::Unreachable:
        body_.inst(op(Sm4Opcode::RET), {});
        return;
    case DxilOpcode::Br:
        if (t.blocks.size() == 1) {
            do_branch(block, t.blocks[0]);
            return;
        }
        if (auto bits = constant_bits(t.operands[0])) {
            do_branch(block, *bits != 0 ? t.blocks[0] : t.blocks[1]);
            return;
        }
        {
            const Slot c = slot(t.operands[0]);
            const Operand cs = src_temp_select(c.reg, c.comp);
            body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&cs});
            do_branch(block, t.blocks[0]);
            body_.inst(op(Sm4Opcode::ELSE), {});
            do_branch(block, t.blocks[1]);
            body_.inst(op(Sm4Opcode::ENDIF), {});
        }
        return;
    case DxilOpcode::Switch: {
        // An if_nz chain over the distinct case targets, default last.
        std::vector<std::pair<u32, std::vector<u64>>> targets;
        for (usize k = 1; k < t.blocks.size(); ++k) {
            const u32 target = t.blocks[k];
            if (target == t.blocks[0]) continue;
            auto it = std::find_if(targets.begin(), targets.end(),
                                   [&](const auto& e) { return e.first == target; });
            if (it == targets.end()) targets.push_back({target, {}}), it = targets.end() - 1;
            it->second.push_back(t.cases[k - 1]);
        }
        const Operand sel = src(t.operands[0]);
        for (const auto& [target, values] : targets) {
            const Slot c = fresh_scalar();
            const Operand cd = dst_temp(c.reg, 1u << c.comp);
            for (usize v = 0; v < values.size(); ++v) {
                const Operand value = imm(static_cast<u32>(values[v]));
                if (v == 0) {
                    body_.inst(op(Sm4Opcode::IEQ), {&cd, &sel, &value});
                } else {
                    const Slot e = fresh_scalar();
                    const Operand ed = dst_temp(e.reg, 1u << e.comp);
                    body_.inst(op(Sm4Opcode::IEQ), {&ed, &sel, &value});
                    const Operand a = src_temp_scalar(c.reg, c.comp);
                    const Operand b = src_temp_scalar(e.reg, e.comp);
                    body_.inst(op(Sm4Opcode::OR), {&cd, &a, &b});
                }
            }
            const Operand cs = src_temp_select(c.reg, c.comp);
            body_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&cs});
            do_branch(block, target);
            body_.inst(op(Sm4Opcode::ELSE), {});
        }
        do_branch(block, t.blocks[0]);
        for (usize k = 0; k < targets.size(); ++k) body_.inst(op(Sm4Opcode::ENDIF), {});
        return;
    }
    default:
        malformed("block without a terminator");
    }
}

void Lowerer::emit_block(u32 block) {
    const auto& list = fn_->blocks[block].instructions;
    for (usize i = 0; i + 1 < list.size() && !failed_; ++i) {
        emit_instruction(fn_->instructions[list[i]]);
    }
}

// =============================================================================
// Instructions
// =============================================================================

bool Lowerer::emit_instruction(const DxilInstruction& inst) {
    if (wants_wide_instruction(*host_, inst)) return lower_wide_instruction(*host_, inst);
    switch (inst.op) {
    case DxilOpcode::Phi:
        slot(inst.result);  // written on each incoming edge
        return true;
    case DxilOpcode::Binop: return emit_binop(inst);
    case DxilOpcode::Cmp: return emit_cmp(inst);
    case DxilOpcode::Cast: return emit_cast(inst);
    case DxilOpcode::Select: {
        if (!check_scalar32(inst.type, "select")) return false;
        const Operand d = dst(inst.result);
        const Operand c = src(inst.operands[0]);
        const Operand a = src(inst.operands[1]);
        const Operand b = src(inst.operands[2]);
        body_.inst(op(Sm4Opcode::MOVC), {&d, &c, &a, &b});
        return true;
    }
    case DxilOpcode::ExtractElement: {
        // A vector of up to four 32-bit scalars lives in one register
        // (vec), component k element k.
        u32 k = 0;
        if (!vector32(m_.type_of(fn_, inst.operands[0])) || !const_u32(inst.operands[1], k) || k > 3) {
            return unimplemented("extractelement of this vector or at a dynamic index");
        }
        const Operand d = dst(inst.result), v = vector_element(inst.operands[0], k);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    case DxilOpcode::InsertElement: {
        u32 at = 0;
        if (!vector32(inst.type) || !const_u32(inst.operands[2], at) || at > 3) {
            return unimplemented("insertelement into this vector or at a dynamic index");
        }
        const u32 r = vec(inst.result);
        const u32 n = static_cast<u32>(type(inst.type).count);
        for (u32 k = 0; k < n; ++k) {
            if (k != at && undef(inst.operands[0])) continue;
            const Operand d = dst_temp(r, 1u << k);
            const Operand v = k == at ? src(inst.operands[1]) : vector_element(inst.operands[0], k);
            body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        }
        return true;
    }
    case DxilOpcode::ExtractValue: {
        if (inst.indices.size() != 1) return unimplemented("nested extractvalue");
        const u32 agg = inst.operands[0];
        auto it = vec_regs_.find(agg);
        if (it == vec_regs_.end()) return unimplemented("extractvalue of this aggregate");
        if (inst.indices[0] == 4) {
            auto st = status_slots_.find(agg);
            if (st == status_slots_.end()) return unimplemented("the status of this resource operation");
            slots_[inst.result] = st->second;
            return true;
        }
        if (inst.indices[0] > 4) return malformed("extractvalue past a ResRet");
        slots_[inst.result] = Slot{it->second, inst.indices[0]};
        return true;
    }
    case DxilOpcode::Call: return emit_call(inst);
    case DxilOpcode::Alloca:
    case DxilOpcode::Gep:
    case DxilOpcode::Load:
    case DxilOpcode::Store:
    case DxilOpcode::AtomicRmw:
    case DxilOpcode::CmpXchg:
        return emit_memory(inst);
    default:
        return unimplemented("LLVM instruction form " + std::to_string(static_cast<u32>(inst.op)));
    }
}

bool Lowerer::emit_binop(const DxilInstruction& inst) {
    if (!check_scalar32(inst.type, "arithmetic")) return false;
    const bool fp = is_f32(inst.type);
    const u32 a = inst.operands[0], b = inst.operands[1];
    const Operand d = dst(inst.result);
    const auto bin = [&](Sm4Opcode o, u32 modb = 0, u32 flags = 0) {
        const Operand sa = src(a);
        const Operand sb = src(b, modb);
        body_.inst(op(o) | flags, {&d, &sa, &sb});
    };
    // DXIL.rst "Precise qualifier": a floating-point op without fast-math
    // flags is precise.
    const u32 precise = fp && (inst.flags & 1u) == 0 ? (1u << slot(inst.result).comp) << kPreciseShift : 0u;
    const Operand nul = null_operand();
    if (fp && preserve_denormals_) {
        // "fp32-denorm-mode"="preserve": VX_F32P (exact, so `precise` holds).
        using dxbc_vitreo_ext::F32POp;
        const Operand sa = src(a), sb = src(b), nb = src(b, 1);
        switch (static_cast<DxilBinop>(inst.sub)) {
        case DxilBinop::Add: f32p(F32POp::Add, d, {&sa, &sb}); return true;
        case DxilBinop::Sub: f32p(F32POp::Add, d, {&sa, &nb}); return true;
        case DxilBinop::Mul: f32p(F32POp::Mul, d, {&sa, &sb}); return true;
        case DxilBinop::SDiv: f32p(F32POp::Div, d, {&sa, &sb}); return true;
        case DxilBinop::SRem: {
            // frem: a - b * trunc(a / b); trunc of a denormal is its signed
            // zero, which the hardware's round_z gives.
            const Slot q = fresh_scalar();
            const Operand qd = dst_temp(q.reg, 1u << q.comp);
            const Operand qs = src_temp_scalar(q.reg, q.comp), nq = src_temp_scalar(q.reg, q.comp, 1);
            f32p(F32POp::Div, qd, {&sa, &sb});
            body_.inst(op(Sm4Opcode::ROUND_Z), {&qd, &qs});
            f32p(F32POp::Fma, d, {&nq, &sb, &sa});
            return true;
        }
        default: return unimplemented("float binop " + std::to_string(inst.sub));
        }
    }
    switch (static_cast<DxilBinop>(inst.sub)) {
    case DxilBinop::Add: fp ? bin(Sm4Opcode::ADD, 0, precise) : bin(Sm4Opcode::IADD); return true;
    case DxilBinop::Sub: fp ? bin(Sm4Opcode::ADD, 1, precise) : bin(Sm4Opcode::IADD, 1); return true;
    case DxilBinop::Mul:
        if (fp) {
            bin(Sm4Opcode::MUL, 0, precise);
        } else {
            const Operand sa = src(a), sb = src(b);
            body_.inst(op(Sm4Opcode::IMUL), {&nul, &d, &sa, &sb});
        }
        return true;
    case DxilBinop::SDiv:
        if (fp) {
            bin(Sm4Opcode::DIV, 0, precise);
            return true;
        }
        [[fallthrough]];
    case DxilBinop::SRem: {
        if (fp) {
            // frem: a - b * trunc(a / b), the fmod of D3D.
            const Slot q = fresh_scalar();
            const Operand qd = dst_temp(q.reg, 1u << q.comp);
            const Operand qs = src_temp_scalar(q.reg, q.comp, 1);
            const Operand sa = src(a), sb = src(b);
            body_.inst(op(Sm4Opcode::DIV) | precise, {&qd, &sa, &sb});
            const Operand qq = src_temp_scalar(q.reg, q.comp);
            body_.inst(op(Sm4Opcode::ROUND_Z), {&qd, &qq});
            body_.inst(op(Sm4Opcode::MAD) | precise, {&d, &qs, &sb, &sa});
            return true;
        }
        // Signed division through the unsigned one: |a| / |b|, then the
        // quotient takes sign(a) ^ sign(b) and the remainder sign(a).
        const Slot aa = fresh_scalar(), bb = fresh_scalar(), q = fresh_scalar(), s = fresh_scalar();
        const Operand aad = dst_temp(aa.reg, 1u << aa.comp), bbd = dst_temp(bb.reg, 1u << bb.comp);
        const Operand sa = src(a), sb = src(b), na = src(a, 1), nb = src(b, 1);
        body_.inst(op(Sm4Opcode::IMAX), {&aad, &sa, &na});
        body_.inst(op(Sm4Opcode::IMAX), {&bbd, &sb, &nb});
        const Operand qd = dst_temp(q.reg, 1u << q.comp);
        const Operand aas = src_temp_scalar(aa.reg, aa.comp), bbs = src_temp_scalar(bb.reg, bb.comp);
        const bool rem = static_cast<DxilBinop>(inst.sub) == DxilBinop::SRem;
        if (rem) body_.inst(op(Sm4Opcode::UDIV), {&nul, &qd, &aas, &bbs});
        else body_.inst(op(Sm4Opcode::UDIV), {&qd, &nul, &aas, &bbs});
        const Operand sd = dst_temp(s.reg, 1u << s.comp);
        const Operand zero = imm(0);
        if (rem) {
            body_.inst(op(Sm4Opcode::ILT), {&sd, &sa, &zero});
        } else {
            body_.inst(op(Sm4Opcode::XOR), {&sd, &sa, &sb});
            const Operand ss = src_temp_scalar(s.reg, s.comp);
            body_.inst(op(Sm4Opcode::ILT), {&sd, &ss, &zero});
        }
        // movc moves bits: a negate modifier on it would be the float one,
        // so the negated quotient is computed with ineg first.
        const Slot nq = fresh_scalar();
        const Operand nqd = dst_temp(nq.reg, 1u << nq.comp);
        const Operand qs = src_temp_scalar(q.reg, q.comp);
        body_.inst(op(Sm4Opcode::INEG), {&nqd, &qs});
        const Operand qn = src_temp_scalar(nq.reg, nq.comp);
        const Operand ss = src_temp_scalar(s.reg, s.comp);
        body_.inst(op(Sm4Opcode::MOVC), {&d, &ss, &qn, &qs});
        return true;
    }
    case DxilBinop::UDiv: {
        if (fp) return malformed("udiv on a float");
        const Operand sa = src(a), sb = src(b);
        body_.inst(op(Sm4Opcode::UDIV), {&d, &nul, &sa, &sb});
        return true;
    }
    case DxilBinop::URem: {
        if (fp) return malformed("urem on a float");
        const Operand sa = src(a), sb = src(b);
        body_.inst(op(Sm4Opcode::UDIV), {&nul, &d, &sa, &sb});
        return true;
    }
    case DxilBinop::Shl: bin(Sm4Opcode::ISHL); return true;
    case DxilBinop::LShr: bin(Sm4Opcode::USHR); return true;
    case DxilBinop::AShr: bin(Sm4Opcode::ISHR); return true;
    case DxilBinop::And: bin(Sm4Opcode::AND); return true;
    case DxilBinop::Or: bin(Sm4Opcode::OR); return true;
    case DxilBinop::Xor: bin(Sm4Opcode::XOR); return true;
    }
    return malformed("unknown binary operator");
}

bool Lowerer::emit_cmp(const DxilInstruction& inst) {
    const u32 a = inst.operands[0], b = inst.operands[1];
    const u32 t = m_.type_of(fn_, a);
    if (!check_scalar32(t, "comparison")) return false;
    const Operand d = dst(inst.result);
    const bool keep = preserve_denormals_ && is_f32(t);
    const auto cmp = [&](Sm4Opcode o, u32 x, u32 y, const Operand& to) {
        const Operand sx = src(x), sy = src(y);
        if (keep && inst.sub < 32) {
            // "fp32-denorm-mode"="preserve": denormals compare by value.
            using dxbc_vitreo_ext::F32POp;
            const F32POp k = o == Sm4Opcode::EQ ? F32POp::Eq
                             : o == Sm4Opcode::NE ? F32POp::Ne
                             : o == Sm4Opcode::LT ? F32POp::Lt
                                                  : F32POp::Ge;
            f32p(k, to, {&sx, &sy});
            return;
        }
        body_.inst(op(o), {&to, &sx, &sy});
    };
    const auto pred = static_cast<DxilPredicate>(inst.sub);
    if (inst.sub >= 32) {
        switch (pred) {
        case DxilPredicate::IEq: cmp(Sm4Opcode::IEQ, a, b, d); break;
        case DxilPredicate::INe: cmp(Sm4Opcode::INE, a, b, d); break;
        case DxilPredicate::ISlt: cmp(Sm4Opcode::ILT, a, b, d); break;
        case DxilPredicate::ISge: cmp(Sm4Opcode::IGE, a, b, d); break;
        case DxilPredicate::ISgt: cmp(Sm4Opcode::ILT, b, a, d); break;
        case DxilPredicate::ISle: cmp(Sm4Opcode::IGE, b, a, d); break;
        case DxilPredicate::IUlt: cmp(Sm4Opcode::ULT, a, b, d); break;
        case DxilPredicate::IUge: cmp(Sm4Opcode::UGE, a, b, d); break;
        case DxilPredicate::IUgt: cmp(Sm4Opcode::ULT, b, a, d); break;
        case DxilPredicate::IUle: cmp(Sm4Opcode::UGE, b, a, d); break;
        default: return malformed("unknown integer predicate");
        }
        return true;
    }
    // DXBC eq / lt / ge are ordered (false on NaN), ne unordered.
    const Slot dslot = slot(inst.result);
    const auto negate = [&]() {
        const Operand s = src_temp_scalar(dslot.reg, dslot.comp);
        body_.inst(op(Sm4Opcode::NOT), {&d, &s});
    };
    const auto both = [&](Sm4Opcode o1, u32 x1, u32 y1, Sm4Opcode o2, u32 x2, u32 y2, Sm4Opcode join) {
        const Slot e = fresh_scalar();
        const Operand ed = dst_temp(e.reg, 1u << e.comp);
        cmp(o1, x1, y1, d);
        cmp(o2, x2, y2, ed);
        const Operand s1 = src_temp_scalar(dslot.reg, dslot.comp), s2 = src_temp_scalar(e.reg, e.comp);
        body_.inst(op(join), {&d, &s1, &s2});
    };
    switch (pred) {
    case DxilPredicate::FFalse: {
        const Operand z = imm(0);
        body_.inst(op(Sm4Opcode::MOV), {&d, &z});
        break;
    }
    case DxilPredicate::FTrue: {
        const Operand o = imm(0xFFFFFFFFu);
        body_.inst(op(Sm4Opcode::MOV), {&d, &o});
        break;
    }
    case DxilPredicate::FOeq: cmp(Sm4Opcode::EQ, a, b, d); break;
    case DxilPredicate::FUne: cmp(Sm4Opcode::NE, a, b, d); break;
    case DxilPredicate::FOlt: cmp(Sm4Opcode::LT, a, b, d); break;
    case DxilPredicate::FOge: cmp(Sm4Opcode::GE, a, b, d); break;
    case DxilPredicate::FOgt: cmp(Sm4Opcode::LT, b, a, d); break;
    case DxilPredicate::FOle: cmp(Sm4Opcode::GE, b, a, d); break;
    case DxilPredicate::FUge: cmp(Sm4Opcode::LT, a, b, d); negate(); break;
    case DxilPredicate::FUlt: cmp(Sm4Opcode::GE, a, b, d); negate(); break;
    case DxilPredicate::FUle: cmp(Sm4Opcode::LT, b, a, d); negate(); break;
    case DxilPredicate::FUgt: cmp(Sm4Opcode::GE, b, a, d); negate(); break;
    case DxilPredicate::FOne: both(Sm4Opcode::LT, a, b, Sm4Opcode::LT, b, a, Sm4Opcode::OR); break;
    case DxilPredicate::FUeq:
        both(Sm4Opcode::LT, a, b, Sm4Opcode::LT, b, a, Sm4Opcode::OR);
        negate();
        break;
    case DxilPredicate::FOrd: both(Sm4Opcode::EQ, a, a, Sm4Opcode::EQ, b, b, Sm4Opcode::AND); break;
    case DxilPredicate::FUno: both(Sm4Opcode::NE, a, a, Sm4Opcode::NE, b, b, Sm4Opcode::OR); break;
    default: return malformed("unknown float predicate");
    }
    return true;
}

bool Lowerer::emit_cast(const DxilInstruction& inst) {
    const u32 x = inst.operands[0];
    const u32 from = m_.type_of(fn_, x), to = inst.type;
    // bitcast T* to U* between 32-bit scalars (dxc's i32* <-> float* when a
    // value is stored or loaded through a reinterpreted struct member, as in
    // a mesh payload read as another struct of the same layout): the same
    // 32-bit slot, read as the other type -- every slot is untyped bits.
    const auto word_pointer = [&](u32 t) {
        if (t >= m_.types.size() || type(t).kind != DxilTypeKind::Pointer) return false;
        const u32 e = type(t).element;
        return is_int(e, 32) ||
               (e < m_.types.size() && type(e).kind == DxilTypeKind::Float);
    };
    if (word_pointer(from) && word_pointer(to)) {
        Pointer p;
        if (!resolve_pointer(x, p)) return false;
        pointers_[inst.result] = p;
        return true;
    }
    if (from < m_.types.size() && to < m_.types.size() && type(from).kind == DxilTypeKind::Pointer &&
        type(to).kind == DxilTypeKind::Pointer) {
        // A pointer bitcast between aggregates of as many register-model
        // scalars (a raytracing payload or attribute struct): the same
        // elements, the register model is untyped. A mesh payload is
        // addressed in bytes, where equal scalar counts do not make equal
        // layouts.
        if (scalar_count(m_, type(from).element) != scalar_count(m_, type(to).element)) {
            return unimplemented("a pointer cast between element sizes");
        }
        Pointer p;
        if (!resolve_pointer(x, p)) return false;
        if (p.payload) return unimplemented("a mesh payload pointer cast between aggregates");
        pointers_[inst.result] = p;
        return true;
    }
    if (!check_scalar32(from, "conversion") || !check_scalar32(to, "conversion")) return false;
    const Operand d = dst(inst.result);
    const Operand s = src(x);
    switch (static_cast<DxilCast>(inst.sub)) {
    case DxilCast::Trunc: {
        if (!is_int(to, 1)) return malformed("trunc to a non-i1 32-bit type");
        const Operand one = imm(1);
        body_.inst(op(Sm4Opcode::AND), {&d, &s, &one});
        const Operand ds = src(inst.result);
        const Operand zero = imm(0);
        body_.inst(op(Sm4Opcode::INE), {&d, &ds, &zero});
        return true;
    }
    case DxilCast::ZExt: {
        const Operand one = imm(1);
        body_.inst(op(Sm4Opcode::AND), {&d, &s, &one});
        return true;
    }
    case DxilCast::SExt:
    case DxilCast::BitCast:
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return true;
    case DxilCast::FPToSI: body_.inst(op(Sm4Opcode::FTOI), {&d, &s}); return true;
    case DxilCast::FPToUI: body_.inst(op(Sm4Opcode::FTOU), {&d, &s}); return true;
    case DxilCast::SIToFP:
    case DxilCast::UIToFP:
        if (is_int(from, 1)) {
            // i1 is a 0 / ~0 mask: 1.0 or -1.0 where set.
            const bool sign = static_cast<DxilCast>(inst.sub) == DxilCast::SIToFP;
            const Operand bits = imm(sign ? 0xBF800000u : 0x3F800000u);
            body_.inst(op(Sm4Opcode::AND), {&d, &s, &bits});
        } else {
            body_.inst(op(static_cast<DxilCast>(inst.sub) == DxilCast::SIToFP ? Sm4Opcode::ITOF
                                                                             : Sm4Opcode::UTOF),
                       {&d, &s});
        }
        return true;
    default:
        return unimplemented("cast " + std::to_string(inst.sub));
    }
}

// ---- memory ----

u32 scalar_count(const DxilModule& m, u32 t) {
    const DxilType& ty = m.types[t];
    switch (ty.kind) {
    case DxilTypeKind::Integer:
        return ty.width <= 32 ? 1u : ty.width == 64 ? 2u : 0u;  // i64: a pair
    case DxilTypeKind::Float:
    case DxilTypeKind::Half: return 1;
    case DxilTypeKind::Double: return 2;  // a pair, as an i64
    case DxilTypeKind::Array:
    case DxilTypeKind::Vector: {
        const u32 e = scalar_count(m, ty.element);
        return e == 0 ? 0u : static_cast<u32>(ty.count) * e;
    }
    case DxilTypeKind::Struct: {
        // Members back to back in the scalar element model (a raytracing
        // payload or attribute struct, an alloca of a struct).
        u32 total = 0;
        for (const u32 member : ty.members) {
            const u32 e = scalar_count(m, member);
            if (e == 0) return 0;
            total += e;
        }
        return total;
    }
    default: return 0;
    }
}

/// GEP step into aggregate @p t by index @p index (a constant for a
/// struct): the element type in @p t, the scalar offset of element 0 of
/// that index in @p base (structs) or the stride in @p stride (arrays and
/// vectors). False when @p t is not an aggregate.
bool gep_step(const DxilModule& m, u32& t, u32 index, bool constant, u32& base, u32& stride) {
    const DxilType& ty = m.types[t];
    if (ty.kind == DxilTypeKind::Struct) {
        if (!constant || index >= ty.members.size()) return false;
        base = 0;
        for (u32 k = 0; k < index; ++k) base += scalar_count(m, ty.members[k]);
        t = ty.members[index];
        stride = 0;
        return true;
    }
    if (ty.kind != DxilTypeKind::Array && ty.kind != DxilTypeKind::Vector) return false;
    t = ty.element;
    base = 0;
    stride = scalar_count(m, t);
    return true;
}

/// The memory layout of DXIL type @p t (its data layout, "i1:32-i16:16-
/// i32:32-i64:64-f16:16-f32:32-f64:64": every scalar naturally aligned; an
/// array strides by its element's size; a structure aligns each member and
/// rounds its size to its largest alignment) -- how a mesh payload's bytes
/// are laid out (DirectX-Specs "Mesh Shader": the runtime matches the AS
/// and MS payload sizes in bytes, so a payload moves as this byte image).
bool byte_layout(const DxilModule& m, u32 t, u32& size, u32& align) {
    const DxilType& ty = m.types[t];
    switch (ty.kind) {
    case DxilTypeKind::Integer:
        if (ty.width == 16) size = align = 2;
        else if (ty.width == 64) size = align = 8;
        else if (ty.width <= 32) size = align = 4;
        else return false;
        return true;
    case DxilTypeKind::Half: size = align = 2; return true;
    case DxilTypeKind::Float: size = align = 4; return true;
    case DxilTypeKind::Double: size = align = 8; return true;
    case DxilTypeKind::Array: {
        u32 es = 0, ea = 0;
        if (!byte_layout(m, ty.element, es, ea)) return false;
        size = static_cast<u32>(ty.count) * es;
        align = ea;
        return true;
    }
    case DxilTypeKind::Struct: {
        u32 at = 0, a = 1;
        for (u32 member : ty.members) {
            u32 ms = 0, ma = 0;
            if (!byte_layout(m, member, ms, ma)) return false;
            at = (at + ma - 1) / ma * ma + ms;
            a = std::max(a, ma);
        }
        size = (at + a - 1) / a * a;
        align = a;
        return true;
    }
    default: return false;
    }
}

/// The byte offset of member @p k of structure @p t (byte_layout).
u32 member_byte_offset(const DxilModule& m, u32 t, u32 k) {
    u32 at = 0;
    for (u32 j = 0; j < m.types[t].members.size(); ++j) {
        u32 ms = 0, ma = 1;
        (void)byte_layout(m, m.types[t].members[j], ms, ma);
        at = (at + ma - 1) / ma * ma;
        if (j == k) break;
        at += ms;
    }
    return at;
}

/// Each register-model scalar of @p t (scalar_count's order) as
/// `byte offset | kind << 16` (dxbc_parser.hpp kDxbcPayload*), from @p base.
bool payload_scalars(const DxilModule& m, u32 t, u32 base, std::vector<u32>& out) {
    const DxilType& ty = m.types[t];
    switch (ty.kind) {
    case DxilTypeKind::Integer:
        if (ty.width == 16) {
            out.push_back(base | (kDxbcPayloadU16 << 16));
        } else if (ty.width == 64) {
            out.push_back(base);
            out.push_back(base + 4);
        } else {
            out.push_back(base);
        }
        return ty.width <= 64;
    case DxilTypeKind::Half: out.push_back(base | (kDxbcPayloadF16 << 16)); return true;
    case DxilTypeKind::Float: out.push_back(base); return true;
    case DxilTypeKind::Double:
        out.push_back(base);
        out.push_back(base + 4);
        return true;
    case DxilTypeKind::Array: {
        u32 es = 0, ea = 0;
        if (!byte_layout(m, ty.element, es, ea)) return false;
        for (u64 i = 0; i < ty.count; ++i) {
            if (!payload_scalars(m, ty.element, base + static_cast<u32>(i) * es, out)) return false;
        }
        return true;
    }
    case DxilTypeKind::Struct:
        for (u32 k = 0; k < ty.members.size(); ++k) {
            if (!payload_scalars(m, ty.members[k], base + member_byte_offset(m, t, k), out)) return false;
        }
        return true;
    default: return false;
    }
}

bool Lowerer::resolve_pointer(u32 id, Pointer& out) {
    if (auto it = pointers_.find(id); it != pointers_.end()) {
        out = it->second;
        return true;
    }
    const DxilValue* v = m_.value(fn_, id);
    if (v == nullptr) return malformed("pointer operand is not a value");
    if (v->kind == DxilValueKind::Global) {
        const DxilGlobal& g = m_.globals[v->index];
        if (g.address_space == 0 && g.is_constant && g.initializer != ~0u) return constant_global(v->index, out);
        if (g.address_space != 3) return unimplemented("a pointer into address space " + std::to_string(g.address_space));
        auto it = tgsm_ids_.find(v->index);
        if (it == tgsm_ids_.end()) {
            const u32 count = scalar_count(m_, g.value_type);
            if (count == 0) return unimplemented("groupshared " + dxil_type_name(m_, g.value_type));
            it = tgsm_ids_.emplace(v->index, static_cast<u32>(tgsm_sizes_.size())).first;
            tgsm_sizes_.push_back(count);
        }
        out = Pointer{true, it->second, 0, false, {}};
        return true;
    }
    if (v->kind == DxilValueKind::Constant) {
        const DxilConstant* c = m_.constant(fn_, id);
        if (c == nullptr || c->kind != DxilConstantKind::GepExpr || c->elements.empty()) {
            return unimplemented("this constant pointer expression");
        }
        Pointer base;
        if (!resolve_pointer(static_cast<u32>(c->elements[0]), base)) return false;
        u32 t = m_.types[m_.type_of(fn_, static_cast<u32>(c->elements[0]))].element;
        for (usize k = 1; k < c->elements.size(); ++k) {
            u32 index = 0;
            if (!const_u32(static_cast<u32>(c->elements[k]), index)) return malformed("non-constant constant GEP");
            if (k == 1) {
                base.constant += index * scalar_count(m_, t);
            } else if (type(t).kind == DxilTypeKind::Struct) {
                if (index >= type(t).members.size()) return malformed("GEP past a structure's members");
                for (u32 k2 = 0; k2 < index; ++k2) base.constant += scalar_count(m_, type(t).members[k2]);
                t = type(t).members[index];
            } else {
                u32 at = 0, stride = 0;
                if (!gep_step(m_, t, index, true, at, stride)) return unimplemented("this constant GEP");
                base.constant += at + index * stride;
            }
        }
        out = base;
        return true;
    }
    return unimplemented("this pointer source");
}

bool Lowerer::flatten_constant(u32 id, u32 t, std::vector<u32>& out) {
    // The 32-bit words of constant @p id of type @p t in the register
    // model: i1 0 / ~0, i16 zero-extended, half as its float32, i64 and
    // double as (low, high).
    const DxilConstant* c = m_.constant(fn_, id);
    if (c == nullptr) return malformed("a global initializer that is not a constant");
    const DxilType& ty = type(t);
    const auto scalar = [&](u64 bits) {
        if (ty.kind == DxilTypeKind::Half) {
            out.push_back(half_bits_to_float_bits(static_cast<u32>(bits)));
        } else if (ty.kind == DxilTypeKind::Integer && ty.width == 1) {
            out.push_back((bits & 1u) != 0 ? ~0u : 0u);
        } else if (ty.kind == DxilTypeKind::Integer && ty.width == 16) {
            out.push_back(static_cast<u32>(bits) & 0xFFFFu);
        } else if ((ty.kind == DxilTypeKind::Integer && ty.width == 64) || ty.kind == DxilTypeKind::Double) {
            out.push_back(static_cast<u32>(bits));
            out.push_back(static_cast<u32>(bits >> 32));
        } else {
            out.push_back(static_cast<u32>(bits));
        }
    };
    switch (c->kind) {
    case DxilConstantKind::Null:
    case DxilConstantKind::Undef:
        out.insert(out.end(), scalar_count(m_, t), 0u);
        return true;
    case DxilConstantKind::Integer:
    case DxilConstantKind::Float: scalar(c->bits); return true;
    case DxilConstantKind::Data: {
        if (ty.kind != DxilTypeKind::Array && ty.kind != DxilTypeKind::Vector) {
            return unimplemented("constant data of " + dxil_type_name(m_, t));
        }
        const u32 element = ty.element;
        const DxilType& et = type(element);
        for (u64 bits : c->elements) {
            if (et.kind == DxilTypeKind::Half) out.push_back(half_bits_to_float_bits(static_cast<u32>(bits)));
            else if (et.kind == DxilTypeKind::Integer && et.width == 16) out.push_back(static_cast<u32>(bits) & 0xFFFFu);
            else if ((et.kind == DxilTypeKind::Integer && et.width == 64) || et.kind == DxilTypeKind::Double) {
                out.push_back(static_cast<u32>(bits));
                out.push_back(static_cast<u32>(bits >> 32));
            } else {
                out.push_back(static_cast<u32>(bits));
            }
        }
        return true;
    }
    case DxilConstantKind::Aggregate: {
        if (ty.kind != DxilTypeKind::Array) return unimplemented("a constant aggregate of " + dxil_type_name(m_, t));
        for (u64 e : c->elements) {
            if (!flatten_constant(static_cast<u32>(e), ty.element, out)) return false;
        }
        return true;
    }
    default: return unimplemented("a global initializer of this constant kind");
    }
}

bool Lowerer::constant_global(u32 global, Pointer& out) {
    // An HLSL static const array: a read-only global with an initializer
    // (LLVM 3.7 LangRef, Global Variables; DXIL.rst Immediate Constant Buffer).
    // Keep its register-model words immutable instead of filling x# per invocation.
    auto it = constant_globals_.find(global);
    if (it == constant_globals_.end()) {
        const DxilGlobal& g = m_.globals[global];
        std::vector<u32> words;
        if (!flatten_constant(g.initializer, g.value_type, words)) return false;
        if (words.empty() || words.size() != scalar_count(m_, g.value_type)) {
            return unimplemented("constant global " + dxil_type_name(m_, g.value_type));
        }
        // Internal readonly slots have a separate namespace from mutable x#.
        // Hull phases reset their mutable allocations, but these globals are
        // module-owned and serialized once with the exact shader cache input.
        const u32 x = 0x80000000u | static_cast<u32>(readonly_initializers_.size());
        readonly_initializers_.emplace(x, std::move(words));
        it = constant_globals_.emplace(global, x).first;
    }
    out = Pointer{false, it->second, 0, false, {}};
    return true;
}

Index Lowerer::pointer_index(const Pointer& p) {
    if (!p.has_reg) return Index{false, p.constant, 0, 0};
    return Index{true, p.constant, p.reg.reg, p.reg.comp};
}

Operand Lowerer::pointer_index_src(const Pointer& p) {
    if (!p.has_reg) return imm(p.constant);
    if (p.constant == 0) return src_temp_scalar(p.reg.reg, p.reg.comp);
    const Slot s = fresh_scalar();
    const Operand d = dst_temp(s.reg, 1u << s.comp);
    const Operand r = src_temp_scalar(p.reg.reg, p.reg.comp);
    const Operand c = imm(p.constant);
    body_.inst(op(Sm4Opcode::IADD), {&d, &r, &c});
    return src_temp_scalar(s.reg, s.comp);
}

bool Lowerer::emit_memory(const DxilInstruction& inst) {
    if (rt_ != nullptr) {
        bool handled = false;
        if (!rt_memory(inst, handled)) return false;
        if (handled) return true;
    }
    switch (inst.op) {
    case DxilOpcode::Alloca: {
        const u32 count = scalar_count(m_, inst.alloca_type);
        if (count == 0) return unimplemented("alloca of " + dxil_type_name(m_, inst.alloca_type));
        const u32 x = static_cast<u32>(indexable_sizes_.size());
        indexable_sizes_.push_back(count);
        pointers_[inst.result] = Pointer{false, x, 0, false, {}};
        return true;
    }
    case DxilOpcode::Gep: {
        Pointer p;
        if (!resolve_pointer(inst.operands[0], p)) return false;
        u32 t = inst.alloca_type;  // source element type
        // The mesh payload is addressed in bytes (byte_layout), every
        // other pointer in register-model scalars.
        const auto stride_of = [&](u32 e) {
            if (!p.payload) return scalar_count(m_, e);
            u32 size = 0, align = 0;
            return byte_layout(m_, e, size, align) ? size : 0u;
        };
        for (usize k = 1; k < inst.operands.size(); ++k) {
            u32 stride = 0;
            const u32 idx = inst.operands[k];
            if (k == 1) {
                stride = stride_of(t);
            } else if (type(t).kind == DxilTypeKind::Struct) {
                // A structure member index is a constant (LLVM LangRef,
                // getelementptr): the offset of the members before it.
                const auto member = constant_bits(inst.operands[k]);
                if (!member || *member >= type(t).members.size()) {
                    return malformed("GEP into a structure by a non-constant or past its members");
                }
                if (p.payload) {
                    p.constant += member_byte_offset(m_, t, *member);
                } else {
                    for (u32 k2 = 0; k2 < *member; ++k2) p.constant += scalar_count(m_, type(t).members[k2]);
                }
                t = type(t).members[static_cast<usize>(*member)];
                continue;
            } else {
                if (p.payload) {
                    // byte_layout has arrays, not vectors (payload members
                    // are scalars, arrays and structures).
                    if (type(t).kind != DxilTypeKind::Array) return unimplemented("GEP into a vector");
                    t = type(t).element;
                    stride = stride_of(t);
                } else {
                    const auto bits = constant_bits(idx);
                    u32 at = 0;
                    if (!gep_step(m_, t, bits ? *bits : 0u, bits.has_value(), at, stride)) {
                        return unimplemented("GEP over " + dxil_type_name(m_, t));
                    }
                    if (stride == 0) {  // a struct member: a constant offset
                        p.constant += at;
                        continue;
                    }
                }
            }
            if (stride == 0) return unimplemented("GEP over " + dxil_type_name(m_, t));
            if (auto bits = constant_bits(idx)) {
                p.constant += *bits * stride;
                continue;
            }
            const Slot s = fresh_scalar();
            const Operand d = dst_temp(s.reg, 1u << s.comp);
            const Operand i = src(idx);
            const Operand st = imm(stride);
            if (p.has_reg) {
                const Operand r = src_temp_scalar(p.reg.reg, p.reg.comp);
                body_.inst(op(Sm4Opcode::IMAD), {&d, &i, &st, &r});
            } else if (stride == 1) {
                body_.inst(op(Sm4Opcode::MOV), {&d, &i});
            } else {
                const Operand nul = null_operand();
                body_.inst(op(Sm4Opcode::IMUL), {&nul, &d, &i, &st});
            }
            p.has_reg = true;
            p.reg = s;
        }
        pointers_[inst.result] = p;
        return true;
    }
    case DxilOpcode::Load: {
        if (vector32(inst.type)) {
            Pointer p;
            if (!resolve_pointer(inst.operands[0], p)) return false;
            if (p.tgsm) return unimplemented("a vector load from groupshared memory");
            const u32 r = vec(inst.result);
            for (u32 k = 0; k < type(inst.type).count; ++k) {
                const Index at{p.has_reg, p.constant + k, p.reg.reg, p.reg.comp};
                const Operand d = dst_temp(r, 1u << k), x = indexable(p.object, at, 0x00, false);
                body_.inst(op(Sm4Opcode::MOV), {&d, &x});
            }
            return true;
        }
        if (!check_scalar32(inst.type, "load")) return false;
        Pointer p;
        if (!resolve_pointer(inst.operands[0], p)) return false;
        const Operand d = dst(inst.result);
        if (p.payload) {
            const DxilType& lt = type(inst.type);
            const u32 kind = lt.kind == DxilTypeKind::Half                          ? kDxbcPayloadF16
                             : lt.kind == DxilTypeKind::Integer && lt.width == 16 ? kDxbcPayloadU16
                                                                                  : kDxbcPayloadWord;
            return emit_payload_load(p, d, kind);
        }
        if (p.tgsm) {
            const Operand i = pointer_index_src(p);
            const Operand zero = imm(0);
            const Operand g = tgsm(p.object, 0x00, false);
            body_.inst(op(Sm4Opcode::LD_STRUCTURED), {&d, &i, &zero, &g});
        } else {
            const Operand x = indexable(p.object, pointer_index(p), 0x00, false);
            body_.inst(op(Sm4Opcode::MOV), {&d, &x});
        }
        return true;
    }
    case DxilOpcode::Store: {
        const u32 vt = m_.type_of(fn_, inst.operands[1]);
        if (vector32(vt)) {
            Pointer p;
            if (!resolve_pointer(inst.operands[0], p)) return false;
            if (!p.tgsm && !p.payload && readonly_initializers_.contains(p.object))
                return malformed("a store to a constant global");
            if (p.tgsm) return unimplemented("a vector store to groupshared memory");
            for (u32 k = 0; k < type(vt).count; ++k) {
                const Index at{p.has_reg, p.constant + k, p.reg.reg, p.reg.comp};
                const Operand x = indexable(p.object, at, 0x1, true), v = vector_element(inst.operands[1], k);
                body_.inst(op(Sm4Opcode::MOV), {&x, &v});
            }
            return true;
        }
        if (!check_scalar32(vt, "store")) return false;
        Pointer p;
        if (!resolve_pointer(inst.operands[0], p)) return false;
        if (p.payload) return malformed("a store to the mesh payload (an input)");
        if (!p.tgsm && readonly_initializers_.contains(p.object))
            return malformed("a store to a constant global");
        const Operand v = src(inst.operands[1]);
        if (p.tgsm) {
            const Operand i = pointer_index_src(p);
            const Operand zero = imm(0);
            const Operand g = tgsm(p.object, 0x1, true);
            body_.inst(op(Sm4Opcode::STORE_STRUCTURED), {&g, &i, &zero, &v});
        } else {
            const Operand x = indexable(p.object, pointer_index(p), 0x1, true);
            body_.inst(op(Sm4Opcode::MOV), {&x, &v});
        }
        return true;
    }
    case DxilOpcode::AtomicRmw:
    case DxilOpcode::CmpXchg: {
        Pointer p;
        if (!resolve_pointer(inst.operands[0], p)) return false;
        if (p.payload) return malformed("an atomic on the mesh payload (an input)");
        if (!p.tgsm) return unimplemented("atomics on thread-local memory");
        const u32 vt = m_.type_of(fn_, inst.operands[1]);
        if (!is_int(vt, 32)) return unimplemented("64-bit groupshared atomics");
        // Structured address (element, byte offset).
        const u32 addr = fresh_vector();
        {
            const Operand ax = dst_temp(addr, 0x1), ay = dst_temp(addr, 0x2);
            const Operand i = pointer_index_src(p);
            const Operand zero = imm(0);
            body_.inst(op(Sm4Opcode::MOV), {&ax, &i});
            body_.inst(op(Sm4Opcode::MOV), {&ay, &zero});
        }
        const Operand a = src_temp(addr, 0x54);  // .xyyy
        const Operand g = tgsm(p.object, 0xF, true);
        if (inst.op == DxilOpcode::CmpXchg) {
            const u32 r = vec(inst.result);
            const Operand d = dst_temp(r, 0x1);
            const Operand cmpv = src(inst.operands[1]), newv = src(inst.operands[2]);
            body_.inst(op(Sm4Opcode::IMM_ATOMIC_CMP_EXCH), {&d, &g, &a, &cmpv, &newv});
            const Operand sd = dst_temp(r, 0x2);
            const Operand old = src_temp_scalar(r, 0);
            body_.inst(op(Sm4Opcode::IEQ), {&sd, &old, &cmpv});
            return true;
        }
        Sm4Opcode o;
        switch (static_cast<DxilRmw>(inst.sub)) {
        case DxilRmw::Xchg: o = Sm4Opcode::IMM_ATOMIC_EXCH; break;
        case DxilRmw::Add: o = Sm4Opcode::IMM_ATOMIC_IADD; break;
        case DxilRmw::And: o = Sm4Opcode::IMM_ATOMIC_AND; break;
        case DxilRmw::Or: o = Sm4Opcode::IMM_ATOMIC_OR; break;
        case DxilRmw::Xor: o = Sm4Opcode::IMM_ATOMIC_XOR; break;
        case DxilRmw::Max: o = Sm4Opcode::IMM_ATOMIC_IMAX; break;
        case DxilRmw::Min: o = Sm4Opcode::IMM_ATOMIC_IMIN; break;
        case DxilRmw::UMax: o = Sm4Opcode::IMM_ATOMIC_UMAX; break;
        case DxilRmw::UMin: o = Sm4Opcode::IMM_ATOMIC_UMIN; break;
        default: return unimplemented("atomicrmw operation " + std::to_string(inst.sub));
        }
        const Operand d = dst(inst.result);
        const Operand v = src(inst.operands[1]);
        body_.inst(op(o), {&d, &g, &a, &v});
        return true;
    }
    default:
        return malformed("not a memory instruction");
    }
}

// ---- dx.op ----

bool Lowerer::handle_of(u32 id, Handle& out) {
    auto it = handles_.find(id);
    if (it == handles_.end()) return unimplemented("a resource handle not made by createHandle (phi / select of handles)");
    out = it->second;
    return true;
}

Operand Lowerer::resource_src(const Handle& h, u32 swizzle) {
    const Sm4OperandType t = h.res->cls == ResourceClass::UAV ? Sm4OperandType::UnorderedAccessView
                             : h.res->cls == ResourceClass::Sampler ? Sm4OperandType::Sampler
                                                                    : Sm4OperandType::Resource;
    return resource(t, h.res->id, h.reg, h.nonuniform, swizzle);
}

bool Lowerer::coords_vector(const std::vector<u32>& ids, u32& reg, u32 extra_id, u32 extra_at) {
    reg = fresh_vector();
    for (usize k = 0; k < ids.size() && k < 4; ++k) {
        if (undef(ids[k])) continue;
        const Operand d = dst_temp(reg, 1u << k);
        const Operand s = src(ids[k]);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    }
    if (extra_id != ~0u) {
        const Operand d = dst_temp(reg, 1u << extra_at);
        const Operand s = undef(extra_id) ? imm(0) : src(extra_id);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    }
    return true;
}

void Lowerer::store_values(u32 reg, const DxilInstruction& inst, usize first, u32 mask) {
    for (u32 k = 0; k < 4; ++k) {
        if ((mask & (1u << k)) == 0) continue;
        const Operand d = dst_temp(reg, 1u << k);
        const Operand s = src(inst.operands[first + k]);
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
    }
}

bool Lowerer::emit_call(const DxilInstruction& inst) {
    const DxilFunction* callee = m_.function_of(inst.operands[0]);
    if (callee == nullptr) return malformed("indirect call");
    if (callee->name.rfind("dx.op.", 0) != 0) {
        if (callee->name.rfind("llvm.", 0) == 0) return unimplemented("intrinsic " + callee->name);
        return unimplemented("a call to " + callee->name + " (DXIL functions are not inlined)");
    }
    u32 opcode = 0;
    if (inst.operands.size() < 2 || !const_u32(inst.operands[1], opcode)) {
        return malformed("dx.op call without a constant opcode");
    }
    if (!bypass_ext_) {
        if (is_wave_opcode(opcode)) return lower_wave_call(*host_, opcode, inst);
        if (is_feedback_opcode(opcode)) return lower_feedback_call(*host_, opcode, inst);
        // GetMeshPayload / DispatchMesh take or return the payload pointer.
        if (opcode != 170 && opcode != 173 && wants_wide_call(*host_, *callee, inst)) {
            return lower_wide_call(*host_, opcode, inst);
        }
    }
    if (rt_ != nullptr) {
        bool handled = false;
        if (!emit_rt_op(opcode, inst, handled)) return false;
        if (handled) return !failed_;
    }
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };  // arg(1) = first after opcode
    const Operand nul = null_operand();
    const auto unary = [&](Sm4Opcode o, u32 modifier = 0, u32 flags = 0) {
        const Operand d = dst(inst.result);
        const Operand s = src(arg(1), modifier);
        body_.inst(op(o) | flags, {&d, &s});
        return true;
    };
    const auto binary = [&](Sm4Opcode o) {
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1)), b = src(arg(2));
        body_.inst(op(o), {&d, &a, &b});
        return true;
    };
    const auto tertiary = [&](Sm4Opcode o) {
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1)), b = src(arg(2)), c = src(arg(3));
        body_.inst(op(o), {&d, &a, &b, &c});
        return true;
    };
    const auto system = [&](Sm4OperandType type, bool scalar) {
        system_inputs_.insert(static_cast<u32>(type));
        u32 comp = 0;
        if (!scalar && !const_u32(arg(1), comp)) return unimplemented("a dynamic system-value component");
        const Operand d = dst(inst.result);
        if (quad_layout_ == QuadLayout::Grid && type != Sm4OperandType::InputThreadGroupId) {
            if (comp > 2) return malformed("a thread ID component above z");
            system_inputs_.insert(static_cast<u32>(Sm4OperandType::InputThreadIdInGroupFlattened));
            system_inputs_.insert(static_cast<u32>(Sm4OperandType::InputThreadGroupId));
            grid_thread_id(type == Sm4OperandType::InputThreadIdInGroup ? comp
                           : type == Sm4OperandType::InputThreadIdInGroupFlattened ? 3u
                                                                                    : 4u + comp,
                           d);
            return true;
        }
        const Operand s = special_src(type, swizzle_of(comp));
        body_.inst(op(Sm4Opcode::MOV), {&d, &s});
        return true;
    };
    const bool handle_op = opcode == 57 || opcode == 216 || opcode == 217 || opcode == 218 ||
                           opcode == 170;  // 170: GetMeshPayload
    if (inst.result != ~0u && !handle_op && !scalar32(inst.type) &&
        type(inst.type).kind != DxilTypeKind::Struct) {
        return check_scalar32(inst.type, ("dx.op " + std::to_string(opcode)).c_str());
    }
    if (inst.result != ~0u && scalar32(inst.type) == false && type(inst.type).kind == DxilTypeKind::Struct) {
        // Aggregate results: every member must be a 32-bit scalar.
        for (u32 member : type(inst.type).members) {
            if (!scalar32(member) && !handle_op) {
                return unimplemented("dx.op " + std::to_string(opcode) + " returning " +
                                     dxil_type_name(m_, inst.type));
            }
        }
    }
    switch (opcode) {
    // ---- arithmetic ----
    case 6:                                                       // FAbs
        if (preserve_denormals_ && is_f32(inst.type)) {
            // The sign bit cleared: a float abs on the GPU may flush.
            const Operand d = dst(inst.result), a = src(arg(1)), m = imm(0x7FFFFFFFu);
            body_.inst(op(Sm4Opcode::AND), {&d, &a, &m});
            return true;
        }
        return unary(Sm4Opcode::MOV, 2);
    case 7:                                                       // Saturate
        if (preserve_denormals_ && is_f32(inst.type)) {
            const Operand d = dst(inst.result), a = src(arg(1));
            f32p(dxbc_vitreo_ext::F32POp::Saturate, d, {&a});
            return true;
        }
        return unary(Sm4Opcode::MOV, 0, kSaturate);
    case 8: {                                                     // IsNaN
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1));
        body_.inst(op(Sm4Opcode::NE), {&d, &a, &a});
        return true;
    }
    case 9:                                                       // IsInf
    case 10: {                                                    // IsFinite
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1), 2);
        const Operand inf = imm(0x7F800000u);
        body_.inst(op(opcode == 9 ? Sm4Opcode::EQ : Sm4Opcode::LT), {&d, &a, &inf});
        return true;
    }
    case 11: {                                                    // IsNormal
        const Operand d = dst(inst.result);
        const Slot e = fresh_scalar();
        const Operand ed = dst_temp(e.reg, 1u << e.comp);
        const Operand a = src(arg(1), 2);
        const Operand lo = imm(0x00800000u), inf = imm(0x7F800000u);
        body_.inst(op(Sm4Opcode::GE), {&d, &a, &lo});
        body_.inst(op(Sm4Opcode::LT), {&ed, &a, &inf});
        const Operand s1 = src(inst.result), s2 = src_temp_scalar(e.reg, e.comp);
        body_.inst(op(Sm4Opcode::AND), {&d, &s1, &s2});
        return true;
    }
    case 12:                                                      // Cos
    case 13: {                                                    // Sin
        const Operand d = dst(inst.result);
        const Operand a = src(arg(1));
        if (opcode == 13) body_.inst(op(Sm4Opcode::SINCOS), {&d, &nul, &a});
        else body_.inst(op(Sm4Opcode::SINCOS), {&nul, &d, &a});
        return true;
    }
    case 21: return unary(Sm4Opcode::EXP);
    case 22: return unary(Sm4Opcode::FRC);
    case 23: return unary(Sm4Opcode::LOG);
    case 24: return unary(Sm4Opcode::SQRT);
    case 25: return unary(Sm4Opcode::RSQ);
    case 26: return unary(Sm4Opcode::ROUND_NE);
    case 27: return unary(Sm4Opcode::ROUND_NI);
    case 28: return unary(Sm4Opcode::ROUND_PI);
    case 29: return unary(Sm4Opcode::ROUND_Z);
    case 30: return unary(Sm4Opcode::BFREV);
    case 31: return unary(Sm4Opcode::COUNTBITS);
    case 32: return unary(Sm4Opcode::FIRSTBIT_LO);
    case 33: return unary(Sm4Opcode::FIRSTBIT_HI);
    case 34: return unary(Sm4Opcode::FIRSTBIT_SHI);
    case 35:
    case 36: {
        const Sm4Opcode o = opcode == 35 ? Sm4Opcode::MAX : Sm4Opcode::MIN;
        if (preserve_denormals_ && is_f32(inst.type)) {
            const Operand d = dst(inst.result), a = src(arg(1)), b = src(arg(2));
            f32p(opcode == 35 ? dxbc_vitreo_ext::F32POp::Max : dxbc_vitreo_ext::F32POp::Min, d, {&a, &b});
            return true;
        }
        if (fn_->fp32_denorm_mode != "ftz" || type(inst.type).kind != DxilTypeKind::Float) return binary(o);
        // "fp32-denorm-mode"="ftz": a denormal input reads as its signed
        // zero (DXIL.rst, the FMax / FMin tables); Metal's fmax / fmin
        // would pass it through (measured, d3d12_dxilc cs_denorm_ftz word
        // 6: max(-denormal, 0) is +0 on WARP12 and the RTX 4080).
        const auto flushed = [&](u32 id) {
            const Slot t = fresh_scalar(), z = fresh_scalar(), r = fresh_scalar();
            const Operand td = dst_temp(t.reg, 1u << t.comp), ts = src_temp_scalar(t.reg, t.comp);
            const Operand zd = dst_temp(z.reg, 1u << z.comp), zs = src_temp_scalar(z.reg, z.comp);
            const Operand rd = dst_temp(r.reg, 1u << r.comp);
            const Operand v = src(id), exponent = imm(0x7F800000u), sign = imm(0x80000000u);
            body_.inst(op(Sm4Opcode::AND), {&td, &v, &exponent});
            body_.inst(op(Sm4Opcode::AND), {&zd, &v, &sign});
            body_.inst(op(Sm4Opcode::MOVC), {&rd, &ts, &v, &zs});
            return src_temp_scalar(r.reg, r.comp);
        };
        const Operand a = flushed(arg(1)), b = flushed(arg(2)), d = dst(inst.result);
        body_.inst(op(o), {&d, &a, &b});
        return true;
    }
    case 37: return binary(Sm4Opcode::IMAX);
    case 38: return binary(Sm4Opcode::IMIN);
    case 39: return binary(Sm4Opcode::UMAX);
    case 40: return binary(Sm4Opcode::UMIN);
    case 46:                                                      // FMad
        if (preserve_denormals_ && is_f32(inst.type)) {
            // Fused (DXIL.rst FMad: "not fused for precise operations" is a
            // permission to fuse the others; the RTX 4080 fuses).
            const Operand d = dst(inst.result), a = src(arg(1)), b = src(arg(2)), c = src(arg(3));
            f32p(dxbc_vitreo_ext::F32POp::Fma, d, {&a, &b, &c});
            return true;
        }
        return tertiary(Sm4Opcode::MAD);
    case 48: return tertiary(Sm4Opcode::IMAD);
    case 49: return tertiary(Sm4Opcode::UMAD);
    case 51: return tertiary(Sm4Opcode::IBFE);
    case 52: return tertiary(Sm4Opcode::UBFE);
    case 53: {                                                    // Bfi(width, offset, value, replaced)
        const Operand d = dst(inst.result);
        const Operand w = src(arg(1)), o = src(arg(2)), v = src(arg(3)), r = src(arg(4));
        body_.inst(op(Sm4Opcode::BFI), {&d, &w, &o, &v, &r});
        return true;
    }
    case 54:
    case 55:
    case 56: {                                                    // Dot2/3/4(a..., b...)
        const u32 n = opcode - 52;
        if (preserve_denormals_) {
            // a0 * b0, then fma(ak, bk, sum) for each further pair.
            using dxbc_vitreo_ext::F32POp;
            const Slot acc = fresh_scalar();
            const Operand ad = dst_temp(acc.reg, 1u << acc.comp), as = src_temp_scalar(acc.reg, acc.comp);
            const Operand a0 = src(arg(1)), b0 = src(arg(1 + n));
            f32p(F32POp::Mul, n == 1 ? dst(inst.result) : ad, {&a0, &b0});
            for (u32 k = 1; k < n; ++k) {
                const Operand ak = src(arg(1 + k)), bk = src(arg(1 + n + k));
                const Operand to = k + 1 == n ? dst(inst.result) : ad;
                f32p(F32POp::Fma, to, {&ak, &bk, &as});
            }
            return true;
        }
        u32 ra = 0, rb = 0;
        std::vector<u32> av, bv;
        for (u32 k = 0; k < n; ++k) {
            av.push_back(arg(1 + k));
            bv.push_back(arg(1 + n + k));
        }
        coords_vector(av, ra);
        coords_vector(bv, rb);
        const Operand d = dst(inst.result);
        const Operand a = src_temp(ra, 0xE4), b = src_temp(rb, 0xE4);
        body_.inst(op(n == 2 ? Sm4Opcode::DP2 : n == 3 ? Sm4Opcode::DP3 : Sm4Opcode::DP4), {&d, &a, &b});
        return true;
    }
    case 126:                                                     // BitcastI32toF32
    case 127: return unary(Sm4Opcode::MOV);                       // BitcastF32toI32
    case 130: return unary(Sm4Opcode::F32TOF16);                  // LegacyF32ToF16
    case 131: return unary(Sm4Opcode::F16TOF32);                  // LegacyF16ToF32
    // ---- compute system values ----
    // ---- signatures and pixel operations ----
    case 4: return emit_load_input(inst);
    case 5: return emit_store_output(inst);
    case 82: {                                                    // Discard(condition)
        if (out_.stage != ShaderStage::Fragment) return malformed("discard outside the pixel stage");
        if (auto bits = constant_bits(arg(1))) {
            if (*bits == 0) return true;
            const Operand t = imm(0xFFFFFFFFu);
            const Slot s = fresh_scalar();
            const Operand d = dst_temp(s.reg, 1u << s.comp);
            body_.inst(op(Sm4Opcode::MOV), {&d, &t});
            const Operand c = src_temp_select(s.reg, s.comp);
            body_.inst(op(Sm4Opcode::DISCARD) | kTestNonZero, {&c});
            return true;
        }
        const Slot s = slot(arg(1));
        const Operand c = src_temp_select(s.reg, s.comp);
        body_.inst(op(Sm4Opcode::DISCARD) | kTestNonZero, {&c});
        return true;
    }
    case 83:                                                      // DerivCoarseX
    case 84:                                                      // DerivCoarseY
    case 85:                                                      // DerivFineX
    case 86: {                                                    // DerivFineY
        if (out_.stage == ShaderStage::Compute) {
            if (quad_layout_ == QuadLayout::None) return malformed("derivatives without a quad layout");
            const Operand d = dst(inst.result);
            const Operand v = src(arg(1));
            quad_derivative(opcode - 83, d, v);
            return true;
        }
        if (out_.stage != ShaderStage::Fragment) {
            return unimplemented("derivatives outside the pixel and compute stages");
        }
        const Sm4Opcode o = opcode == 83   ? Sm4Opcode::DERIV_RTX_COARSE
                            : opcode == 84 ? Sm4Opcode::DERIV_RTY_COARSE
                            : opcode == 85 ? Sm4Opcode::DERIV_RTX_FINE
                                           : Sm4Opcode::DERIV_RTY_FINE;
        return unary(o);
    }
    case 93: return system(Sm4OperandType::InputThreadId, false);
    case 94: return system(Sm4OperandType::InputThreadGroupId, false);
    case 95: return system(Sm4OperandType::InputThreadIdInGroup, false);
    case 96: return system(Sm4OperandType::InputThreadIdInGroupFlattened, true);
    // ---- barriers ----
    case 80: {
        u32 mode = 0;
        if (!const_u32(arg(1), mode)) return malformed("non-constant barrier mode");
        u32 flags = 0;
        if (mode & kBarrierSyncThreadGroup) flags |= kSyncThreadsInGroup;
        if (mode & kBarrierUavFenceGlobal) flags |= kSyncUavGlobal;
        if (mode & kBarrierUavFenceThreadGroup) flags |= kSyncUavGroup;
        if (mode & kBarrierTgsmFence) flags |= kSyncTgsm;
        body_.inst(op(Sm4Opcode::SYNC) | flags, {});
        return true;
    }
    // ---- resources ----
    case 57: {                                                    // CreateHandle
        u32 cls = 0, range = 0;
        if (!const_u32(arg(1), cls) || !const_u32(arg(2), range)) return malformed("non-constant createHandle class");
        Handle h;
        for (const Resource& r : resources_) {
            if (static_cast<u32>(r.cls) == cls && r.id == range) h.res = &r;
        }
        if (h.res == nullptr) return malformed("createHandle names no resource record");
        h.reg = index_of(arg(3));
        u64 nonuniform = 0;
        m_.constant_int(fn_, arg(4), nonuniform);
        h.nonuniform = (nonuniform & 1u) != 0;
        handles_[inst.result] = h;
        return true;
    }
    default:
        return emit_resource_op(opcode, inst);
    }
}

bool Lowerer::emit_resource_op(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    if (bool handled = false; !emit_op_ext(opcode, inst, handled) || handled) return !failed_;
    Handle h;
    switch (opcode) {
    case 59: {                                                    // CBufferLoadLegacy(handle, row)
        if (!handle_of(arg(1), h)) return false;
        const u32 r = vec(inst.result);
        const Operand d = dst_temp(r, 0xF);
        const Operand cb = cbuffer(h.res->id, h.reg, index_of(arg(2)), h.nonuniform, 0xE4);
        body_.inst(op(Sm4Opcode::MOV), {&d, &cb});
        return true;
    }
    case 68:                                                      // BufferLoad(res, c0, c1)
    case 139: {                                                   // RawBufferLoad(res, index, offset, mask, align)
        if (!handle_of(arg(1), h)) return false;
        const usize at = body_.size();
        const u32 r = vec(inst.result);
        const Operand d = dst_temp(r, 0xF);
        const Operand res = resource_src(h);
        u32 mask = 0xF;
        if (opcode == 139) {
            u32 m = 0;
            if (!const_u32(arg(4), m)) return malformed("non-constant raw buffer mask");
            mask = m & 0xFu;
        }
        const Operand dm = dst_temp(r, mask);
        switch (h.res->shape) {
        case ResourceShape::TypedBuffer: {
            const Operand c = src(arg(2));
            body_.inst(op(h.res->cls == ResourceClass::UAV ? Sm4Opcode::LD_UAV_TYPED : Sm4Opcode::LD),
                       {&d, &c, &res});
            return add_status(at, inst.result);
        }
        case ResourceShape::RawBuffer: {
            const Operand c = src(arg(2));
            body_.inst(op(Sm4Opcode::LD_RAW), {&dm, &c, &res});
            return add_status(at, inst.result);
        }
        case ResourceShape::StructuredBuffer: {
            const Operand i = src(arg(2)), o = src(arg(3));
            body_.inst(op(Sm4Opcode::LD_STRUCTURED), {&dm, &i, &o, &res});
            return add_status(at, inst.result);
        }
        default:
            return malformed("buffer load from a texture");
        }
    }
    case 69:                                                      // BufferStore(res, c0, c1, v0..v3, mask)
    case 140: {                                                   // RawBufferStore(res, index, offset, v0..v3, mask, align)
        if (!handle_of(arg(1), h)) return false;
        u32 mask = 0;
        if (!const_u32(arg(8), mask)) return malformed("non-constant store mask");
        mask &= 0xFu;
        const u32 r = fresh_vector();
        store_values(r, inst, 5, mask);  // operands[5..8]: v0..v3
        const Operand v = src_temp(r, 0xE4);
        const Operand u = resource(Sm4OperandType::UnorderedAccessView, h.res->id, h.reg, h.nonuniform, ~0u,
                                   h.res->shape == ResourceShape::TypedBuffer ? 0xF : mask);
        switch (h.res->shape) {
        case ResourceShape::TypedBuffer: {
            const Operand c = src(arg(2));
            body_.inst(op(Sm4Opcode::STORE_UAV_TYPED), {&u, &c, &v});
            return true;
        }
        case ResourceShape::RawBuffer: {
            const Operand c = src(arg(2));
            body_.inst(op(Sm4Opcode::STORE_RAW), {&u, &c, &v});
            return true;
        }
        case ResourceShape::StructuredBuffer: {
            const Operand i = src(arg(2)), o = src(arg(3));
            body_.inst(op(Sm4Opcode::STORE_STRUCTURED), {&u, &i, &o, &v});
            return true;
        }
        default:
            return malformed("buffer store to a texture");
        }
    }
    case 67: {                                                    // TextureStore(res, c0, c1, c2, v0..v3, mask)
        if (!handle_of(arg(1), h)) return false;
        u32 coords = 0;
        coords_vector({arg(2), arg(3), arg(4)}, coords);
        u32 mask = 0;
        if (!const_u32(arg(9), mask)) return malformed("non-constant store mask");
        const u32 r = fresh_vector();
        store_values(r, inst, 6, mask & 0xFu);  // operands[6..9]: v0..v3
        const Operand u = resource(Sm4OperandType::UnorderedAccessView, h.res->id, h.reg, h.nonuniform, ~0u, 0xF);
        const Operand c = src_temp(coords, 0xE4), v = src_temp(r, 0xE4);
        body_.inst(op(Sm4Opcode::STORE_UAV_TYPED), {&u, &c, &v});
        return true;
    }
    case 78: {                                                    // AtomicBinOp(res, op, o0, o1, o2, value)
        if (!handle_of(arg(1), h)) return false;
        u32 aop = 0;
        if (!const_u32(arg(2), aop)) return malformed("non-constant atomic operation");
        Sm4Opcode o;
        switch (static_cast<AtomicOp>(aop)) {
        case AtomicOp::Add: o = Sm4Opcode::IMM_ATOMIC_IADD; break;
        case AtomicOp::And: o = Sm4Opcode::IMM_ATOMIC_AND; break;
        case AtomicOp::Or: o = Sm4Opcode::IMM_ATOMIC_OR; break;
        case AtomicOp::Xor: o = Sm4Opcode::IMM_ATOMIC_XOR; break;
        case AtomicOp::IMin: o = Sm4Opcode::IMM_ATOMIC_IMIN; break;
        case AtomicOp::IMax: o = Sm4Opcode::IMM_ATOMIC_IMAX; break;
        case AtomicOp::UMin: o = Sm4Opcode::IMM_ATOMIC_UMIN; break;
        case AtomicOp::UMax: o = Sm4Opcode::IMM_ATOMIC_UMAX; break;
        case AtomicOp::Exchange: o = Sm4Opcode::IMM_ATOMIC_EXCH; break;
        default: return unimplemented("atomic operation " + std::to_string(aop));
        }
        u32 addr = 0;
        coords_vector({arg(3), arg(4), arg(5)}, addr);
        const Operand a = src_temp(addr, 0xE4);
        const Operand u = resource(Sm4OperandType::UnorderedAccessView, h.res->id, h.reg, h.nonuniform, ~0u, 0xF);
        const Operand v = src(arg(6));
        const Slot s = inst.result != ~0u ? slot(inst.result) : fresh_scalar();
        const Operand d = dst_temp(s.reg, 1u << s.comp);
        body_.inst(op(o), {&d, &u, &a, &v});
        return true;
    }
    case 79: {                                                    // AtomicCompareExchange(res, o0, o1, o2, cmp, new)
        if (!handle_of(arg(1), h)) return false;
        u32 addr = 0;
        coords_vector({arg(2), arg(3), arg(4)}, addr);
        const Operand a = src_temp(addr, 0xE4);
        const Operand u = resource(Sm4OperandType::UnorderedAccessView, h.res->id, h.reg, h.nonuniform, ~0u, 0xF);
        const Operand cmpv = src(arg(5)), newv = src(arg(6));
        const Slot s = inst.result != ~0u ? slot(inst.result) : fresh_scalar();
        const Operand d = dst_temp(s.reg, 1u << s.comp);
        body_.inst(op(Sm4Opcode::IMM_ATOMIC_CMP_EXCH), {&d, &u, &a, &cmpv, &newv});
        return true;
    }
    case 70: {                                                    // BufferUpdateCounter(res, inc)
        if (!handle_of(arg(1), h)) return false;
        u32 inc = 0;
        if (!const_u32(arg(2), inc)) return malformed("non-constant counter direction");
        const Operand u = resource(Sm4OperandType::UnorderedAccessView, h.res->id, h.reg, h.nonuniform, ~0u, 0xF);
        const Slot s = inst.result != ~0u ? slot(inst.result) : fresh_scalar();
        const Operand d = dst_temp(s.reg, 1u << s.comp);
        body_.inst(op(static_cast<i8>(inc) > 0 ? Sm4Opcode::IMM_ATOMIC_ALLOC : Sm4Opcode::IMM_ATOMIC_CONSUME),
                   {&d, &u});
        return true;
    }
    default:
        return unimplemented("dx.op " + std::to_string(opcode));
    }
}

// =============================================================================
// Signatures
// =============================================================================

namespace {

// D3D_NAME (d3dcommon.h) values a signature entry's system value names.
constexpr u32 kNamePosition = 1, kNameClip = 2, kNameCull = 3, kNameRtai = 4, kNameVpai = 5,
              kNameVertexId = 6, kNamePrimitiveId = 7, kNameInstanceId = 8, kNameFrontFace = 9,
              kNameSampleIndex = 10, kNameTarget = 64, kNameDepth = 65, kNameCoverage = 66,
              kNameDepthGe = 67, kNameDepthLe = 68, kNameStencilRef = 69;
constexpr u32 kDclInputSgv = 96, kDclInputSiv = 97, kDclInputPs = 98, kDclInputPsSgv = 99,
              kDclInputPsSiv = 100, kDclOutput = 101, kDclOutputSiv = 103;
// D3D10_SB_INTERPOLATION_MODE constant.
constexpr u32 kInterpConstant = 1;

Operand reg_operand(Sm4OperandType type, u32 reg, u32 mode_bits, bool destination) {
    Operand o;
    // 4 components; mask for a destination / declaration, swizzle otherwise.
    const u32 mode = destination ? 0u : 1u;
    o.tokens.push_back(2u | (mode << 2) | (mode_bits << 4) | (static_cast<u32>(type) << 12) | (1u << 20));
    o.tokens.push_back(reg);
    return o;
}

Operand scalar_register(Sm4OperandType type) {
    Operand o;
    o.tokens.push_back(1u | (static_cast<u32>(type) << 12));
    return o;
}

/// The output-register operand type a pixel-shader system value writes
/// through, or Output for an ordinary register.
Sm4OperandType pixel_output_type(u32 name) {
    switch (name) {
    case kNameDepth: return Sm4OperandType::OutputDepth;
    case kNameDepthGe: return Sm4OperandType::OutputDepthGreaterEqual;
    case kNameDepthLe: return Sm4OperandType::OutputDepthLessEqual;
    case kNameCoverage: return Sm4OperandType::OutputCoverageMask;
    case kNameStencilRef: return Sm4OperandType::OutputStencilRef;
    default: return Sm4OperandType::Output;
    }
}

}  // namespace

bool Lowerer::declare_signatures() {
    if (out_.stage == ShaderStage::Geometry || out_.stage == ShaderStage::Hull ||
        out_.stage == ShaderStage::Domain) {
        return declare_stage_signatures();
    }
    const bool pixel = out_.stage == ShaderStage::Fragment;
    for (const SigEntry& e : entries_in_) {
        if (e.reg == ~0u) continue;  // read through an intrinsic, not a register
        const Operand v = reg_operand(Sm4OperandType::Input, e.reg, e.mask, true);
        if (!pixel) {
            if (e.system_value == kNameVertexId || e.system_value == kNameInstanceId ||
                e.system_value == kNamePrimitiveId) {
                decls_.inst(kDclInputSgv, {&v}, {e.system_value});
            } else if (e.system_value != 0) {
                decls_.inst(kDclInputSiv, {&v}, {e.system_value});
            } else {
                decls_.inst(kDclInput, {&v});
            }
            continue;
        }
        u32 interp = 0;
        for (const SigElement& s : sig_in_) {
            if (s.start_row >= 0 && e.reg >= s.start_row && e.reg < s.start_row + s.rows &&
                (e.mask >> s.start_col) & 1u) {
                interp = s.interpolation;
            }
        }
        if (e.system_value == kNamePosition) {
            decls_.inst(kDclInputPsSiv | (interp << 11), {&v}, {e.system_value});
        } else if (e.system_value == kNameFrontFace || e.system_value == kNamePrimitiveId ||
                   e.system_value == kNameSampleIndex || e.system_value == kNameRtai ||
                   e.system_value == kNameVpai) {
            decls_.inst(kDclInputPsSgv | (kInterpConstant << 11), {&v}, {e.system_value});
        } else if (e.system_value == kNameClip || e.system_value == kNameCull) {
            decls_.inst(kDclInputPsSiv | (interp << 11), {&v}, {e.system_value});
        } else if (e.system_value != 0) {
            return unimplemented("pixel input system value " + std::to_string(e.system_value));
        } else {
            decls_.inst(kDclInputPs | (interp << 11), {&v});
        }
    }
    for (const SigEntry& e : entries_out_) {
        if (pixel) {
            const Sm4OperandType t = pixel_output_type(e.system_value);
            if (t != Sm4OperandType::Output) {
                const Operand o = scalar_register(t);
                decls_.inst(kDclOutput, {&o});
                continue;
            }
            if (e.system_value != 0 && e.system_value != kNameTarget) {
                return unimplemented("pixel output system value " + std::to_string(e.system_value));
            }
            const Operand o = reg_operand(Sm4OperandType::Output, e.reg, e.mask, true);
            decls_.inst(kDclOutput, {&o});
            continue;
        }
        const Operand o = reg_operand(Sm4OperandType::Output, e.reg, e.mask, true);
        if (e.system_value == kNamePosition || e.system_value == kNameClip || e.system_value == kNameCull ||
            e.system_value == kNameRtai || e.system_value == kNameVpai) {
            decls_.inst(kDclOutputSiv, {&o}, {e.system_value});
        } else if (e.system_value != 0) {
            return unimplemented("output system value " + std::to_string(e.system_value));
        } else {
            decls_.inst(kDclOutput, {&o});
        }
    }
    return true;
}

bool Lowerer::emit_load_input(const DxilInstruction& inst) {
    // LoadInput(inputSigId, rowIndex, colIndex, gsVertexAxis)
    u32 id = 0, row = 0, col = 0;
    if (!const_u32(inst.operands[2], id) || !const_u32(inst.operands[4], col)) {
        return malformed("non-constant loadInput element");
    }
    if (!const_u32(inst.operands[3], row) || !undef(inst.operands[5]) || out_.stage == ShaderStage::Geometry ||
        out_.stage == ShaderStage::Hull || out_.stage == ShaderStage::Domain) {
        return emit_load_input_ext(inst);
    }
    const SigElement* e = nullptr;
    for (const SigElement& s : sig_in_) {
        if (s.id == id) e = &s;
    }
    if (e != nullptr && e->kind == 28 && out_.stage == ShaderStage::Fragment) {
        // SV_Barycentrics (SemanticKind 28 Barycentrics, as dxc 1.6 writes
        // it in dx.entryPoints; DXIL.rst: a float3,
        // NotPacked): VX_BARY Coord with the element's interpolation mode.
        if (col > 2) return malformed("SV_Barycentrics beyond .z");
        const u32 r = fresh_vector();
        const Operand weights = dst_temp(r, 0xF);
        emit_ext(body_, dxbc_vitreo_ext::kOpBary,
                 static_cast<u32>(dxbc_vitreo_ext::BaryOp::Coord) | ((e->interpolation & 0xFu) << 8), {&weights});
        const Operand d = dst(inst.result);
        const Operand v = src_temp_select(r, col);
        body_.inst(op(Sm4Opcode::MOV), {&d, &v});
        return true;
    }
    if (e == nullptr || e->start_row < 0) return unimplemented("an input element with no register");
    const u32 reg = static_cast<u32>(e->start_row) + row;
    const u32 comp = e->start_col + col;
    const Operand d = dst(inst.result);
    const Operand v = reg_operand(Sm4OperandType::Input, reg, swizzle_of(comp), false);
    body_.inst(op(Sm4Opcode::MOV), {&d, &v});
    return true;
}

bool Lowerer::emit_store_output(const DxilInstruction& inst) {
    // StoreOutput(outputSigId, rowIndex, colIndex, value)
    u32 id = 0, row = 0, col = 0;
    if (!const_u32(inst.operands[2], id) || !const_u32(inst.operands[4], col)) {
        return malformed("non-constant storeOutput element");
    }
    if (!const_u32(inst.operands[3], row)) return emit_store_output_ext(inst);
    const SigElement* e = nullptr;
    for (const SigElement& s : sig_out_) {
        if (s.id == id) e = &s;
    }
    if (e == nullptr) return malformed("storeOutput names no element");
    const Operand v = src(inst.operands[5]);
    if (out_.stage == ShaderStage::Fragment) {
        u32 name = 0;
        for (const SigEntry& s : entries_out_) {
            if (s.name == e->name) name = s.system_value;
        }
        const Sm4OperandType t = pixel_output_type(name);
        if (t != Sm4OperandType::Output) {
            const Operand o = scalar_register(t);
            body_.inst(op(Sm4Opcode::MOV), {&o, &v});
            return true;
        }
    }
    if (e->start_row < 0) return unimplemented("an output element with no register");
    const u32 reg = static_cast<u32>(e->start_row) + row;
    const u32 comp = e->start_col + col;
    const Operand o = reg_operand(Sm4OperandType::Output, reg, 1u << comp, true);
    body_.inst(op(Sm4Opcode::MOV), {&o, &v});
    return true;
}

std::vector<u8> Lowerer::write_signature(const std::vector<SigEntry>& entries, bool with_stream) {
    // ISGN / OSGN / PCSG (24-byte elements) or OSG5 (28: stream first).
    const u32 stride = with_stream ? 28u : 24u;
    std::vector<u8> out(8u + entries.size() * stride, 0);
    const auto put = [&](usize at, u32 v) { std::memcpy(out.data() + at, &v, 4); };
    put(0, static_cast<u32>(entries.size()));
    put(4, 8);
    for (usize i = 0; i < entries.size(); ++i) {
        const SigEntry& e = entries[i];
        usize at = 8u + i * stride;
        if (with_stream) {
            put(at, e.stream);
            at += 4;
        }
        put(at, static_cast<u32>(out.size()));
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.push_back(0);
        put(at + 4, e.index);
        put(at + 8, e.system_value);
        put(at + 12, e.component_type);
        put(at + 16, e.reg);
        out[at + 20] = e.mask;
        out[at + 21] = e.rw_mask;
    }
    while (out.size() % 4 != 0) out.push_back(0);
    return out;
}

// =============================================================================
// Container
// =============================================================================

std::vector<u8> Lowerer::write_container() {
    std::vector<u32> shex;
    const u32 kind = out_.stage == ShaderStage::Fragment ? 0u
                     : out_.stage == ShaderStage::Vertex ? 1u
                     : out_.stage == ShaderStage::Geometry ? 2u
                     : out_.stage == ShaderStage::Hull ? 3u
                     : out_.stage == ShaderStage::Domain ? 4u
                                                         : 5u;
    shex.push_back((kind << 16) | (5u << 4) | 1u);
    shex.push_back(0);
    for (const auto& [slot, words] : readonly_initializers_) {
        shex.push_back(53u | (dxbc_vitreo_ext::kReadonlyIndexableDataClass << 11));
        shex.push_back(static_cast<u32>(words.size()) + 3u);
        shex.push_back(slot);
        shex.insert(shex.end(), words.begin(), words.end());
    }
    shex.insert(shex.end(), decls_.code().begin(), decls_.code().end());
    shex.insert(shex.end(), prologue_.code().begin(), prologue_.code().end());
    shex.insert(shex.end(), body_.code().begin(), body_.code().end());
    shex[1] = static_cast<u32>(shex.size());

    struct Chunk {
        u32 fourcc;
        std::vector<u8> data;
    };
    std::vector<Chunk> chunks;
    // Signatures and the root signature are copied byte for byte.
    std::vector<u8> shex_bytes(shex.size() * 4);
    std::memcpy(shex_bytes.data(), shex.data(), shex_bytes.size());
    chunks.push_back({dxil_fourcc("SHEX"), std::move(shex_bytes)});
    const auto& parts = c_.parts;
    // The signatures in the layout the DXBC parser reads (ISGN / OSGN /
    // PCSG), from the DXIL container's ISG1 / OSG1 / PSG1; the root
    // signature byte for byte.
    if (c_.part("ISG1") != nullptr) {
        // SV_Barycentrics (D3D_NAME_BARYCENTRICS 23) has no register and no
        // SM 5.1 meaning: VX_BARY reads it, so the ISGN leaves it out.
        std::vector<SigEntry> in;
        for (const SigEntry& e : entries_in_) {
            if (!(e.system_value == 23 && e.reg == 0xFFFFFFFFu)) in.push_back(e);
        }
        chunks.push_back({dxil_fourcc("ISGN"), write_signature(in, false)});
    }
    if (c_.part("OSG1") != nullptr) {
        const bool gs = out_.stage == ShaderStage::Geometry;
        chunks.push_back({dxil_fourcc(gs ? "OSG5" : "OSGN"), write_signature(entries_out_, gs)});
    }
    if (c_.part("PSG1") != nullptr) chunks.push_back({dxil_fourcc("PCSG"), write_signature(entries_pc_, false)});
    chunks.push_back({dxil_fourcc("RDEF"), write_rdef()});
    if (out_.stage == ShaderStage::Mesh || out_.stage == ShaderStage::Amplification) {
        chunks.push_back({dxil_fourcc("VXMS"), write_mesh_chunk()});
    }
    for (const auto& p : parts) {
        if (p.fourcc == dxil_fourcc("RTS0")) {
            chunks.push_back({p.fourcc, std::vector<u8>(source_ + p.offset, source_ + p.offset + p.size)});
        }
    }
    const u32 header = 32 + static_cast<u32>(chunks.size()) * 4u;
    u32 total = header;
    for (const auto& ch : chunks) total += 8u + static_cast<u32>((ch.data.size() + 3u) & ~usize{3});
    std::vector<u8> out(total, 0);
    const auto put = [&](usize at, u32 v) { std::memcpy(out.data() + at, &v, 4); };
    std::memcpy(out.data(), "DXBC", 4);
    put(20, 1);
    put(24, total);
    put(28, static_cast<u32>(chunks.size()));
    u32 at = header;
    for (usize i = 0; i < chunks.size(); ++i) {
        put(32 + i * 4, at);
        put(at, chunks[i].fourcc);
        put(at + 4, static_cast<u32>((chunks[i].data.size() + 3u) & ~usize{3}));
        std::memcpy(out.data() + at + 8, chunks[i].data.data(), chunks[i].data.size());
        at += 8u + static_cast<u32>((chunks[i].data.size() + 3u) & ~usize{3});
    }
    u8 digest[16];
    dxbc_container_checksum(out.data(), total, digest);
    std::memcpy(out.data() + 4, digest, 16);
    return out;
}

// =============================================================================
// The extension lowering's view of the lowerer (dxil_lower_ext.hpp)
// =============================================================================

class ExtHost final : public LowerHost {
public:
    explicit ExtHost(Lowerer& l) : l_(l) {}
    const DxilModule& module() const override { return l_.m_; }
    const DxilFunction* function() const override { return l_.fn_; }
    ShaderStage stage() const override { return l_.out_.stage; }
    Emitter& body() override { return l_.body_; }
    ExtState& ext() override { return l_.ext_; }
    RegSlot slot(u32 id) override {
        const Slot s = l_.slot(id);
        return {s.reg, s.comp};
    }
    void bind(u32 id, RegSlot s) override { l_.slots_[id] = Slot{s.reg, s.comp}; }
    RegSlot fresh_scalar() override {
        const Slot s = l_.fresh_scalar();
        return {s.reg, s.comp};
    }
    RegSlot fresh_pair() override {
        const Slot s = l_.fresh_pair();
        return {s.reg, s.comp};
    }
    u32 fresh_vector() override { return l_.fresh_vector(); }
    u32 vector(u32 id) override { return l_.vec(id); }
    bool vector_of(u32 id, u32& reg) override {
        const auto it = l_.vec_regs_.find(id);
        if (it == l_.vec_regs_.end()) return false;
        reg = it->second;
        return true;
    }
    Operand src(u32 id, u32 modifier) override { return l_.src(id, modifier); }
    Operand dst(u32 id) override { return l_.dst(id); }
    bool const_u32(u32 id, u32& out) override { return l_.const_u32(id, out); }
    bool const_u64(u32 id, u64& out) override { return l_.m_.constant_int(l_.fn_, id, out); }
    bool undef(u32 id) override { return l_.undef(id); }
    bool resource_of(u32 handle, ResourceView& out) override {
        Handle h;
        if (!l_.handle_of(handle, h)) return false;
        out.cls = h.res->cls;
        out.shape = h.res->shape;
        out.range_id = h.res->id;
        out.reg = h.reg;
        out.nonuniform = h.nonuniform;
        out.element_type = h.res->element_type;
        out.stride = h.res->stride;
        return true;
    }
    bool load_element(u32 pointer, u32 offset, const Operand& d) override {
        Pointer p;
        if (!l_.resolve_pointer(pointer, p)) return false;
        if (p.payload) {
            // The words of an i64 / double, 4 bytes apart in the payload.
            p.constant += offset * 4u;
            return l_.emit_payload_load(p, d, kDxbcPayloadWord);
        }
        p.constant += offset;
        if (p.tgsm) {
            const Operand i = l_.pointer_index_src(p);
            const Operand zero = imm(0);
            const Operand g = tgsm(p.object, 0x00, false);
            l_.body_.inst(op(Sm4Opcode::LD_STRUCTURED), {&d, &i, &zero, &g});
        } else {
            const Operand x = indexable(p.object, l_.pointer_index(p), 0x00, false);
            l_.body_.inst(op(Sm4Opcode::MOV), {&d, &x});
        }
        return true;
    }
    bool store_element(u32 pointer, u32 offset, const Operand& v) override {
        Pointer p;
        if (!l_.resolve_pointer(pointer, p)) return false;
        p.constant += offset;
        if (p.tgsm) {
            const Operand i = l_.pointer_index_src(p);
            const Operand zero = imm(0);
            const Operand g = tgsm(p.object, 0x1, true);
            l_.body_.inst(op(Sm4Opcode::STORE_STRUCTURED), {&g, &i, &zero, &v});
        } else {
            const Operand x = indexable(p.object, l_.pointer_index(p), 0x1, true);
            l_.body_.inst(op(Sm4Opcode::MOV), {&x, &v});
        }
        return true;
    }
    bool emit_call_core(const DxilInstruction& inst) override {
        l_.bypass_ext_ = true;
        const bool ok = l_.emit_call(inst);
        l_.bypass_ext_ = false;
        return ok;
    }
    bool unimplemented(const std::string& what) override { return l_.unimplemented(what); }
    bool malformed(const std::string& what) override { return l_.malformed(what); }
    bool failed() const override { return l_.failed_; }

private:
    Lowerer& l_;
};

std::unique_ptr<LowerHost> make_ext_host(Lowerer& l) { return std::make_unique<ExtHost>(l); }

DxilToDxbcResult Lowerer::run() {
    host_ = make_ext_host(*this);
    if (!read_metadata()) return out_;
    if (out_.stage == ShaderStage::Hull) return run_hull();
    if (!build_cfg()) return out_;
    brk_reg_ = 0;  // r0.x: the multi-level branch target
    next_reg_ = 0;
    next_comp_ = 1;
    {
        const Operand d = dst_temp(0, 1);
        const Operand z = imm(0);
        body_.inst(op(Sm4Opcode::MOV), {&d, &z});
    }
    do_tree(0);
    if (failed_) return out_;
    if (!body_.last_was_jump()) body_.inst(op(Sm4Opcode::RET), {});
    if (!check_mesh_program()) return out_;
    if (!declare()) return out_;
    out_.container = write_container();
    out_.ok = true;
    return out_;
}

#include "dxil_lower_stages.inc"
#include "dxil_lower_mesh.inc"
#include "dxil_lower_ops.inc"
#include "dxil_lower_quads.inc"
#include "dxil_lower_rt.inc"
#include "dxil_lower_rtpipe.inc"

}  // namespace

DxilRtProgram dxil_raytracing_to_dxbc(const DxilRtProgramDesc& desc) { return build_rt_program(desc, false); }

DxilRtProgram dxil_raytracing_validate(const DxilRtProgramDesc& desc) { return build_rt_program(desc, true); }

bool dxil_library_exports(const u8* data, usize size, std::map<std::string, u32>& kinds) {
    const DxilContainer container = parse_dxil_container(data, size);
    if (!container.ok || container.program_kind() != 6) return false;
    const DxilModule module = read_dxil_module(container.bitcode, container.bitcode_size);
    if (!module.ok) return false;
    std::map<std::string, std::pair<const DxilFunction*, u32>> entries;
    if (!rt_entry_points(module, entries)) return false;
    for (const auto& [name, e] : entries) kinds[name] = e.second;
    return true;
}

namespace {

DxilToDxbcResult dxil_to_dxbc_uncached(const u8* data, usize size);

/// A lowered program as a TranslationCache value.
std::vector<u8> encode_lowered(const DxilToDxbcResult& r) {
    PayloadWriter w;
    w.u32_value(static_cast<u32>(r.stage));
    w.u32_value(r.shader_model_major);
    w.u32_value(r.shader_model_minor);
    w.u32_value(r.wave_size_min);
    w.u32_value(r.wave_size_max);
    w.bytes(r.container.data(), r.container.size());
    return std::move(w.out);
}

bool decode_lowered(const std::vector<u8>& payload, DxilToDxbcResult& r) {
    static_assert(sizeof(DxilToDxbcResult) == 80,
                  "a new DxilToDxbcResult field must be part of encode_lowered");
    PayloadReader in(payload);
    u32 stage = 0;
    r = DxilToDxbcResult{};
    if (!in.u32_value(stage) || !in.u32_value(r.shader_model_major) ||
        !in.u32_value(r.shader_model_minor) || !in.u32_value(r.wave_size_min) ||
        !in.u32_value(r.wave_size_max) || !in.bytes(r.container) || !in.done() ||
        r.container.empty()) {
        return false;
    }
    r.stage = static_cast<ShaderStage>(stage);
    r.ok = true;
    return true;
}

}  // namespace

DxilToDxbcResult dxil_to_dxbc(const u8* data, usize size) {
    // The lowering reads nothing but the container (no host property, no
    // environment), so the container is the whole key.
    const compile_profile::Scope timed(compile_profile::DxilLower);
    TranslationCache& cache = TranslationCache::instance();
    if (!cache.enabled() || data == nullptr || size == 0) return dxil_to_dxbc_uncached(data, size);
    const std::array<u8, 32> key = TranslationKey(TranslationKind::DxilToDxbc).bytes(data, size).finish();
    std::vector<u8> payload;
    DxilToDxbcResult out;
    if (cache.lookup(key, payload) && decode_lowered(payload, out)) {
        compile_profile::note(compile_profile::DxilHit);
        return out;
    }
    compile_profile::note(compile_profile::DxilMiss);
    out = dxil_to_dxbc_uncached(data, size);
    if (out.ok && out.failure.empty() && !out.unimplemented) cache.store(key, encode_lowered(out));
    return out;
}

namespace {

DxilToDxbcResult dxil_to_dxbc_uncached(const u8* data, usize size) {
    DxilToDxbcResult out;
    const DxilContainer container = parse_dxil_container(data, size);
    if (!container.ok) {
        out.failure = container.error;
        return out;
    }
    const u32 kind = container.program_kind();
    if (kind == 6 || (kind >= 7 && kind <= 12)) {
        out.failure = kind == 6 ? "DXIL libraries (raytracing, work graphs)"
                                : "raytracing programs";
        out.unimplemented = true;
        return out;
    }
    const DxilModule module = read_dxil_module(container.bitcode, container.bitcode_size);
    if (!module.ok) {
        out.failure = "DXIL bitcode: " + module.error;
        return out;
    }
    Lowerer lowerer(module, container);
    lowerer.source_ = data;
    return lowerer.run();
}

}  // namespace

}  // namespace vitreo::graphics
