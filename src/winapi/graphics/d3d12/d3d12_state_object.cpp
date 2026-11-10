// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_state_object.cpp
/// @brief ID3D12Device5::CreateStateObject, ID3D12Device7::AddToStateObject
/// and ID3D12StateObjectProperties for DXR: RAYTRACING_PIPELINE and
/// COLLECTION state objects (DirectX-Specs Raytracing.md, "State objects",
/// "Subobject association behavior", CreateStateObject structures). A
/// pipeline compiles to one compute megakernel
/// (graphics/shaders/dxil_raytracing.hpp) that DispatchRays runs
/// (d3d12_replay_raytracing.cpp).
///
/// Subobjects come from the description and from its DXIL libraries (RDAT,
/// graphics::dxil_library_subobjects); each export's local and global root
/// signature and configs are resolved by the association rules and their
/// measured precedence (d3d12_a48 subobjects). Every local root argument
/// kind is supported (dxil_raytracing.hpp).
///
/// Refused loudly (UNIMPLEMENTED, E_INVALIDARG): EXECUTABLE state objects
/// with programs (work graphs / generic programs: D3D12_WORK_GRAPHS_TIER,
/// not part of any raytracing tier), a subset of an existing collection's
/// exports, and ALLOW_OPACITY_MICROMAPS (raytracing tier 1.2).
///
/// Also ID3D12LifetimeTracker (ID3D12Device5::CreateLifetimeTracker; measured,
/// tools/oracle_probe/d3d12_a12_sessions_probe).

#include "winapi/graphics/d3d12/d3d12_internal.hpp"

#include <algorithm>
#include <set>

namespace vitreo::winapi::d3d12 {

StateObject::~StateObject() = default;

namespace {

constexpr u32 kSubConfig = 0, kSubGlobalRoot = 1, kSubLocalRoot = 2, kSubNodeMask = 3, kSubLibrary = 5,
              kSubCollection = 6, kSubAssociation = 7, kSubDxilAssociation = 8, kSubShaderConfig = 9,
              kSubPipelineConfig = 10, kSubHitGroup = 11, kSubPipelineConfig1 = 12;
constexpr u32 D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE = 1;
/// Shader identifier bytes 0..3 ("VRSI"); the index is at byte 8.
// kRtIdentifierMagic (d3d12_internal.hpp) starts every shader identifier.
constexpr u64 kStackUnknown = 0xFFFFFFFFull;
/// D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES (d3d12.h).
constexpr u32 kShaderIdentifierBytes = 32;

}  // namespace

namespace {

/// @p layout as the raytracing lowering sees it (dxil_raytracing.hpp
/// DxilRtProgramDesc::Root). A local root signature's arguments follow the
/// 32-byte identifier in each shader record, packed in declaration order,
/// each aligned to its size: constants 4 bytes, root descriptors and table
/// handles 8 (Raytracing.md, "Shader table memory initialization"). Its
/// static samplers become s(kRtLocalSamplerBase + n), n their index in
/// @p samplers (shared by the subobjects of one root signature).
graphics::DxilRtProgramDesc::Root rt_root(const RootLayout& layout, bool local, std::vector<SamplerState>* samplers,
                                          std::map<std::pair<const RootLayout*, usize>, u32>* indices) {
    using Root = graphics::DxilRtProgramDesc::Root;
    Root out;
    u32 offset = 0;
    for (const BindingParameter& p : layout.parameters) {
        Root::Param q;
        q.space = p.space;
        q.reg = p.shader_register;
        switch (p.type) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            q.kind = Root::Constants;
            q.words = p.num_32bit_values;
            q.record_offset = offset;
            offset += 4 * p.num_32bit_values;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            q.kind = Root::Table;
            for (const BindingRange& r : p.ranges) {
                q.ranges.push_back({r.type, r.space, r.base_register, r.count, r.table_offset});
            }
            [[fallthrough]];
        default:
            if (p.type == D3D12_ROOT_PARAMETER_TYPE_CBV) q.kind = Root::CBV;
            else if (p.type == D3D12_ROOT_PARAMETER_TYPE_SRV) q.kind = Root::SRV;
            else if (p.type == D3D12_ROOT_PARAMETER_TYPE_UAV) q.kind = Root::UAV;
            offset = (offset + 7u) & ~7u;
            q.record_offset = offset;
            offset += 8;
            break;
        }
        out.params.push_back(std::move(q));
    }
    for (usize k = 0; k < layout.static_samplers.size(); ++k) {
        const BindingSampler& s = layout.static_samplers[k];
        Root::Sampler q{s.space, s.shader_register, 0};
        if (local && samplers != nullptr && indices != nullptr) {
            auto [it, fresh] = indices->emplace(std::make_pair(&layout, k), static_cast<u32>(samplers->size()));
            if (fresh) samplers->push_back(s.state);
            q.reserved = it->second;
        }
        out.samplers.push_back(q);
    }
    return out;
}

}  // namespace

void Frontend::create_state_object(Marshaler& m, Device& device, bool add) {
    const char* api = add ? "ID3D12Device7::AddToStateObject" : "ID3D12Device5::CreateStateObject";
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr parent_address = add ? m.arg_ptr(2) : 0;
    const GuestAddr iid = m.arg_ptr(add ? 3 : 2);
    const GuestAddr out = m.arg_ptr(add ? 4 : 3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0 || out == 0) {
        m.set_return(desc == 0 ? E_INVALIDARG : E_POINTER);
        return;
    }
    const auto read_wide = [&](GuestAddr at, std::string& s) {
        s.clear();
        if (at == 0) return false;
        for (u32 k = 0; k < 4096; ++k) {
            const auto unit = m_memory.read_u16(at + k * 2u);
            if (!unit.is_ok()) return false;
            if (unit.value() == 0) return true;
            s.push_back(unit.value() < 0x80 ? static_cast<char>(unit.value()) : '?');
        }
        return false;
    };
    const auto refuse = [&](u32 hr, const char* why, bool missing) {
        if (missing) unimplemented(api, why);
        else d3d12_diag("%s: %s", api, why);
        m.set_return(hr);
    };
    const auto& dl = layout::D3D12_STATE_OBJECT_DESC[m_is_64bit];
    const auto& sl = layout::D3D12_STATE_SUBOBJECT[m_is_64bit];
    const u32 type = read_u32(desc + dl.Type);
    const u32 count = read_u32(desc + dl.NumSubobjects);
    GuestAddr subs = 0;
    (void)read_pointer(desc + dl.pSubobjects, subs);
    constexpr u32 kExecutable = 4;
    if (type != D3D12_STATE_OBJECT_TYPE_COLLECTION && type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE &&
        type != kExecutable) {
        refuse(E_INVALIDARG, "an unknown D3D12_STATE_OBJECT_TYPE", false);
        return;
    }
    if (count != 0 && subs == 0) {
        refuse(E_INVALIDARG, "no subobjects", false);
        return;
    }
    auto so = std::make_unique<StateObject>();
    so->device = device.address;
    so->type = type;
    StateObject* parent = nullptr;
    if (add) {
        parent = find<StateObject>(parent_address, Kind::StateObject);
        // Raytracing.md (AddToStateObject): the parent allows additions, is
        // not a collection, and the addition is the parent's type (measured,
        // d3d12_a48 add_closed: a collection parent and a COLLECTION
        // addition are E_INVALIDARG).
        if (parent == nullptr || (parent->flags & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) == 0 ||
            parent->type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE || type != parent->type) {
            refuse(E_INVALIDARG, "the parent does not allow additions", false);
            return;
        }
        // The child is the parent plus the addition; the addition itself is
        // resolved on its own (it must be valid for CreateStateObject).
        so->flags = parent->flags;
        so->libraries = parent->libraries;
        so->hit_groups = parent->hit_groups;
        so->has_shader_config = parent->has_shader_config;
        so->max_payload = parent->max_payload;
        so->max_attributes = parent->max_attributes;
        so->has_pipeline_config = parent->has_pipeline_config;
        so->max_recursion = parent->max_recursion;
        so->pipeline_flags = parent->pipeline_flags;
        so->local_roots = parent->local_roots;
        so->global_roots = parent->global_roots;
        so->indices = parent->indices;
        so->kinds = parent->kinds;
        so->stack_size = parent->stack_size;
    }

    // ---- pass 1: the subobjects, the libraries' own ones included ----
    // Associable subobjects. scope -1: the state object's own; k: declared
    // in this description's k-th DXIL library (Raytracing.md, "Default
    // associations", Terminology).
    struct Sub {
        u32 kind{0};
        i32 scope{-1};
        std::string name;
        std::shared_ptr<const RootLayout> root;
        u32 a{0}, b{0};
    };
    struct Assoc {
        i32 scope{-1};
        i32 target{-1};
        std::string target_name;  // a library subobject named by name
        std::vector<std::string> exports;
    };
    std::vector<Sub> sobs;
    std::vector<Assoc> assocs;
    std::vector<i32> sob_of(count, -1);
    std::vector<std::pair<std::string, i32>> added_functions;  // (export, library scope)
    std::map<std::string, i32> added_groups;                    // hit group -> scope
    std::set<std::string> added_names;
    u32 own_libraries = 0;
    u32 soc_flags = 0;
    bool any_soc = false;
    const auto new_name = [&](const std::string& name) {
        // Exports are unique in a state object and against its parent
        // (Raytracing.md; measured, d3d12_a48 subobjects "two", add_17/18).
        if (so->kinds.contains(name) || !added_names.insert(name).second) {
            refuse(E_INVALIDARG, "an export defined twice", false);
            return false;
        }
        return true;
    };
    for (u32 k = 0; k < count; ++k) {
        const GuestAddr sub = subs + static_cast<GuestAddr>(k) * sl.size;
        const u32 sub_type = read_u32(sub + sl.Type);
        GuestAddr d = 0;
        (void)read_pointer(sub + sl.pDesc, d);
        if (d == 0) {
            refuse(E_INVALIDARG, "a subobject without a description", false);
            return;
        }
        if (type == kExecutable && sub_type != kSubConfig && sub_type != kSubGlobalRoot && sub_type != kSubNodeMask) {
            // Measured (a12 state_object): an EXECUTABLE of only configs,
            // a global root signature or a node mask is created (it holds
            // no program); programs in one are work graphs / generic
            // programs (DirectX-Specs WorkGraphs.md, "Generic programs"),
            // which belong to D3D12_WORK_GRAPHS_TIER, not to the raytracing
            // tiers, and which this device does not have.
            refuse(E_INVALIDARG, "EXECUTABLE state objects with programs (work graphs, generic programs)", true);
            return;
        }
        switch (sub_type) {
        case kSubConfig: {
            // D3D12_STATE_OBJECT_FLAGS: LOCAL_DEP_ON_EXTERNAL 0x1,
            // EXTERNAL_DEP_ON_LOCAL 0x2, ALLOW_STATE_OBJECT_ADDITIONS 0x4;
            // the work-graph flags above them belong to a feature this
            // device lacks (measured: a12 "config flags 0x80" E_INVALIDARG).
            const u32 flags = read_u32(d + layout::D3D12_STATE_OBJECT_CONFIG[m_is_64bit].Flags);
            if ((flags & ~0x7u) != 0) {
                refuse(E_INVALIDARG, "unknown D3D12_STATE_OBJECT_FLAGS", false);
                return;
            }
            soc_flags |= flags;
            any_soc = true;
            sob_of[k] = static_cast<i32>(sobs.size());
            sobs.push_back({kSubConfig, -1, "", nullptr, flags, 0});
            break;
        }
        case kSubGlobalRoot:
        case kSubLocalRoot: {
            GuestAddr rs_address = 0;
            (void)read_pointer(d + layout::D3D12_GLOBAL_ROOT_SIGNATURE[m_is_64bit].pGlobalRootSignature,
                               rs_address);
            RootSignature* rs = find<RootSignature>(rs_address, Kind::RootSignature);
            if (rs == nullptr) {
                refuse(E_INVALIDARG, "not a root signature", false);
                return;
            }
            const bool local = (rs->layout->flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) != 0;
            if (local != (sub_type == kSubLocalRoot)) {
                refuse(E_INVALIDARG, "a root signature of the wrong kind (LOCAL_ROOT_SIGNATURE flag)", false);
                return;
            }
            sob_of[k] = static_cast<i32>(sobs.size());
            sobs.push_back({sub_type, -1, "", rs->layout, 0, 0});
            break;
        }
        case kSubNodeMask:
            // One node: 0 or 1 (as every NodeMask here); associable like the
            // configs.
            if (read_u32(d + layout::D3D12_NODE_MASK[m_is_64bit].NodeMask) > 1) {
                refuse(E_INVALIDARG, "a NodeMask naming a node this adapter lacks", false);
                return;
            }
            sob_of[k] = static_cast<i32>(sobs.size());
            sobs.push_back({kSubNodeMask, -1, "", nullptr, 1, 0});
            break;
        case kSubLibrary: {
            const auto& l = layout::D3D12_DXIL_LIBRARY_DESC[m_is_64bit];
            GuestAddr code = 0, size = 0, exports = 0;
            (void)read_pointer(d + l.DXILLibrary_pShaderBytecode, code);
            (void)read_pointer(d + l.DXILLibrary_BytecodeLength, size);
            const u32 n = read_u32(d + l.NumExports);
            (void)read_pointer(d + l.pExports, exports);
            StateObject::Library lib;
            lib.bytes.resize(size);
            if (code == 0 || size == 0 || !m_memory.read_bytes(code, lib.bytes.data(), size).is_ok()) {
                refuse(E_INVALIDARG, "an empty DXIL library", false);
                return;
            }
            std::map<std::string, u32> kinds;
            std::vector<graphics::DxilSubobject> declared;
            if (!graphics::dxil_library_exports(lib.bytes.data(), lib.bytes.size(), kinds) ||
                !graphics::dxil_library_subobjects(lib.bytes.data(), lib.bytes.size(), declared)) {
                refuse(E_INVALIDARG, "not a DXIL library", false);
                return;
            }
            const i32 scope = static_cast<i32>(own_libraries++);
            // What the library contributes: every function and subobject,
            // or the export list's (renamed) ones. Measured (d3d12_a48
            // subobjects "exports"): a list that leaves the subobjects out
            // leaves them out.
            std::vector<std::pair<std::string, std::string>> taken;  // (name, source)
            const auto& el = layout::D3D12_EXPORT_DESC[m_is_64bit];
            for (u32 e = 0; e < n; ++e) {
                GuestAddr name = 0, rename = 0;
                (void)read_pointer(exports + e * el.size + el.Name, name);
                (void)read_pointer(exports + e * el.size + el.ExportToRename, rename);
                std::string export_name, source;
                if (!read_wide(name, export_name)) {
                    refuse(E_INVALIDARG, "an export without a name", false);
                    return;
                }
                if (rename == 0) source = export_name;
                else (void)read_wide(rename, source);
                const bool subobject = std::any_of(declared.begin(), declared.end(),
                                                   [&](const auto& s) { return s.name == source; });
                if (!kinds.contains(source) && !subobject) {
                    refuse(E_INVALIDARG, "an export the library does not define", false);
                    return;
                }
                taken.emplace_back(export_name, source);
            }
            if (n == 0) {
                for (const auto& [name, kind] : kinds) taken.emplace_back(name, name);
                for (const auto& s : declared) taken.emplace_back(s.name, s.name);
            }
            for (const auto& [name, source] : taken) {
                if (const auto f = kinds.find(source); f != kinds.end()) {
                    if (n != 0) lib.exports.emplace_back(name, source);
                    if (f->second < 7 || f->second > 12) continue;  // a library function
                    if (!new_name(name)) return;
                    so->kinds[name] = f->second;
                    added_functions.emplace_back(name, scope);
                    continue;
                }
                const auto s = std::find_if(declared.begin(), declared.end(),
                                            [&](const auto& x) { return x.name == source; });
                switch (s->kind) {
                case kSubConfig:
                    if ((s->flags & ~0x7u) != 0) {
                        refuse(E_INVALIDARG, "unknown D3D12_STATE_OBJECT_FLAGS in a library", false);
                        return;
                    }
                    soc_flags |= s->flags;
                    any_soc = true;
                    break;
                case kSubGlobalRoot:
                case kSubLocalRoot: {
                    u32 error = S_OK;
                    auto root = root_layout_from_part(s->root_signature.data(), s->root_signature.size(), error);
                    if (!root) {
                        refuse(E_INVALIDARG, "a library root signature that does not parse", error == E_NOTIMPL);
                        return;
                    }
                    const bool local = (root->flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) != 0;
                    if (local != (s->kind == kSubLocalRoot)) {
                        refuse(E_INVALIDARG, "a library root signature of the wrong kind", false);
                        return;
                    }
                    sobs.push_back({s->kind, scope, name, std::move(root), 0, 0});
                    break;
                }
                case kSubShaderConfig:
                    sobs.push_back({s->kind, scope, name, nullptr, s->max_payload, s->max_attributes});
                    break;
                case kSubPipelineConfig:
                case kSubPipelineConfig1:
                    sobs.push_back({kSubPipelineConfig, scope, name, nullptr, s->max_recursion,
                                    s->kind == kSubPipelineConfig1 ? s->flags : 0u});
                    break;
                case kSubDxilAssociation:
                    assocs.push_back({scope, -1, s->subobject, s->exports});
                    break;
                case kSubHitGroup: {
                    graphics::DxilRtProgramDesc::HitGroup g;
                    g.name = name;
                    g.procedural = s->hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
                    g.any_hit = s->any_hit;
                    g.closest_hit = s->closest_hit;
                    g.intersection = s->intersection;
                    if (g.procedural == g.intersection.empty()) {
                        refuse(E_INVALIDARG, "a library hit group whose type and intersection shader disagree",
                               false);
                        return;
                    }
                    if (!new_name(name)) return;
                    so->kinds[name] = 0;
                    added_groups[name] = scope;
                    so->hit_groups.push_back(std::move(g));
                    break;
                }
                default:
                    break;
                }
            }
            // An export list of subobjects only takes no code (an empty
            // export list is the lowering's "every function").
            if (n == 0 || !lib.exports.empty()) so->libraries.push_back(std::move(lib));
            break;
        }
        case kSubCollection: {
            const auto& l = layout::D3D12_EXISTING_COLLECTION_DESC[m_is_64bit];
            GuestAddr address = 0;
            (void)read_pointer(d + l.pExistingCollection, address);
            StateObject* c = find<StateObject>(address, Kind::StateObject);
            if (c == nullptr || c->type != D3D12_STATE_OBJECT_TYPE_COLLECTION) {
                refuse(E_INVALIDARG, "EXISTING_COLLECTION names no collection", false);
                return;
            }
            if (read_u32(d + l.NumExports) != 0) {
                refuse(E_INVALIDARG, "a subset of a collection's exports", true);
                return;
            }
            for (const auto& [name, kind] : c->kinds) {
                if (!new_name(name)) return;
            }
            for (const auto& lib : c->libraries) so->libraries.push_back(lib);
            for (const auto& g : c->hit_groups) so->hit_groups.push_back(g);
            for (const auto& [name, kind] : c->kinds) so->kinds[name] = kind;
            for (const auto& [name, index] : c->indices) so->indices[name] = index;
            // Its exports keep the associations it resolved (Raytracing.md:
            // state-object associations do not reach into a collection;
            // measured, d3d12_a48 subobjects "collection_assoc").
            for (const auto& [name, root] : c->local_roots) so->local_roots[name] = root;
            for (const auto& [name, root] : c->global_roots) so->global_roots[name] = root;
            if (c->has_shader_config) {
                if (so->has_shader_config &&
                    (so->max_payload != c->max_payload || so->max_attributes != c->max_attributes)) {
                    refuse(E_INVALIDARG, "collections with different shader configs", false);
                    return;
                }
                so->has_shader_config = true;
                so->max_payload = c->max_payload;
                so->max_attributes = c->max_attributes;
            }
            if (c->has_pipeline_config) {
                if (so->has_pipeline_config && so->max_recursion != c->max_recursion) {
                    refuse(E_INVALIDARG, "collections with different pipeline configs", false);
                    return;
                }
                so->has_pipeline_config = true;
                so->max_recursion = c->max_recursion;
                so->pipeline_flags = c->pipeline_flags;
            }
            break;
        }
        case kSubAssociation: {
            const auto& l = layout::D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION[m_is_64bit];
            GuestAddr target = 0, names = 0;
            (void)read_pointer(d + l.pSubobjectToAssociate, target);
            (void)read_pointer(d + l.pExports, names);
            const u32 n = read_u32(d + l.NumExports);
            if (target < subs || (target - subs) % sl.size != 0 || (target - subs) / sl.size >= count) {
                refuse(E_INVALIDARG, "an association naming a subobject outside the description", false);
                return;
            }
            Assoc a;
            a.target = static_cast<i32>((target - subs) / sl.size);  // a subobject index, mapped below
            for (u32 e = 0; e < n; ++e) {
                GuestAddr name = 0;
                std::string s;
                (void)read_pointer(names + e * (m_is_64bit ? 8u : 4u), name);
                if (!read_wide(name, s)) {
                    refuse(E_INVALIDARG, "an association export without a name", false);
                    return;
                }
                a.exports.push_back(s);
            }
            assocs.push_back(std::move(a));
            break;
        }
        case kSubDxilAssociation: {
            const auto& l = layout::D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION[m_is_64bit];
            GuestAddr target = 0, names = 0;
            (void)read_pointer(d + l.SubobjectToAssociate, target);
            (void)read_pointer(d + l.pExports, names);
            const u32 n = read_u32(d + l.NumExports);
            Assoc a;
            if (!read_wide(target, a.target_name)) {
                refuse(E_INVALIDARG, "a DXIL subobject association without a subobject name", false);
                return;
            }
            for (u32 e = 0; e < n; ++e) {
                GuestAddr name = 0;
                std::string s;
                (void)read_pointer(names + e * (m_is_64bit ? 8u : 4u), name);
                if (!read_wide(name, s)) {
                    refuse(E_INVALIDARG, "an association export without a name", false);
                    return;
                }
                a.exports.push_back(s);
            }
            a.target = -2;  // by name, resolved below
            assocs.push_back(std::move(a));
            break;
        }
        case kSubShaderConfig: {
            const auto& l = layout::D3D12_RAYTRACING_SHADER_CONFIG[m_is_64bit];
            const u32 payload = read_u32(d + l.MaxPayloadSizeInBytes);
            const u32 attributes = read_u32(d + l.MaxAttributeSizeInBytes);
            if (attributes > 32) {  // D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES
                refuse(E_INVALIDARG, "MaxAttributeSizeInBytes above 32", false);
                return;
            }
            sob_of[k] = static_cast<i32>(sobs.size());
            sobs.push_back({kSubShaderConfig, -1, "", nullptr, payload, attributes});
            break;
        }
        case kSubPipelineConfig:
        case kSubPipelineConfig1: {
            const auto& l = layout::D3D12_RAYTRACING_PIPELINE_CONFIG1[m_is_64bit];
            const u32 depth = read_u32(d + l.MaxTraceRecursionDepth);
            const u32 flags = sub_type == kSubPipelineConfig1 ? read_u32(d + l.Flags) : 0u;
            if (depth > 31) {  // D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH
                refuse(E_INVALIDARG, "MaxTraceRecursionDepth above 31", false);
                return;
            }
            if ((flags & 0x400u) != 0) {
                refuse(E_INVALIDARG, "ALLOW_OPACITY_MICROMAPS", true);
                return;
            }
            sob_of[k] = static_cast<i32>(sobs.size());
            sobs.push_back({kSubPipelineConfig, -1, "", nullptr, depth, flags});
            break;
        }
        case kSubHitGroup: {
            const auto& l = layout::D3D12_HIT_GROUP_DESC[m_is_64bit];
            graphics::DxilRtProgramDesc::HitGroup g;
            GuestAddr name = 0, any = 0, closest = 0, intersection = 0;
            (void)read_pointer(d + l.HitGroupExport, name);
            (void)read_pointer(d + l.AnyHitShaderImport, any);
            (void)read_pointer(d + l.ClosestHitShaderImport, closest);
            (void)read_pointer(d + l.IntersectionShaderImport, intersection);
            if (!read_wide(name, g.name)) {
                refuse(E_INVALIDARG, "a hit group without a name", false);
                return;
            }
            g.procedural = read_u32(d + l.Type) == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            if (any != 0) (void)read_wide(any, g.any_hit);
            if (closest != 0) (void)read_wide(closest, g.closest_hit);
            if (intersection != 0) (void)read_wide(intersection, g.intersection);
            if (g.procedural == g.intersection.empty()) {
                refuse(E_INVALIDARG, "a hit group whose type and intersection shader disagree", false);
                return;
            }
            if (!new_name(g.name)) return;
            so->kinds[g.name] = 0;
            added_groups[g.name] = -1;
            so->hit_groups.push_back(std::move(g));
            break;
        }
        default:
            refuse(E_INVALIDARG, "a subobject type raytracing state objects do not take", sub_type >= 13);
            return;
        }
    }
    const bool external_ok = type == D3D12_STATE_OBJECT_TYPE_COLLECTION &&
                             (soc_flags & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS) != 0;
    // Association targets: a subobject of this description (associable
    // kinds only), or a library subobject by name.
    for (Assoc& a : assocs) {
        if (a.target >= 0 && a.scope == -1 && a.target_name.empty()) {
            const i32 at = a.target < static_cast<i32>(sob_of.size()) ? sob_of[static_cast<usize>(a.target)] : -1;
            if (at < 0) {
                refuse(E_INVALIDARG, "an association of a subobject that cannot be associated", false);
                return;
            }
            a.target = at;
            continue;
        }
        // By name: the library's own subobject first, then any library's.
        a.target = -1;
        for (usize s = 0; s < sobs.size(); ++s) {
            if (sobs[s].scope >= 0 && sobs[s].name == a.target_name &&
                (a.target < 0 || sobs[s].scope == a.scope)) {
                a.target = static_cast<i32>(s);
            }
        }
        if (a.target < 0) {
            // Measured (d3d12_a48 subobjects "an unknown library subobject").
            if (external_ok) continue;
            refuse(E_INVALIDARG, "an association of a subobject the libraries do not declare", false);
            return;
        }
    }
    // Explicit associations name exports of the state object (measured,
    // d3d12_a48 subobjects "association to ms and an unknown export", also
    // in a collection).
    for (const Assoc& a : assocs) {
        for (const std::string& e : a.exports) {
            if (!so->kinds.contains(e) && !external_ok) {
                refuse(E_INVALIDARG, "an association names an export the state object does not define", false);
                return;
            }
        }
    }

    // ---- pass 2: each added export's subobject of each kind
    // (Raytracing.md, "Subobject association behavior"; measured, d3d12_a48
    // subobjects): a state-object explicit association, else a
    // state-object default (overriding the libraries' own), else a library
    // explicit association, else a default of the export's own library. An
    // association of a hit group reaches its shaders. Two different
    // subobjects at the deciding level conflict. ----
    std::map<std::string, std::vector<std::string>> groups_of;  // function -> its hit groups here
    for (const auto& g : so->hit_groups) {
        if (!added_groups.contains(g.name)) continue;
        for (const std::string* part : {&g.any_hit, &g.closest_hit, &g.intersection}) {
            if (!part->empty()) groups_of[*part].push_back(g.name);
        }
    }
    const auto same = [&](i32 x, i32 y) {
        const Sub& a = sobs[static_cast<usize>(x)];
        const Sub& b = sobs[static_cast<usize>(y)];
        if (a.kind == kSubGlobalRoot || a.kind == kSubLocalRoot) return a.root.get() == b.root.get();
        return a.a == b.a && a.b == b.b;
    };
    bool conflict = false;
    const auto resolve = [&](u32 kind, const std::string& function, i32 scope) -> i32 {
        std::vector<std::string> names{function};
        if (const auto g = groups_of.find(function); g != groups_of.end()) {
            names.insert(names.end(), g->second.begin(), g->second.end());
        }
        const auto names_it = [&](const Assoc& a) {
            return std::any_of(a.exports.begin(), a.exports.end(), [&](const std::string& e) {
                return std::find(names.begin(), names.end(), e) != names.end();
            });
        };
        const auto of_kind = [&](i32 t) { return t >= 0 && sobs[static_cast<usize>(t)].kind == kind; };
        const auto pick = [&](const std::vector<i32>& found) -> i32 {
            for (const i32 t : found) {
                if (!same(t, found[0])) conflict = true;
            }
            return found.empty() ? -1 : found[0];
        };
        std::vector<i32> found;
        for (const Assoc& a : assocs) {
            if (a.scope == -1 && !a.exports.empty() && of_kind(a.target) && names_it(a)) found.push_back(a.target);
        }
        if (!found.empty()) return pick(found);
        for (usize s = 0; s < sobs.size(); ++s) {
            if (sobs[s].scope != -1 || sobs[s].kind != kind) continue;
            const bool referenced = std::any_of(assocs.begin(), assocs.end(), [&](const Assoc& a) {
                return a.scope == -1 && a.target == static_cast<i32>(s);
            });
            if (!referenced) found.push_back(static_cast<i32>(s));
        }
        for (const Assoc& a : assocs) {
            if (a.scope == -1 && a.exports.empty() && of_kind(a.target)) found.push_back(a.target);
        }
        if (!found.empty()) return pick(found);
        for (const Assoc& a : assocs) {
            if (a.scope >= 0 && !a.exports.empty() && of_kind(a.target) && names_it(a)) found.push_back(a.target);
        }
        if (!found.empty()) return pick(found);
        if (scope < 0) return -1;
        for (usize s = 0; s < sobs.size(); ++s) {
            if (sobs[s].scope != scope || sobs[s].kind != kind) continue;
            const bool referenced = std::any_of(assocs.begin(), assocs.end(), [&](const Assoc& a) {
                return a.scope == scope && a.target == static_cast<i32>(s);
            });
            if (!referenced) found.push_back(static_cast<i32>(s));
        }
        for (const Assoc& a : assocs) {
            if (a.scope == scope && a.exports.empty() && of_kind(a.target)) found.push_back(a.target);
        }
        return pick(found);
    };
    i32 shader_config = -1, pipeline_config = -1;
    bool configs_match = true;
    bool missing_config = false;
    for (const auto& [name, scope] : added_functions) {
        const i32 local = resolve(kSubLocalRoot, name, scope);
        const i32 global = resolve(kSubGlobalRoot, name, scope);
        const i32 sc = resolve(kSubShaderConfig, name, scope);
        const i32 pc = resolve(kSubPipelineConfig, name, scope);
        if (conflict) {
            refuse(E_INVALIDARG, "conflicting subobject associations", false);
            return;
        }
        if (local >= 0) so->local_roots[name] = sobs[static_cast<usize>(local)].root;
        if (global >= 0) so->global_roots[name] = sobs[static_cast<usize>(global)].root;
        // Raytracing.md ("Subobject association requirements"): the shader
        // and pipeline configs are required and match for every export.
        if (sc < 0 || pc < 0) missing_config = true;
        if (sc >= 0) {
            if (shader_config >= 0 && !same(sc, shader_config)) configs_match = false;
            shader_config = sc;
        }
        if (pc >= 0) {
            if (pipeline_config >= 0 && !same(pc, pipeline_config)) configs_match = false;
            pipeline_config = pc;
        }
    }
    // A hit group's local root signature is its shaders' (they must agree).
    for (const auto& g : so->hit_groups) {
        if (!added_groups.contains(g.name)) continue;
        std::shared_ptr<const RootLayout> root;
        bool first = true;
        for (const std::string* part : {&g.any_hit, &g.closest_hit, &g.intersection}) {
            if (part->empty()) continue;
            const auto found = so->local_roots.find(*part);
            const std::shared_ptr<const RootLayout> r = found != so->local_roots.end() ? found->second : nullptr;
            if (!first && r != root) {
                refuse(E_INVALIDARG, "a hit group whose shaders have different local root signatures", false);
                return;
            }
            root = r;
            first = false;
        }
        if (root) so->local_roots[g.name] = root;
    }
    if (!configs_match) {
        refuse(E_INVALIDARG, "exports with different shader or pipeline configs", false);
        return;
    }
    // With no shaders the description's own configs stand (AddToStateObject
    // compares them with the parent's).
    if (added_functions.empty()) {
        for (usize s = 0; s < sobs.size(); ++s) {
            if (sobs[s].scope != -1) continue;
            if (sobs[s].kind == kSubShaderConfig && shader_config < 0) shader_config = static_cast<i32>(s);
            if (sobs[s].kind == kSubPipelineConfig && pipeline_config < 0) pipeline_config = static_cast<i32>(s);
        }
    }
    if (add) {
        // Measured (d3d12_a48 add_*, RTX 4080): the addition repeats the
        // parent's ALLOW_STATE_OBJECT_ADDITIONS config, a shader config
        // equal to the parent's and a pipeline config of the parent's
        // MaxTraceRecursionDepth (CONFIG or CONFIG1, its flags free), with
        // or without shaders; its global root signature only has to bind
        // what its own shaders read. (A different MaxAttributeSizeInBytes
        // removed the device there; it is refused here like the payload.)
        if ((soc_flags & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) == 0) {
            refuse(E_INVALIDARG, "an addition without ALLOW_STATE_OBJECT_ADDITIONS", false);
            return;
        }
        const Sub* sc = shader_config >= 0 ? &sobs[static_cast<usize>(shader_config)] : nullptr;
        const Sub* pc = pipeline_config >= 0 ? &sobs[static_cast<usize>(pipeline_config)] : nullptr;
        if (sc == nullptr || pc == nullptr || !parent->has_shader_config || !parent->has_pipeline_config ||
            sc->a != parent->max_payload || sc->b != parent->max_attributes || pc->a != parent->max_recursion) {
            refuse(E_INVALIDARG, "an addition whose configs are not the parent's", false);
            return;
        }
    } else {
        so->flags = soc_flags;
        if (shader_config >= 0) {
            const Sub& sc = sobs[static_cast<usize>(shader_config)];
            if (so->has_shader_config && (so->max_payload != sc.a || so->max_attributes != sc.b)) {
                refuse(E_INVALIDARG, "a shader config different from a collection's", false);
                return;
            }
            so->has_shader_config = true;
            so->max_payload = sc.a;
            so->max_attributes = sc.b;
        }
        if (pipeline_config >= 0) {
            const Sub& pc = sobs[static_cast<usize>(pipeline_config)];
            if (so->has_pipeline_config && so->max_recursion != pc.a) {
                refuse(E_INVALIDARG, "a pipeline config different from a collection's", false);
                return;
            }
            so->has_pipeline_config = true;
            so->max_recursion = pc.a;
            so->pipeline_flags = pc.b;
        }
    }
    if (missing_config && type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
        refuse(E_INVALIDARG, "a raytracing shader without a shader config or pipeline config", false);
        return;
    }
    (void)any_soc;
    // The root signature the kernel is compiled against: a raygen's global
    // one (DispatchRays binds the command list's own).
    so->global_root = nullptr;
    so->has_global_root = false;
    for (const auto& [name, root] : so->global_roots) {
        const auto kind = so->kinds.find(name);
        if (!so->has_global_root || (kind != so->kinds.end() && kind->second == 7)) {
            so->global_root = root;
            so->has_global_root = true;
            if (kind != so->kinds.end() && kind->second == 7) break;
        }
    }
    // ---- identifiers ----
    for (const auto& [name, kind] : so->kinds) {
        if ((kind == 7 || kind == 11 || kind == 12 || kind == 0) && !so->indices.contains(name)) {
            so->indices[name] = m_next_identifier++;
        }
    }
    for (const auto& g : so->hit_groups) {
        for (const std::string* part : {&g.any_hit, &g.closest_hit, &g.intersection}) {
            if (!part->empty() && !so->kinds.contains(*part)) {
                refuse(E_INVALIDARG, "a hit group imports a shader the state object does not define",
                       false);
                return;
            }
        }
    }
    const usize bytes = std::max<usize>(so->indices.size() * kShaderIdentifierBytes, 64);
    so->identifier_bytes = (bytes + 0xFFFFu) & ~usize{0xFFFFu};
    so->identifiers = m_memory.map_first_fit(0x30000000ull, so->identifier_bytes, MemoryProtection::ReadWrite);
    if (so->identifiers == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    u32 slot = 0;
    for (const auto& [name, index] : so->indices) {
        u8 id[kShaderIdentifierBytes] = {};
        std::memcpy(id, &kRtIdentifierMagic, 4);
        std::memcpy(id + 8, &index, 4);
        (void)m_memory.write_bytes(so->identifiers + slot * kShaderIdentifierBytes, id, sizeof(id));
        so->identifier_slot[name] = slot++;
    }
    // ---- what the lowering sees: libraries, hit groups, configs, and each
    // export's root signatures ----
    graphics::DxilRtProgramDesc d;
    for (const auto& lib : so->libraries) d.libraries.push_back({lib.bytes.data(), lib.bytes.size(), lib.exports});
    d.hit_groups = so->hit_groups;
    d.max_recursion = so->max_recursion;
    d.pipeline_flags = so->pipeline_flags;
    d.max_payload_bytes = so->max_payload;
    d.max_attribute_bytes = so->max_attributes;
    d.indices = so->indices;
    d.check_sizes = so->has_shader_config;
    std::vector<SamplerState> local_samplers;
    std::map<std::pair<const RootLayout*, usize>, u32> sampler_index;
    for (const auto& [name, root] : so->local_roots) {
        d.local_roots[name] = rt_root(*root, true, &local_samplers, &sampler_index);
    }
    // A hit group's shaders use its local root signature.
    for (const auto& g : so->hit_groups) {
        const auto found = d.local_roots.find(g.name);
        if (found == d.local_roots.end()) continue;
        for (const std::string* part : {&g.any_hit, &g.closest_hit, &g.intersection}) {
            if (!part->empty() && !d.local_roots.contains(*part)) d.local_roots[*part] = found->second;
        }
    }
    for (const auto& [name, root] : so->global_roots) d.global_roots[name] = rt_root(*root, false, nullptr, nullptr);
    // Measured (d3d12_a48 local_rules): a collection checks the bindings
    // too, unless it allows local dependencies on external definitions.
    d.validate_bindings = type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ||
                          (so->flags & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS) == 0;
    if (type == D3D12_STATE_OBJECT_TYPE_COLLECTION && d.validate_bindings && !so->libraries.empty()) {
        const graphics::DxilRtProgram check = graphics::dxil_raytracing_validate(d);
        if (!check.ok) {
            if (check.unimplemented) unimplemented(api, check.failure.c_str());
            else d3d12_diag("%s: %s", api, check.failure.c_str());
            m.set_return(E_INVALIDARG);
            return;
        }
    }
    // ---- the megakernel ----
    if (type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
        // Measured (a12 state_object "RAYTRACING_PIPELINE, none"): a
        // pipeline without shaders is created; one with shaders needs both
        // configs.
        if (so->indices.empty()) {
            m.set_return(return_new_object(std::move(so), iid, out));
            return;
        }
        if (!so->has_shader_config || !so->has_pipeline_config) {
            refuse(E_INVALIDARG, "a raytracing pipeline without a shader config or pipeline config", false);
            return;
        }
        auto pso = std::make_shared<PipelineState>();
        pso->device = device.address;
        pso->compute = true;
        pso->root = so->has_global_root ? so->global_root : std::make_shared<RootLayout>();
        // The megakernel's lowering, translation and Metal compile run
        // outside the frontend lock, as a pipeline state's do
        // (build_pipeline_unlocked): `d`, `so` and `pso` are this call's own
        // (`so` is not published yet), and the capabilities are made first.
        (void)caps();
        const GuestAddr device_address = device.address;
        const u64 device_serial = device.serial;
        graphics::DxilRtProgram program;
        u32 built = S_OK;
        {
            FrontendUnlock unlocked(*this);
            program = graphics::dxil_raytracing_to_dxbc(d);
            if (!program.ok) {
                if (program.unimplemented) unimplemented(api, program.failure.c_str());
                else d3d12_diag("%s: %s", api, program.failure.c_str());
                built = E_INVALIDARG;
            } else {
                built = compile_shader(program.container, graphics::ShaderStage::Compute,
                                       pso->cs, api, pso->root.get(),
                                       D3D12_SHADER_VISIBILITY_ALL);
                if (built == S_OK) built = finish_compute_pipeline(*pso, api);
            }
        }
        if (built == S_OK &&
            find_serial<Device>(device_address, device_serial, Kind::Device) == nullptr) {
            built = E_INVALIDARG;  // the device died meanwhile
        }
        if (built != S_OK) {
            m.set_return(built);
            return;
        }
        pso->cs.rt_samplers = std::move(local_samplers);
        so->local_views = program.local_views;
        so->record_views.clear();
        for (const auto& [owner, list] : program.owner_views) {
            if (const auto index = so->indices.find(owner); index != so->indices.end()) {
                so->record_views[index->second] = list;
            }
        }
        so->kernel = std::move(pso);
    }
    m.set_return(return_new_object(std::move(so), iid, out));
}

void Frontend::state_object_properties_method(Marshaler& m, StateObject& so, usize slot) {
    using S = StateObjectPropertiesSlot;
    const auto read_name = [&](GuestAddr at, std::string& s) {
        s.clear();
        if (at == 0) return false;
        for (u32 k = 0; k < 4096; ++k) {
            const auto unit = m_memory.read_u16(at + k * 2u);
            if (!unit.is_ok()) return false;
            if (unit.value() == 0) return true;
            s.push_back(unit.value() < 0x80 ? static_cast<char>(unit.value()) : '?');
        }
        return false;
    };
    // IUnknown (slots 0-2) is the state object's own (tearoff_method).
    switch (slot) {
    case S::GetShaderIdentifier: {
        // Raygen, miss, callable shaders and hit groups only; anything
        // else (or unknown) is NULL.
        std::string name;
        const auto found = read_name(m.arg_ptr(1), name) ? so.identifier_slot.find(name) : so.identifier_slot.end();
        m.set_return(found == so.identifier_slot.end()
                                 ? 0
                                 : so.identifiers + found->second * kShaderIdentifierBytes);
        return;
    }
    case S::GetShaderStackSize: {
        // The megakernel keeps a ray's state in registers: no shader needs
        // stack. "hitgroup::closesthit|anyhit|intersection" names a hit
        // group's part; a hit group itself, or an unknown name, is
        // 0xffffffff (Raytracing.md, GetShaderStackSize).
        std::string name;
        u64 size = kStackUnknown;
        if (read_name(m.arg_ptr(1), name)) {
            const usize sep = name.find("::");
            if (sep == std::string::npos) {
                const auto k = so.kinds.find(name);
                if (k != so.kinds.end() && (k->second == 7 || k->second == 11 || k->second == 12)) size = 0;
            } else {
                const std::string group = name.substr(0, sep), part = name.substr(sep + 2);
                for (const auto& g : so.hit_groups) {
                    if (g.name != group) continue;
                    if ((part == "closesthit" && !g.closest_hit.empty()) ||
                        (part == "anyhit" && !g.any_hit.empty()) ||
                        (part == "intersection" && !g.intersection.empty())) {
                        size = 0;
                    }
                }
            }
        }
        m.set_return_i64(size);
        return;
    }
    case S::GetPipelineStackSize:
        // Measured (d3d12_a47 collection, a12 state_object, RTX 4080): a
        // collection or executable answers 0xffffffff, not the spec's 0.
        m.set_return_i64(so.type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ? kStackUnknown : so.stack_size);
        return;
    case S::SetPipelineStackSize: {
        usize next = 0;
        const u64 size = read_arg_u64(m, m_is_64bit, 1, next);
        if (so.type != D3D12_STATE_OBJECT_TYPE_COLLECTION) so.stack_size = size;
        m.set_return(0);
        return;
    }
    case S::GetProgramIdentifier: {
        // ID3D12StateObjectProperties1. Hidden return pointer (RetVal,
        // pProgramName): generic programs belong to work graphs, which this
        // device lacks, so no program exists -- the zero identifier
        // (D3D12_PROGRAM_IDENTIFIER { UINT64 OpaqueData[4] }).
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            for (u32 i = 0; i < 32; i += 8) (void)m_memory.write_u64(out + i, 0);
        }
        m.set_return(out);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12LifetimeTracker
// =============================================================================

void Frontend::create_lifetime_tracker(Marshaler& m, Device& device) {
    // CreateLifetimeTracker(ID3D12LifetimeOwner *pOwner, REFIID riid,
    //     void **ppvTracker). Measured (a12 lifetime): a NULL owner is
    //     E_INVALIDARG with *ppv untouched; ppv NULL E_UNEXPECTED; the owner
    //     gets no reference.
    const GuestAddr owner = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (owner == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        m.set_return(E_UNEXPECTED);
        return;
    }
    auto tracker = std::make_unique<LifetimeTracker>();
    tracker->device = device.address;
    tracker->owner = owner;
    m.set_return(return_new_object(std::move(tracker), m.arg_ptr(2), out));
}

void Frontend::lifetime_tracker_method(Marshaler& m, LifetimeTracker& tracker,
                                       usize slot) {
    (void)tracker;
    if (slot == LifetimeTrackerSlot::DestroyOwnedObject) {
        // Only an object created with this tracker is owned
        // (ID3D12CompatibilityDevice::CreateSharedResource's pLifetimeTracker,
        // which this frontend refuses): measured, any other is E_INVALIDARG,
        // the owner not notified. (NULL faults in d3d12.dll; refused.)
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(E_NOTIMPL);
}

}  // namespace vitreo::winapi::d3d12
