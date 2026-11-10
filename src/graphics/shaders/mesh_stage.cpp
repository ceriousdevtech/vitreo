// SPDX-License-Identifier: LGPL-2.1-only

/// @file mesh_stage.cpp
/// @brief D3D12 mesh / amplification shaders on Metal (mesh_stage.hpp).

#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/raster_emulation_types.hpp"

#include "graphics/shaders/dxbc_to_spirv.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

namespace vitreo::graphics {
namespace {

// D3D_NAME (d3dcommon.h).
constexpr u32 kNamePosition = 1, kNameClip = 2, kNameCull = 3, kNameRtai = 4, kNameVpai = 5,
              kNamePrimitiveId = 7, kNameIsFrontFace = 9, kNameSampleIndex = 10,
              kNameBarycentrics = 23, kNameShadingRate = 24, kNameCullPrimitive = 25;

bool same_semantic(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<u32> components(u8 mask) {
    std::vector<u32> out;
    for (u32 c = 0; c < 4; ++c) {
        if ((mask >> c) & 1u) out.push_back(c);
    }
    return out;
}

std::string u(u32 v) { return std::to_string(v) + "u"; }

const char* vec_type(u8 component_type) {
    return component_type == 1 ? "uint4" : component_type == 2 ? "int4" : "float4";
}

/// The pixel-input field of register @p reg, or null when no output feeds it.
const MeshStageLayout::Field* per_vertex_field(const MeshStageLayout& l, u32 reg) {
    for (const auto& f : l.fields) {
        if (f.location == reg) return &f;
    }
    return nullptr;
}

}  // namespace

u32 MeshStageLayout::staging_bytes() const {
    if (state.is_amplification()) return 12u;
    const u32 nv = std::max(state.max_vertices, 1u);
    const u32 np = std::max(state.max_primitives, 1u);
    return 4u * (nv * std::max(vertex_scalars, 1u) + np * std::max(primitive_scalars, 1u) +
                 np * vertices_per_primitive() + 2u + state.payload_words);
}

u32 MeshStageLayout::device_staging_words() const {
    const auto up4 = [](u32 v) { return (v + 3u) / 4u * 4u; };
    const u32 nv = std::max(state.max_vertices, 1u);
    const u32 np = std::max(state.max_primitives, 1u);
    return up4(nv * std::max(vertex_scalars, 1u)) + up4(np * std::max(primitive_scalars, 1u)) +
           up4(np * vertices_per_primitive()) + 4u;  // + the counts
}

u32 mesh_stage_threadgroup_bytes(const MeshStageLayout& l, bool payload_in_place) {
    if (l.device_staging && l.state.is_mesh()) {
        // Without an amplification shader, the zeroed payload copy: the
        // counts are in device memory too. Metal also counts the
        // metal::mesh against one limit with threadgroup memory (measured
        // on the M4 Pro: "Threadgroup memory size and mesh size (28680 +
        // 32768) exceed the maximum limit allowed (61440)"), which a 28 KB
        // groupshared shader with 32 KB of outputs reaches exactly.
        // Behind an amplification shader, the shared slot word
        // (mesh_stage_device_staging).
        return 4u * (payload_in_place ? 1u : l.state.payload_words);
    }
    const u32 all = l.staging_bytes();
    if (!payload_in_place || l.state.is_amplification()) return all;
    return all - 4u * l.state.payload_words;
}

bool mesh_stage_direct_payload(std::string& msl, u32 payload_words) {
    if (payload_words == 0) return false;
    const std::string n = std::to_string(payload_words);
    const std::string param = "threadgroup uint (&vitreo_ms_payload)[" + n + "]";
    const std::string local = "    threadgroup uint vitreo_ms_payload[" + n + "];\n";
    const std::string copy_head = "for (uint i = ";
    const std::string copy_tail = ") vitreo_ms_payload[i] = vitreo_payload.w[i];";
    const auto once = [&](const std::string& what) {
        const usize at = msl.find(what);
        return at != std::string::npos && msl.find(what, at + 1) == std::string::npos;
    };
    if (!once(param) || !once(local) || !once(copy_tail)) return false;
    // The entry's call passes the threadgroup array by name; it is the only
    // other use of the name outside the helper's body (which uses the
    // parameter).
    const usize call = msl.find("main0(", msl.find(local));
    if (call == std::string::npos) return false;
    const usize call_end = msl.find(");", call);
    const usize arg = msl.find("vitreo_ms_payload", call);
    if (call_end == std::string::npos || arg == std::string::npos || arg > call_end) {
        return false;
    }
    std::string out = msl;
    // From the back, so earlier offsets stay valid.
    out.replace(arg, std::string("vitreo_ms_payload").size(), "vitreo_payload.w");
    const usize tail = out.find(copy_tail);
    const usize line = out.rfind('\n', tail);
    const usize head = out.find(copy_head, line);
    if (head == std::string::npos || head > tail) return false;
    out.erase(head, tail + copy_tail.size() - head);
    out.erase(out.find(local), local.size());
    out.replace(out.find(param), param.size(),
                "const object_data uint (&vitreo_ms_payload)[" + n + "]");
    msl = std::move(out);
    return true;
}

namespace {

/// The entry `qualifier main0(...)` of @p msl: [params begin, params end)
/// and the offset just past the body's opening brace line.
bool entry_span(const std::string& msl, const std::string& qualifier, usize& params, usize& params_end,
                usize& body) {
    const usize at = msl.find(qualifier + "main0(");
    if (at == std::string::npos) return false;
    params = at + qualifier.size() + 6;
    params_end = msl.find(")\n{\n", params);
    if (params_end == std::string::npos) return false;
    body = params_end + 4;
    return true;
}

/// Replaces SPIRV-Cross's [[threadgroup_position_in_grid]] entry argument,
/// if any, by a local offset by the chunk origin.
void offset_group_id(std::string& msl, const std::string& qualifier) {
    usize params = 0, params_end = 0, body = 0;
    if (!entry_span(msl, qualifier, params, params_end, body)) return;
    const std::string sig = msl.substr(params, params_end - params);
    // Vitreo's own vitreo_tg (mesh_stage_parameters) comes first and stays.
    const std::regex arg(R"(, uint3 ([A-Za-z_0-9]+) \[\[threadgroup_position_in_grid\]\])");
    std::string name;
    usize in_sig = 0, len = 0;
    for (auto it = std::sregex_iterator(sig.begin(), sig.end(), arg); it != std::sregex_iterator(); ++it) {
        if ((*it)[1].str() == "vitreo_tg") continue;
        name = (*it)[1].str();
        in_sig = static_cast<usize>(it->position(0));
        len = static_cast<usize>(it->length(0));
        break;
    }
    if (name.empty()) return;
    // vitreo_origin.w 1: a chunk of an indirect dispatch, a linear run of
    // groups from .x of a dispatch .y groups wide and .z high
    // (d3d12_replay_mesh.cpp); else a box at .xyz (.w 0, or 2 + the slot
    // counter word of an amplified chunk).
    msl.insert(body, "    const uint vitreo_linear = vitreo_origin.x + vitreo_tg.x;\n"
                     "    const uint3 " + name + " = vitreo_origin.w != 1u ? vitreo_tg + vitreo_origin.xyz"
                     " : uint3(vitreo_linear % max(vitreo_origin.y, 1u), (vitreo_linear / max(vitreo_origin.y, 1u))"
                     " % max(vitreo_origin.z, 1u), vitreo_linear / max(vitreo_origin.y * vitreo_origin.z, 1u));\n");
    msl.erase(params + in_sig, len);
}

}  // namespace

bool mesh_stage_device_staging(std::string& msl, const MeshStageLayout& l, u32 payload_words) {
    if (!l.device_staging || !l.state.is_mesh()) return false;
    const u32 nv = std::max(l.state.max_vertices, 1u);
    const u32 np = std::max(l.state.max_primitives, 1u);
    const auto up4 = [](u32 v) { return (v + 3u) / 4u * 4u; };
    const struct {
        const char* name;
        u32 words;
    } arrays[4] = {{"vitreo_ms_vertices", nv * std::max(l.vertex_scalars, 1u)},
                   {"vitreo_ms_primitives", np * std::max(l.primitive_scalars, 1u)},
                   {"vitreo_ms_indices", np * l.vertices_per_primitive()},
                   {"vitreo_ms_counts", 2u}};
    std::string out = msl;
    // Without an amplification shader the slot is the group's index in the
    // chunk's grid. An amplified mesh group cannot name its object group
    // (and a 16 KB payload has no room to carry it): it takes the next slot
    // of the chunk from a counter, the scratch word vitreo_origin.w - 2
    // (zeroed per chunk); a chunk of N object groups launches at most
    // N * kMeshSlotsPerObjectGroup mesh groups, the slots it has.
    const std::string slot =
        payload_words != 0 ? std::string("vitreo_slot")
                           : std::string("((vitreo_tg.z * vitreo_tgs.y + vitreo_tg.y) * vitreo_tgs.x + vitreo_tg.x)");
    u32 offset = 0;
    for (const auto& a : arrays) {
        const std::string n = std::to_string(a.words);
        const std::string param = std::string("threadgroup uint (&") + a.name + ")[" + n + "]";
        const std::string local = std::string("    threadgroup uint ") + a.name + "[" + n + "];\n";
        const usize p = out.find(param);
        if (p == std::string::npos || out.find(param, p + 1) != std::string::npos) return false;
        out.replace(p, param.size(), std::string("device uint* ") + a.name);
        const usize q = out.find(local);
        if (q == std::string::npos || out.find(local, q + 1) != std::string::npos) return false;
        out.replace(q, local.size(),
                    std::string("    device uint* ") + a.name + " = vitreo_ms_scratch + " + slot + " * " +
                        u(l.device_staging_words()) + " + " + u(offset) + ";\n");
        offset += up4(a.words);
    }
    if (payload_words == 0) {
        offset_group_id(out, "[[mesh]] void ");
    } else {
        usize params = 0, params_end = 0, body = 0;
        if (!entry_span(out, "[[mesh]] void ", params, params_end, body)) return false;
        // The thread's index: SPIRV-Cross's own argument when there is one
        // (Metal refuses a second argument of the same attribute).
        std::string tid = "vitreo_tid";
        std::smatch m;
        const std::string sig = out.substr(params, params_end - params);
        if (std::regex_search(sig, m, std::regex(R"(uint ([A-Za-z_0-9]+) \[\[thread_index_in_threadgroup\]\])"))) {
            tid = m[1].str();
        }
        out.insert(body,
                   "    threadgroup uint vitreo_slot_shared;\n"
                   "    if (" + tid + " == 0u) vitreo_slot_shared = atomic_fetch_add_explicit("
                   "(device atomic_uint*)(vitreo_ms_scratch + (vitreo_origin.w - 2u)), 1u, memory_order_relaxed);\n"
                   "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                   "    const uint vitreo_slot = vitreo_slot_shared;\n");
        if (tid == "vitreo_tid") out.insert(params_end, ", uint vitreo_tid [[thread_index_in_threadgroup]]");
    }
    msl = std::move(out);
    return true;
}

bool object_stage_device_staging(std::string& msl) {
    usize params = 0, params_end = 0, body = 0;
    if (!entry_span(msl, "[[object]] void ", params, params_end, body)) return false;
    offset_group_id(msl, "[[object]] void ");
    return true;
}

u32 object_stage_buffer_addresses(std::string& msl) {
    // struct spvDescriptorSetBufferN { <space> T* name [[id(k)]]; ... };
    const std::regex member(
        R"(^(\s*)((?:const )?(?:constant|device) [A-Za-z_0-9]+)\* ([A-Za-z_0-9]+) (\[\[id\(\d+\)\]\]);$)");
    const std::regex param(R"(constant (spvDescriptorSetBuffer\d+)& (spvDescriptorSet\d+) \[\[buffer)");
    std::map<std::string, std::string> param_of;  // struct -> parameter name
    for (std::sregex_iterator it(msl.begin(), msl.end(), param), end; it != end; ++it) {
        param_of[(*it)[1]] = (*it)[2];
    }
    std::istringstream in(msl);
    std::string out;
    out.reserve(msl.size() + 256);
    std::string line;
    std::string current;
    std::vector<std::pair<std::string, std::string>> casts;  // "setvar.name", type
    while (std::getline(in, line)) {
        if (line.rfind("struct spvDescriptorSetBuffer", 0) == 0) {
            current = line.substr(std::string("struct ").size());
        } else if (line == "};") {
            current.clear();
        } else if (!current.empty() && param_of.contains(current)) {
            std::smatch m;
            if (std::regex_match(line, m, member)) {
                casts.emplace_back(param_of[current] + "." + m[3].str(), m[2].str());
                line = m[1].str() + "ulong " + m[3].str() + " " + m[4].str() + ";";
            }
        }
        out += line;
        out += '\n';
    }
    if (casts.empty()) return 0;
    for (const auto& [use, type] : casts) {
        const std::string cast = "reinterpret_cast<" + type + "*>(" + use + ")";
        usize at = 0;
        while ((at = out.find(use, at)) != std::string::npos) {
            const usize after = at + use.size();
            const char next = after < out.size() ? out[after] : '\0';
            if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
                at = after;
                continue;
            }
            out.replace(at, use.size(), cast);
            at += cast.size();
        }
    }
    msl = std::move(out);
    return static_cast<u32>(casts.size());
}

MeshStageLayout amplification_stage_layout(const DxbcContainer& as) {
    MeshStageLayout out{};
    out.state = as.mesh;
    out.vertex_slot.fill(kMeshNoSlot);
    out.primitive_slot.fill(kMeshNoSlot);
    return out;
}

bool build_mesh_stage_layout(const DxbcContainer& ms, const std::vector<DxbcSignatureElement>* ps_inputs,
                             MeshStageLayout& out, std::string& why, bool& invalid) {
    out = MeshStageLayout{};
    out.state = ms.mesh;
    out.vertex_slot.fill(kMeshNoSlot);
    out.primitive_slot.fill(kMeshNoSlot);
    invalid = false;
    const auto refuse = [&](bool is_invalid, std::string reason) {
        invalid = is_invalid;
        why = std::move(reason);
        return false;
    };
    if (!ms.mesh.is_mesh()) return refuse(true, "not a mesh shader");
    const auto slot = [](std::array<u16, 128>& table, u32& count, u32 reg, u32 comp) -> u16 {
        u16& s = table[(reg & 31u) * 4u + (comp & 3u)];
        if (s == kMeshNoSlot) s = static_cast<u16>(count++);
        return s;
    };
    const auto vslot = [&](u32 reg, u32 comp) { return slot(out.vertex_slot, out.vertex_scalars, reg, comp); };
    const auto pslot = [&](u32 reg, u32 comp) { return slot(out.primitive_slot, out.primitive_scalars, reg, comp); };

    bool position = false;
    for (const auto& e : ms.output_signature) {
        if (e.register_num >= 32) return refuse(true, "a vertex output without a register");
        const auto comps = components(e.component_mask);
        switch (e.system_value_type) {
        case kNamePosition:
            position = true;
            for (usize k = 0; k < comps.size() && k < 4; ++k) out.position[k] = vslot(e.register_num, comps[k]);
            break;
        case kNameClip:
            for (u32 c : comps) out.clip_distance.push_back(vslot(e.register_num, c));
            break;
        case kNameCull:
            for (u32 c : comps) out.cull_distance.push_back(vslot(e.register_num, c));
            break;
        case 0:
            break;
        case kNameRtai:
        case kNameVpai:
            // "SV_RenderTargetIndex and SV_ViewportIndex ... can only be
            // per-primitive attributes in MeshShader."
            return refuse(true, "SV_RenderTargetArrayIndex / SV_ViewportArrayIndex as a vertex output");
        default:
            return refuse(false, "vertex output system value " + std::to_string(e.system_value_type));
        }
    }
    if (!position) return refuse(true, "a mesh shader without an SV_Position vertex output");
    for (const auto& e : ms.patch_constant_signature) {
        if (e.system_value_type == kNameCullPrimitive) {
            out.cull_primitive_slot = static_cast<u16>(out.primitive_scalars++);
            continue;
        }
        if (e.register_num >= 32) return refuse(true, "a primitive output without a register");
        const auto comps = components(e.component_mask);
        switch (e.system_value_type) {
        case kNameRtai:
            if (!comps.empty()) out.layer = pslot(e.register_num, comps[0]);
            break;
        case kNameVpai:
            if (!comps.empty()) out.viewport = pslot(e.register_num, comps[0]);
            break;
        case 0:
        case kNamePrimitiveId:  // an ordinary per-primitive value here
            break;
        case kNameShadingRate:
            // D3D12_OPTIONS6 VariableShadingRateTier is NOT_SUPPORTED.
            return refuse(false, "SV_ShadingRate (variable-rate shading is not supported)");
        default:
            return refuse(false, "primitive output system value " + std::to_string(e.system_value_type));
        }
    }
    if (ps_inputs == nullptr) return true;

    // Which pixel input registers take elements of both kinds: linkage is
    // by semantic, not by packed location (DirectX-Specs "Mesh Shader":
    // "Elements must no longer be linked by the packed location"; "Vertex
    // and primitive attributes will not be packed into the same row" -- on
    // the mesh side), so a pixel row may pack a vertex attribute with a
    // primitive one. Its primitive-fed components then travel apart
    // (ps_primitive_split).
    std::array<u8, 32> fed_by_vertex{};
    for (const auto& p : *ps_inputs) {
        if (p.register_num >= 32 || (p.system_value_type != 0 && p.system_value_type != kNameClip &&
                                     p.system_value_type != kNameCull)) {
            continue;
        }
        for (const auto& m : ms.output_signature) {
            const bool same = m.semantic_index == p.semantic_index &&
                              (p.system_value_type != 0 ? m.system_value_type == p.system_value_type
                                                        : m.system_value_type == 0 &&
                                                              same_semantic(m.semantic_name, p.semantic_name));
            if (same) fed_by_vertex[p.register_num] = 1;
        }
    }
    for (const auto& p : *ps_inputs) {
        const u32 sv = p.system_value_type;
        if (sv == kNamePosition || sv == kNameIsFrontFace || sv == kNameSampleIndex || sv == kNameBarycentrics ||
            sv == kNameRtai || sv == kNameVpai || p.register_num >= 32) {
            continue;  // from the rasterizer, not the mesh
        }
        // SV_PrimitiveID: "there is no system generated primitiveID" -- the
        // pixel shader reads the mesh shader's per-primitive SV_PrimitiveID
        // (a flat user varying, DxbcTranslationOptions::primitive_id_varying;
        // Metal's [[primitive_id]] is the rasterizer's), and a mesh shader
        // that writes none fails (E_INVALIDARG, measured: a42 primid).
        if (sv != 0 && sv != kNameClip && sv != kNameCull && sv != kNamePrimitiveId) {
            return refuse(false, "pixel input system value " + std::to_string(sv));
        }
        const DxbcSignatureElement* source = nullptr;
        bool per_primitive = false;
        const auto match = [&](const DxbcSignatureElement& m) {
            if (m.semantic_index != p.semantic_index) return false;
            if (sv != 0) return m.system_value_type == sv;
            return m.system_value_type == 0 && same_semantic(m.semantic_name, p.semantic_name);
        };
        for (const auto& m : ms.output_signature) {
            if (match(m)) source = &m;
        }
        for (const auto& m : ms.patch_constant_signature) {
            if (source == nullptr && (match(m) || (sv == 0 && m.system_value_type == kNamePrimitiveId &&
                                                   same_semantic(m.semantic_name, p.semantic_name) &&
                                                   m.semantic_index == p.semantic_index))) {
                source = &m;
                per_primitive = true;
            }
        }
        if (source == nullptr) {
            return refuse(true, "pixel input " + p.semantic_name + std::to_string(p.semantic_index) +
                                    " has no mesh shader output");
        }
        const bool split = per_primitive && fed_by_vertex[p.register_num] != 0;
        const u32 location = split ? primitive_split_input_location(p.register_num) : p.register_num;
        if (split) out.ps_primitive_split[p.register_num] |= static_cast<u8>(p.component_mask & 0xFu);
        auto field = std::find_if(out.fields.begin(), out.fields.end(),
                                  [&](const MeshStageLayout::Field& f) { return f.location == location; });
        if (field == out.fields.end()) {
            MeshStageLayout::Field f{};
            f.location = location;
            f.component_type = p.component_type;
            f.per_primitive = per_primitive;
            out.fields.push_back(f);
            field = out.fields.end() - 1;
        } else if (field->per_primitive != per_primitive) {
            return refuse(false, "a pixel input register fed by both vertex and primitive outputs");
        }
        const auto dst = components(p.component_mask);
        const auto src = components(source->component_mask);
        for (usize k = 0; k < dst.size() && k < src.size(); ++k) {
            field->slot[dst[k]] = per_primitive ? pslot(source->register_num, src[k])
                                                : vslot(source->register_num, src[k]);
        }
    }
    std::sort(out.fields.begin(), out.fields.end(),
              [](const MeshStageLayout::Field& a, const MeshStageLayout::Field& b) {
                  return a.location < b.location;
              });
    return true;
}

bool mesh_stage_capture_layout(MeshStageLayout& l, std::string& why) {
    l.capture.clear();
    l.raster_capture = false;
    if (!l.state.is_mesh()) {
        why = "not a mesh shader";
        return false;
    }
    // D3D11.3 3.4.3: lines are drawn by their own rules, which pass C's
    // triangle coverage test does not express.
    if (l.state.topology == 1) {
        why = "target-independent / conservative rasterization of mesh shader lines";
        return false;
    }
    // Pass C's own data sits past v0..v31 (raster_emulation_types.hpp
    // kRasterEdgeLocation), so every guest register is a record register.
    constexpr u32 kRecordRegisters = 32;
    std::array<bool, kRecordRegisters> used{};
    for (const auto& f : l.fields) {
        if (f.location >= kRecordRegisters) {
            why = "conservative / target-independent rasterization of a mesh output past v31";
            return false;
        }
        used[f.location] = true;
        MeshStageLayout::CaptureRecord r{};
        r.register_num = f.location;
        r.component_type = f.component_type;
        r.per_primitive = f.per_primitive;
        r.slot = f.slot;
        l.capture.push_back(r);
    }
    const auto take = [&](MeshStageLayout::CaptureRecord r) {
        for (u32 reg = 0; reg < kRecordRegisters; ++reg) {
            if (used[reg]) continue;
            used[reg] = true;
            r.register_num = reg;
            l.capture.push_back(r);
            return true;
        }
        return false;
    };
    bool fits = true;
    MeshStageLayout::CaptureRecord position{};
    position.position = true;
    position.slot = l.position;
    fits = fits && take(position);
    for (usize k = 0; k < l.clip_distance.size(); k += 4) {
        MeshStageLayout::CaptureRecord clip{};
        for (usize c = 0; c < 4 && k + c < l.clip_distance.size(); ++c) {
            clip.slot[c] = l.clip_distance[k + c];
            clip.system_values[c] = static_cast<u8>(kNameClip);
        }
        fits = fits && take(clip);
    }
    const std::pair<u16, u32> indices[2] = {{l.layer, kNameRtai}, {l.viewport, kNameVpai}};
    for (const auto& [slot, sv] : indices) {
        if (slot == kMeshNoSlot) continue;
        MeshStageLayout::CaptureRecord index{};
        index.component_type = 1;
        index.per_primitive = true;
        index.slot[0] = slot;
        index.system_values[0] = static_cast<u8>(sv);
        fits = fits && take(index);
    }
    if (!fits) {
        why = "conservative / target-independent rasterization of a mesh shader whose records "
              "need more than 26 registers";
        l.capture.clear();
        return false;
    }
    std::sort(l.capture.begin(), l.capture.end(),
              [](const MeshStageLayout::CaptureRecord& a, const MeshStageLayout::CaptureRecord& b) {
                  return a.register_num < b.register_num;
              });
    l.raster_capture = true;
    return true;
}

std::vector<std::string> mesh_stage_preamble(const MeshStageLayout& l, u32 payload_words) {
    std::vector<std::string> out;
    out.emplace_back("// Generated by vitreo: a D3D12 mesh / amplification shader (mesh_stage.hpp).");
    out.emplace_back("struct vitreo_mesh_payload");
    out.emplace_back("{");
    out.emplace_back("    uint w[" + std::to_string(std::max(payload_words, 1u)) + "];");
    // Device staging: the scratch slot of the object group's first mesh
    // group (written by the object function, read by the mesh function).
    out.emplace_back("};");
    if (l.statistics) {
        // A 64-bit counter as {low, high}: Metal has no 64-bit atomic add.
        out.emplace_back("static inline void vitreo_stat_add(device atomic_uint* s, uint c, uint v)");
        out.emplace_back("{");
        out.emplace_back("    const uint old = atomic_fetch_add_explicit(&s[c * 2u], v, memory_order_relaxed);");
        out.emplace_back("    if (old + v < old) atomic_fetch_add_explicit(&s[c * 2u + 1u], 1u, memory_order_relaxed);");
        out.emplace_back("}");
        if (l.state.is_mesh()) {
            // CPrimitives: the clipper the input-assembler path counts with.
            std::istringstream clip(stats_clip_msl());
            for (std::string line; std::getline(clip, line);) out.push_back(line);
        }
    }
    if (!l.state.is_mesh()) {
        out.emplace_back("");
        return out;
    }
    out.emplace_back("struct vitreo_ms_vertex");
    out.emplace_back("{");
    out.emplace_back("    float4 position [[position]];");
    for (const auto& f : l.fields) {
        if (f.per_primitive) continue;
        const std::string n = std::to_string(f.location);
        out.emplace_back(std::string("    ") + vec_type(f.component_type) + " locn" + n + " [[user(locn" + n +
                         ")]];");
    }
    if (!l.clip_distance.empty()) {
        out.emplace_back("    float vitreo_clip [[clip_distance]] [" + std::to_string(l.clip_distance.size()) + "];");
    }
    out.emplace_back("};");
    out.emplace_back("struct vitreo_ms_primitive");
    out.emplace_back("{");
    out.emplace_back("    bool vitreo_culled [[primitive_culled]];");
    if (l.layer != kMeshNoSlot) out.emplace_back("    uint vitreo_layer [[render_target_array_index]];");
    if (l.viewport != kMeshNoSlot) out.emplace_back("    uint vitreo_viewport [[viewport_array_index]];");
    for (const auto& f : l.fields) {
        if (!f.per_primitive) continue;
        const std::string n = std::to_string(f.location);
        out.emplace_back(std::string("    ") + vec_type(f.component_type) + " locn" + n + " [[user(locn" + n +
                         ")]];");
    }
    for (u32 reg = 0; reg < 32; ++reg) {
        if ((l.per_vertex_registers & (1u << reg)) == 0) continue;
        const MeshStageLayout::Field* f = per_vertex_field(l, reg);
        for (u32 k = 0; k < 3; ++k) {
            out.emplace_back(std::string("    ") + vec_type(f != nullptr ? f->component_type : 3) + " vitreo_pv" +
                             std::to_string(reg) + "_" + std::to_string(k) + " [[user(locn" +
                             std::to_string(per_vertex_input_location(reg, k)) + ")]];");
        }
    }
    out.emplace_back("};");
    out.emplace_back("using vitreo_ms_mesh = metal::mesh<vitreo_ms_vertex, vitreo_ms_primitive, " +
                     std::to_string(std::max(l.state.max_vertices, 1u)) + ", " +
                     std::to_string(std::max(l.state.max_primitives, 1u)) + ", metal::topology::" +
                     (l.state.topology == 1 ? "line" : "triangle") + ">;");
    out.emplace_back("");
    return out;
}

std::string mesh_stage_qualifier(const MeshStageLayout& l) {
    return l.state.is_mesh() ? "[[mesh]] void " : "[[object]] void ";
}

std::string mesh_stage_parameters(const MeshStageLayout& l, u32 payload_words) {
    std::string stats = l.statistics ? "device atomic_uint* vitreo_stats [[buffer(" +
                                           std::to_string(kMeshStatisticsBufferIndex) + ")]], "
                                     : std::string();
    if (l.vi_capture != kMeshViCaptureNone) {
        stats += "device uint* vitreo_vi [[buffer(" + std::to_string(kMeshViewCaptureBufferIndex) + ")]], ";
    }
    const std::string grid = "uint3 vitreo_tg [[threadgroup_position_in_grid]], "
                             "uint3 vitreo_tgs [[threadgroups_per_grid]], ";
    const std::string origin =
        "constant uint4& vitreo_origin [[buffer(" + std::to_string(kMeshOriginBufferIndex) + ")]], ";
    if (l.state.is_amplification()) {
        return "object_data vitreo_mesh_payload& vitreo_payload [[payload]], "
               "mesh_grid_properties vitreo_grid, " + stats + (l.device_staging ? grid + origin : std::string());
    }
    std::string s = "vitreo_ms_mesh vitreo_mesh, " + stats;
    if (payload_words != 0) s += "const object_data vitreo_mesh_payload& vitreo_payload [[payload]], ";
    if (l.device_staging) {
        s += "device uint* vitreo_ms_scratch [[buffer(" + std::to_string(kMeshScratchBufferIndex) + ")]], " + grid +
             origin;
    }
    if (l.raster_capture) {
        s += "device uint* vitreo_cap [[buffer(" + std::to_string(kMeshCaptureBufferIndex) + ")]], ";
    }
    return s;
}

std::vector<std::string> mesh_stage_payload_copy(const MeshStageLayout& l, u32 payload_words,
                                                 const std::string& copy, const std::string& tid) {
    // Without an amplification shader a declared payload is undefined: 0.
    // A replayed amplification group's base word (vi_payload_base) follows
    // the D3D payload: not part of the shader's copy.
    const u32 words = payload_words != 0 ? payload_words - (l.vi_payload_base ? 1u : 0u)
                                         : l.state.payload_words;
    return {"for (uint i = " + tid + "; i < " + u(words) + "; i += " + u(l.thread_count()) + ") " + copy +
            "[i] = " + (payload_words != 0 ? std::string("vitreo_payload.w[i]") : std::string("0u")) + ";"};
}

u32 mesh_vi_ms_record_words(const MeshStageLayout& l) {
    const u32 nv = std::max(l.state.max_vertices, 1u);
    const u32 np = std::max(l.state.max_primitives, 1u);
    return 3u + nv * std::max(l.vertex_scalars, 1u) + np * std::max(l.primitive_scalars, 1u) +
           np * l.vertices_per_primitive();
}

std::string mesh_vi_object_replay_msl(const MeshStageLayout& as, u32 payload_words, bool base) {
    MeshStageLayout l = as;
    l.statistics = false;
    l.vi_capture = kMeshViCaptureNone;
    std::string msl = "#include <metal_stdlib>\nusing namespace metal;\n";
    for (const auto& line : mesh_stage_preamble(l, payload_words + (base ? 1u : 0u))) msl += line + "\n";
    const std::string rec = u(mesh_vi_as_record_words(payload_words));
    msl += "[[object]] void " + std::string(kMeshStageEntryPoint) +
           "(object_data vitreo_mesh_payload& vitreo_payload [[payload]], mesh_grid_properties vitreo_grid, "
           "device const uint* vitreo_vi [[buffer(" +
           std::to_string(kMeshViewCaptureBufferIndex) +
           ")]], uint3 vitreo_tg [[threadgroup_position_in_grid]], uint3 vitreo_tgs [[threadgroups_per_grid]], "
           "uint vitreo_tid [[thread_index_in_threadgroup]])\n{\n";
    msl += "    // A view-instancing replay of the captured amplification group.\n";
    msl += "    const uint s = vitreo_tg.x + vitreo_tgs.x * (vitreo_tg.y + vitreo_tgs.y * vitreo_tg.z);\n";
    msl += "    device const uint* r = vitreo_vi + s * " + rec + ";\n";
    msl += "    for (uint i = vitreo_tid; i < " + u(payload_words) + "; i += " + u(l.thread_count()) +
           ") vitreo_payload.w[i] = r[" + u(kMeshViAsHeaderWords) + " + i];\n";
    msl += "    if (vitreo_tid == 0u) {\n";
    if (base) msl += "        vitreo_payload.w[" + u(payload_words) + "] = r[3];\n";
    msl += "        vitreo_grid.set_threadgroups_per_grid(uint3(r[0], r[1], r[2]));\n    }\n}\n";
    return msl;
}

std::string mesh_vi_mesh_replay_msl(const MeshStageLayout& ms) {
    MeshStageLayout l = ms;
    l.statistics = false;
    l.vi_capture = kMeshViCaptureNone;
    l.vi_payload_base = false;
    const u32 nv = std::max(l.state.max_vertices, 1u);
    const u32 np = std::max(l.state.max_primitives, 1u);
    const u32 vs = std::max(l.vertex_scalars, 1u);
    const u32 ps = std::max(l.primitive_scalars, 1u);
    std::string msl = "#include <metal_stdlib>\nusing namespace metal;\n";
    for (const auto& line : mesh_stage_preamble(l, 0)) msl += line + "\n";
    msl += "[[mesh]] void " + std::string(kMeshStageEntryPoint) +
           "(vitreo_ms_mesh vitreo_mesh, device const uint* vitreo_vi [[buffer(" +
           std::to_string(kMeshViewCaptureBufferIndex) +
           ")]], uint3 vitreo_tg [[threadgroup_position_in_grid]], "
           "uint vitreo_tid [[thread_index_in_threadgroup]])\n{\n";
    msl += "    // A view-instancing replay of a captured mesh group's outputs.\n";
    msl += "    device const uint* vitreo_rc = vitreo_vi + vitreo_tg.x * " + u(mesh_vi_ms_record_words(l)) + ";\n";
    msl += "    device const uint* vitreo_rv = vitreo_rc + 3u;\n";
    msl += "    device const uint* vitreo_rp = vitreo_rv + " + u(nv * vs) + ";\n";
    msl += "    device const uint* vitreo_ri = vitreo_rp + " + u(np * ps) + ";\n";
    for (const auto& line : mesh_stage_epilogue(l, "vitreo_rv", "vitreo_rp", "vitreo_ri", "vitreo_rc", "vitreo_tid",
                                                "as_type<float>(vitreo_rc[2])")) {
        msl += "    " + line + "\n";
    }
    msl += "}\n";
    return msl;
}

std::vector<std::string> mesh_stage_epilogue(const MeshStageLayout& l, const std::string& vertices,
                                             const std::string& primitives, const std::string& indices,
                                             const std::string& counts, const std::string& tid,
                                             const std::string& depth_offset, const std::string& linear) {
    const u32 nv = std::max(l.state.max_vertices, 1u);
    const u32 np = std::max(l.state.max_primitives, 1u);
    const u32 vs = std::max(l.vertex_scalars, 1u);
    const u32 ps = std::max(l.primitive_scalars, 1u);
    const u32 vpp = l.vertices_per_primitive();
    const std::string threads = u(l.thread_count());
    const auto vword = [&](const std::string& base, u16 s) {
        return s == kMeshNoSlot ? std::string("0u") : vertices + "[" + base + " + " + u(s) + "]";
    };
    const auto pword = [&](u16 s) {
        return s == kMeshNoSlot ? std::string("0u") : primitives + "[b + " + u(s) + "]";
    };
    const auto vec = [&](const MeshStageLayout::Field& f, const auto& word) {
        return std::string("as_type<") + vec_type(f.component_type) + ">(uint4(" + word(f.slot[0]) + ", " +
               word(f.slot[1]) + ", " + word(f.slot[2]) + ", " + word(f.slot[3]) + "))";
    };
    std::vector<std::string> out;
    out.emplace_back("{");
    out.emplace_back("    // The staged outputs, after the barrier, into the metal::mesh.");
    out.emplace_back("    const uint vitreo_nv = min(" + counts + "[0], " + u(nv) + ");");
    out.emplace_back("    const uint vitreo_np = min(" + counts + "[1], " + u(np) + ");");
    out.emplace_back("    const float vitreo_depth_offset = " + depth_offset + ";");
    if (l.vi_capture == kMeshViCaptureMesh) {
        // View instancing: this group's outputs for the views replayed
        // after it (mesh_vi_mesh_replay_msl).
        const std::string slot =
            (l.vi_payload_base ? "vitreo_payload.w[" + u(l.vi_base_word) + "] + " : std::string()) + linear;
        out.emplace_back("    {");
        out.emplace_back("        device uint* vitreo_vr = vitreo_vi + (" + slot + ") * " +
                         u(mesh_vi_ms_record_words(l)) + ";");
        out.emplace_back("        if (" + tid + " == 0u) { vitreo_vr[0] = vitreo_nv; vitreo_vr[1] = vitreo_np; "
                         "vitreo_vr[2] = as_type<uint>(vitreo_depth_offset); }");
        out.emplace_back("        for (uint k = " + tid + "; k < vitreo_nv * " + u(vs) + "; k += " + threads +
                         ") vitreo_vr[3u + k] = " + vertices + "[k];");
        out.emplace_back("        for (uint k = " + tid + "; k < vitreo_np * " + u(ps) + "; k += " + threads +
                         ") vitreo_vr[" + u(3 + nv * vs) + " + k] = " + primitives + "[k];");
        out.emplace_back("        for (uint k = " + tid + "; k < vitreo_np * " + u(vpp) + "; k += " + threads +
                         ") vitreo_vr[" + u(3 + nv * vs + np * ps) + " + k] = uint(" + indices + "[k]);");
        out.emplace_back("    }");
    }
    // Whether the primitive at primitive word `b` / index word `i` is culled
    // (SV_CullPrimitive, or SV_CullDistance: D3D11.3 15.4.2, every vertex
    // below zero for some distance).
    const bool culls = l.cull_primitive_slot != kMeshNoSlot || !l.cull_distance.empty();
    const auto culled_of = [&](const std::string& b, const std::string& i) {
        std::string e = l.cull_primitive_slot == kMeshNoSlot
                            ? std::string("false")
                            : "(" + primitives + "[" + b + " + " + u(l.cull_primitive_slot) + "] != 0u)";
        for (u16 s : l.cull_distance) {
            std::string all;
            for (u32 k = 0; k < vpp; ++k) {
                if (k != 0) all += " && ";
                all += "as_type<float>(" + vertices + "[min(" + indices + "[" + i + " + " + u(k) + "], " +
                       u(nv - 1) + ") * " + u(vs) + " + " + u(s) + "]) < 0.0";
            }
            e += " || (" + all + ")";
        }
        return e;
    };
    if (l.raster_capture) {
        // Raster capture: nothing is drawn here. One thread counts the
        // group's surviving primitives and reserves their records with one
        // atomic add; the base goes to the others through the counts word,
        // read above (both barriers are in uniform control flow).
        out.emplace_back("    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
        out.emplace_back("    if (" + tid + " == 0u)");
        out.emplace_back("    {");
        out.emplace_back("        vitreo_mesh.set_primitive_count(0u);");
        if (culls) {
            out.emplace_back("        uint n = 0u;");
            out.emplace_back("        for (uint p = 0u; p < vitreo_np; ++p) { const uint b = p * " + u(ps) +
                             "; const uint i = p * " + u(vpp) + "; n += (" + culled_of("b", "i") +
                             ") ? 0u : 1u; }");
        } else {
            out.emplace_back("        const uint n = vitreo_np;");
        }
        out.emplace_back("        " + counts +
                         "[0] = atomic_fetch_add_explicit((device atomic_uint*)vitreo_cap, n, "
                         "memory_order_relaxed);");
        out.emplace_back("    }");
        out.emplace_back("    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
        out.emplace_back("    const uint vitreo_cap_base = " + counts + "[0];");
        out.emplace_back("    const uint vitreo_cap_capacity = vitreo_cap[1];");
        out.emplace_back("    device uint* vitreo_cap_records = vitreo_cap + " + u(kMeshCaptureHeaderWords) + ";");
    } else {
        out.emplace_back("    if (" + tid + " == 0u) vitreo_mesh.set_primitive_count(vitreo_np);");
    }
    if (l.statistics) {
        // MSInvocations count threads ("the number of mesh shaders
        // invoked", measured: 64 for 16 groups of 4 threads).
        out.emplace_back("    if (" + tid + " == 0u) { vitreo_stat_add(vitreo_stats, " + u(kMeshStatMs) + ", " +
                         threads + "); vitreo_stat_add(vitreo_stats, " + u(kMeshStatPrimitives) +
                         ", vitreo_np); }");
    }
    out.emplace_back("    for (uint j = " + tid + "; j < (" + (l.raster_capture ? "0u" : "vitreo_nv") + "); j += " +
                     threads + ")");
    out.emplace_back("    {");
    out.emplace_back("        vitreo_ms_vertex v = {};");
    out.emplace_back("        const uint b = j * " + u(vs) + ";");
    out.emplace_back("        v.position = as_type<float4>(uint4(" + vword("b", l.position[0]) + ", " +
                     vword("b", l.position[1]) + ", " + vword("b", l.position[2]) + ", " +
                     vword("b", l.position[3]) + "));");
    out.emplace_back("        if (vitreo_depth_offset != 0.0) v.position.z += vitreo_depth_offset * v.position.w;");
    for (const auto& f : l.fields) {
        if (f.per_primitive) continue;
        out.emplace_back("        v.locn" + std::to_string(f.location) + " = " +
                         vec(f, [&](u16 s) { return vword("b", s); }) + ";");
    }
    for (usize k = 0; k < l.clip_distance.size(); ++k) {
        out.emplace_back("        v.vitreo_clip[" + std::to_string(k) + "] = as_type<float>(" +
                         vword("b", l.clip_distance[k]) + ");");
    }
    out.emplace_back("        vitreo_mesh.set_vertex(j, v);");
    out.emplace_back("    }");
    out.emplace_back("    for (uint p = " + tid + "; p < vitreo_np; p += " + threads + ")");
    out.emplace_back("    {");
    out.emplace_back("        vitreo_ms_primitive q = {};");
    out.emplace_back("        const uint b = p * " + u(ps) + ";");
    out.emplace_back("        const uint i = p * " + u(vpp) + ";");
    out.emplace_back("        bool culled = " +
                     (l.cull_primitive_slot == kMeshNoSlot ? std::string("false")
                                                           : pword(l.cull_primitive_slot) + " != 0u") +
                     ";");
    // SV_CullDistance (D3D11.3 15.4.2): the primitive is culled when, for
    // some distance, every one of its vertices has it below zero.
    for (u16 s : l.cull_distance) {
        std::string all;
        for (u32 k = 0; k < vpp; ++k) {
            if (k != 0) all += " && ";
            all += "as_type<float>(" + vertices + "[min(" + indices + "[i + " + u(k) + "], " + u(nv - 1) + ") * " +
                   u(vs) + " + " + u(s) + "]) < 0.0";
        }
        out.emplace_back("        culled = culled || (" + all + ");");
    }
    out.emplace_back("        q.vitreo_culled = culled;");
    if (l.statistics) {
        // CInvocations: every primitive the clipper sees (not culled);
        // CPrimitives: what it makes of it (graphics::stats_clip_primitives).
        out.emplace_back("        if (!culled)");
        out.emplace_back("        {");
        out.emplace_back("            vitreo_stat_add(vitreo_stats, " + u(kMeshStatClipper) + ", 1u);");
        out.emplace_back("            ClipV cv[3];");
        for (u32 k = 0; k < vpp; ++k) {
            const std::string vb = "min(" + indices + "[i + " + u(k) + "], " + u(nv - 1) + ") * " + u(vs);
            out.emplace_back("            {");
            out.emplace_back("                const uint vb = " + vb + ";");
            out.emplace_back("                cv[" + u(k) + "].p = as_type<float4>(uint4(" + vword("vb", l.position[0]) +
                             ", " + vword("vb", l.position[1]) + ", " + vword("vb", l.position[2]) + ", " +
                             vword("vb", l.position[3]) + "));");
            out.emplace_back("                for (uint d = 0u; d < 8u; ++d) cv[" + u(k) + "].d[d] = 0.0f;");
            for (usize d = 0; d < l.clip_distance.size() && d < 8; ++d) {
                out.emplace_back("                cv[" + u(k) + "].d[" + u(static_cast<u32>(d)) +
                                 "] = as_type<float>(" + vword("vb", l.clip_distance[d]) + ");");
            }
            out.emplace_back("            }");
        }
        out.emplace_back("            vitreo_stat_add(vitreo_stats, " + u(kMeshStatClipped) + ", stats_clip(cv, " +
                         u(vpp) + ", " + u(static_cast<u32>(std::min<usize>(l.clip_distance.size(), 8))) + ", " +
                         (l.depth_clip ? "true" : "false") + "));");
        out.emplace_back("        }");
    }
    if (l.raster_capture) {
        // The primitive's rank among the group's survivors places its three
        // records (pass C reads record 3 g + k of primitive g).
        const u32 stride = static_cast<u32>(l.capture.size());
        out.emplace_back("        if (culled) continue;");
        if (culls) {
            out.emplace_back("        uint rank = 0u;");
            out.emplace_back("        for (uint t = 0u; t < p; ++t) { const uint tb = t * " + u(ps) +
                             "; const uint ti = t * " + u(vpp) + "; rank += (" + culled_of("tb", "ti") +
                             ") ? 0u : 1u; }");
        } else {
            out.emplace_back("        const uint rank = p;");
        }
        out.emplace_back("        const uint g = vitreo_cap_base + rank;");
        out.emplace_back("        if (g >= vitreo_cap_capacity) continue;");
        out.emplace_back("        for (uint k = 0u; k < 3u; ++k)");
        out.emplace_back("        {");
        out.emplace_back("            const uint vb = min(" + indices + "[i + k], " + u(nv - 1) + ") * " + u(vs) + ";");
        out.emplace_back("            device uint* o = vitreo_cap_records + (g * 3u + k) * " + u(stride * 4u) + ";");
        for (u32 r = 0; r < stride; ++r) {
            const auto& rec = l.capture[r];
            const auto word = [&](u16 s) { return rec.per_primitive ? pword(s) : vword("vb", s); };
            const std::string words =
                "uint4(" + word(rec.slot[0]) + ", " + word(rec.slot[1]) + ", " + word(rec.slot[2]) + ", " +
                word(rec.slot[3]) + ")";
            const std::string at = "*(device uint4*)(o + " + u(r * 4u) + ")";
            if (rec.position) {
                out.emplace_back("            {");
                out.emplace_back("                float4 P = as_type<float4>(" + words + ");");
                out.emplace_back("                if (vitreo_depth_offset != 0.0) P.z += vitreo_depth_offset * P.w;");
                out.emplace_back("                " + at + " = as_type<uint4>(P);");
                out.emplace_back("            }");
            } else {
                out.emplace_back("            " + at + " = " + words + ";");
            }
        }
        out.emplace_back("        }");
        out.emplace_back("    }");
        out.emplace_back("}");
        return out;
    }
    if (l.layer != kMeshNoSlot) out.emplace_back("        q.vitreo_layer = " + pword(l.layer) + ";");
    if (l.viewport != kMeshNoSlot) out.emplace_back("        q.vitreo_viewport = " + pword(l.viewport) + ";");
    for (const auto& f : l.fields) {
        if (!f.per_primitive) continue;
        out.emplace_back("        q.locn" + std::to_string(f.location) + " = " + vec(f, pword) + ";");
    }
    // GetAttributeAtVertex: vertex k of the primitive is the one its k-th
    // index names.
    for (u32 reg = 0; reg < 32; ++reg) {
        if ((l.per_vertex_registers & (1u << reg)) == 0) continue;
        const MeshStageLayout::Field* f = per_vertex_field(l, reg);
        for (u32 k = 0; k < 3; ++k) {
            const std::string name = "q.vitreo_pv" + std::to_string(reg) + "_" + std::to_string(k);
            if (f == nullptr || k >= vpp) {
                out.emplace_back("        " + name + " = " + vec_type(f != nullptr ? f->component_type : 3) + "(0);");
                continue;
            }
            if (f->per_primitive) {
                out.emplace_back("        " + name + " = " + vec(*f, pword) + ";");
                continue;
            }
            const std::string base = "min(uint(" + indices + "[i + " + u(k) + "]), " + u(nv - 1) + ") * " + u(vs);
            out.emplace_back("        " + name + " = " + vec(*f, [&](u16 s) { return vword(base, s); }) + ";");
        }
    }
    for (u32 k = 0; k < vpp; ++k) {
        out.emplace_back("        vitreo_mesh.set_index(i + " + u(k) + ", uchar(" + indices + "[i + " + u(k) + "]));");
    }
    out.emplace_back("        vitreo_mesh.set_primitive(p, q);");
    out.emplace_back("    }");
    out.emplace_back("}");
    return out;
}

namespace {

/// One scalar of the groupshared payload object within a payload word.
struct PayloadPiece {
    u32 scalar;
    u32 kind;  ///< kDxbcPayload*
    u32 byte;  ///< within the word
    bool operator==(const PayloadPiece&) const = default;
};

/// The payload's byte image from the groupshared object @p payload
/// (DxbcMeshState::payload_layout): each word ORs the scalars that lie in
/// it; runs of words of one shape whose scalars advance by one stride are
/// written by a loop over the threads.
void pack_payload(std::vector<std::string>& out, const std::vector<u32>& layout, u32 words,
                  const std::string& payload, const std::string& tid, u32 threads) {
    std::vector<std::vector<PayloadPiece>> word(words);
    for (u32 s = 0; s < layout.size(); ++s) {
        const u32 byte = layout[s] & 0xFFFFu;
        if (byte / 4u < words) word[byte / 4u].push_back({s, layout[s] >> 16, byte & 3u});
    }
    const auto shape = [&](u32 w) {
        std::vector<PayloadPiece> rel = word[w];
        for (auto& p : rel) p.scalar -= word[w].front().scalar;
        return rel;
    };
    const auto piece = [&](const PayloadPiece& p, const std::string& base) {
        const std::string x = payload + "[" + base + " + " + u(p.scalar) + "]";
        const std::string at = p.byte != 0 ? " << " + u(p.byte * 8u) : std::string();
        if (p.kind == kDxbcPayloadU16) return "((" + x + " & 0xFFFFu)" + at + ")";
        if (p.kind == kDxbcPayloadF16) return "(uint(as_type<ushort>(half(as_type<float>(" + x + "))))" + at + ")";
        return "(" + x + at + ")";
    };
    for (u32 w = 0; w < words;) {
        if (word[w].empty()) {
            u32 n = 1;
            while (w + n < words && word[w + n].empty()) ++n;
            out.emplace_back("    for (uint i = " + tid + "; i < " + u(n) + "; i += " + u(threads) +
                             ") vitreo_payload.w[" + u(w) + " + i] = 0u;");
            w += n;
            continue;
        }
        const auto first = shape(w);
        u32 n = 1, stride = 0;
        if (w + 1 < words && !word[w + 1].empty() && shape(w + 1) == first) {
            stride = word[w + 1].front().scalar - word[w].front().scalar;
            while (w + n < words && !word[w + n].empty() && shape(w + n) == first &&
                   word[w + n].front().scalar == word[w].front().scalar + n * stride) {
                ++n;
            }
        }
        std::string value;
        for (const auto& p : first) value += (value.empty() ? "" : " | ") + piece(p, "s");
        out.emplace_back("    for (uint i = " + tid + "; i < " + u(n) + "; i += " + u(threads) + ") { const uint s = " +
                         u(word[w].front().scalar) + " + i * " + u(stride) + "; vitreo_payload.w[" + u(w) +
                         " + i] = " + value + "; }");
        w += n;
    }
}

}  // namespace

std::vector<std::string> object_stage_epilogue(const MeshStageLayout& l, u32 payload_words,
                                               const std::string& payload, const std::string& grid,
                                               const std::string& tid, const std::string& linear) {
    std::vector<std::string> out;
    if (l.vi_capture == kMeshViCaptureAmplification) {
        // View instancing: record the grid and payload, launch no mesh
        // group (each view replays the record, mesh_vi_object_replay_msl).
        out.emplace_back("{");
        out.emplace_back("    device uint* vitreo_vr = vitreo_vi + (" + linear + ") * " +
                         u(mesh_vi_as_record_words(payload_words)) + ";");
        out.emplace_back("    for (uint i = " + tid + "; i < " + u(payload_words) + "; i += " +
                         u(l.thread_count()) + ") vitreo_vr[" + u(kMeshViAsHeaderWords) + " + i] = " + payload +
                         "[i];");
        out.emplace_back("    if (" + tid + " == 0u) { vitreo_vr[0] = " + grid + "[0]; vitreo_vr[1] = " + grid +
                         "[1]; vitreo_vr[2] = " + grid + "[2]; vitreo_grid.set_threadgroups_per_grid(uint3(0u)); }");
        if (l.statistics) {
            out.emplace_back("    if (" + tid + " == 0u) vitreo_stat_add(vitreo_stats, " + u(kMeshStatAs) + ", " +
                             u(l.thread_count()) + ");");
        }
        out.emplace_back("}");
        return out;
    }
    out.emplace_back("{");
    out.emplace_back("    // DispatchMesh: the payload and the mesh grid, after the barrier.");
    if (l.state.payload_layout.empty()) {
        out.emplace_back("    for (uint i = " + tid + "; i < " + u(payload_words) + "; i += " + u(l.thread_count()) +
                         ") vitreo_payload.w[i] = " + payload + "[i];");
    } else {
        pack_payload(out, l.state.payload_layout, payload_words, payload, tid, l.thread_count());
    }
    out.emplace_back("    if (" + tid + " == 0u) vitreo_grid.set_threadgroups_per_grid(uint3(" + grid + "[0], " + grid +
                     "[1], " + grid + "[2]));");
    if (l.statistics) {
        out.emplace_back("    if (" + tid + " == 0u) vitreo_stat_add(vitreo_stats, " + u(kMeshStatAs) + ", " +
                         u(l.thread_count()) + ");");
    }
    out.emplace_back("}");
    return out;
}

}  // namespace vitreo::graphics
