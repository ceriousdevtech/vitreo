// SPDX-License-Identifier: LGPL-2.1-only
#include "ff_emulation.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace vitreo::graphics {

// =============================================================================
// State hashing — FNV-1a over the meaningful state fields
// =============================================================================

static void fnv1a_update(u64& h, const void* data, usize len) {
    constexpr u64 prime = 1099511628211ULL;
    auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i < len; ++i) {
        h ^= p[i];
        h *= prime;
    }
}

template <typename T>
static void fnv1a_pod(u64& h, const T& value) {
    fnv1a_update(h, &value, sizeof(value));
}

u64 FFPipelineState::hash() const {
    // Key only the fields that change generated MSL. Light positions,
    // material colors, fog distances, tween factor, and alpha ref are
    // SetLight / SetMaterial / SetRenderState constant-buffer payload —
    // hashing them compiled a new Metal pipeline on every camera move.
    u64 h = 14695981039346656037ULL;
    fnv1a_pod(h, fvf_flags);
    const u8 flags[] = {
        static_cast<u8>(flat_shading),
        static_cast<u8>(point_size_output),
        static_cast<u8>(point_scale_enabled),
        static_cast<u8>(clip_planes_enabled),
        static_cast<u8>(indexed_vertex_blend),
        static_cast<u8>(vertex_tweening),
        static_cast<u8>(lighting_enabled),
        static_cast<u8>(normalize_normals),
        static_cast<u8>(local_viewer),
        static_cast<u8>(specular_enable),
        static_cast<u8>(vertex_color_enable),
        static_cast<u8>(table_fog),
        static_cast<u8>(table_fog_w),
        static_cast<u8>(range_fog_enable),
        static_cast<u8>(alpha_test_enable),
        static_cast<u8>(fog_mode),
    };
    fnv1a_update(h, flags, sizeof(flags));
    fnv1a_pod(h, vertex_blend_weight_count);
    fnv1a_pod(h, active_light_count);
    const u32 light_count =
        std::min(active_light_count, static_cast<u32>(lights.size()));
    for (u32 i = 0; i < light_count; ++i) {
        fnv1a_pod(h, lights[i].type);
    }
    fnv1a_pod(h, diffuse_material_source);
    fnv1a_pod(h, ambient_material_source);
    fnv1a_pod(h, specular_material_source);
    fnv1a_pod(h, emissive_material_source);
    fnv1a_pod(h, active_texture_stages);
    const u32 stage_count = std::min(
        active_texture_stages, static_cast<u32>(texture_stages.size()));
    for (u32 i = 0; i < stage_count; ++i) {
        const TextureStageState& stage = texture_stages[i];
        fnv1a_pod(h, stage.color_op);
        fnv1a_pod(h, stage.color_arg0);
        fnv1a_pod(h, stage.color_arg1);
        fnv1a_pod(h, stage.color_arg2);
        fnv1a_pod(h, stage.alpha_op);
        fnv1a_pod(h, stage.alpha_arg0);
        fnv1a_pod(h, stage.alpha_arg1);
        fnv1a_pod(h, stage.alpha_arg2);
        fnv1a_pod(h, stage.result_arg);
        fnv1a_pod(h, stage.texcoord_index);
        fnv1a_pod(h, stage.texture_transform_flags);
        fnv1a_pod(h, stage.bump_env_mat00);
        fnv1a_pod(h, stage.bump_env_mat01);
        fnv1a_pod(h, stage.bump_env_mat10);
        fnv1a_pod(h, stage.bump_env_mat11);
        fnv1a_pod(h, stage.bump_env_lscale);
        fnv1a_pod(h, stage.bump_env_loffset);
    }
    fnv1a_pod(h, alpha_test_func);
    fnv1a_pod(h, srgb_texture_mask);
    fnv1a_pod(h, static_cast<u8>(srgb_write_enable));
    // The OpenGL fields enter the hash only when one of them differs from its
    // Direct3D default, so every Direct3D state keeps its hash (goldens).
    if (ff_uses_gl_fields(*this)) {
        const u8 gl_flags[] = {
            static_cast<u8>('G'),
            static_cast<u8>(position_is_homogeneous),
            static_cast<u8>(gl_spot_model),
            static_cast<u8>(two_sided_lighting),
            static_cast<u8>(line_stipple.enabled),
            static_cast<u8>(polygon_stipple),
            static_cast<u8>(logic_op),
            static_cast<u8>(window_offset),
            static_cast<u8>(depth_offset),
            static_cast<u8>(color_targets),
            static_cast<u8>(gl_texture_sampling),
        };
        fnv1a_update(h, gl_flags, sizeof(gl_flags));
        for (const auto& unit : texgen_plane_mode) {
            for (FFTexGenMode mode : unit) fnv1a_pod(h, mode);
        }
        if (two_sided_lighting) {
            fnv1a_pod(h, back_diffuse_material_source);
            fnv1a_pod(h, back_ambient_material_source);
            fnv1a_pod(h, back_specular_material_source);
            fnv1a_pod(h, back_emissive_material_source);
        }
    }
    return h;
}

bool ff_uses_gl_fields(const FFPipelineState& s) {
    if (s.position_is_homogeneous || s.gl_spot_model || s.two_sided_lighting || s.line_stipple.enabled ||
        s.polygon_stipple || s.logic_op != FFLogicOp::Copy || s.window_offset || s.depth_offset ||
        s.color_targets != 1 || s.gl_texture_sampling) {
        return true;
    }
    for (const auto& unit : s.texgen_plane_mode) {
        for (FFTexGenMode mode : unit) {
            if (mode != FFTexGenMode::None) return true;
        }
    }
    return false;
}

bool ff_texgen_needs_planes(const FFPipelineState& s) {
    for (const auto& unit : s.texgen_plane_mode) {
        for (FFTexGenMode mode : unit) {
            if (mode == FFTexGenMode::ObjectLinear || mode == FFTexGenMode::EyeLinear) return true;
        }
    }
    return false;
}

bool ff_lighting_uniforms(const FFPipelineState& s) {
    return s.lighting_enabled && (s.active_light_count > 0 || s.gl_spot_model);
}

/// The preconditions the OpenGL fields document (ff_emulation.hpp).
static bool ff_gl_fields_valid(const FFPipelineState& s) {
    const bool pretransformed = (s.fvf_flags & fvf::POSITION_MASK) == fvf::XYZRHW;
    if (s.position_is_homogeneous) {
        if (pretransformed || s.vertex_tweening || s.vertex_blend_weight_count != 0 || s.indexed_vertex_blend) {
            return false;
        }
        for (u32 i = 0; i < std::min(s.active_texture_stages, 8u); ++i) {
            if ((s.texture_stages[i].texcoord_index & 0xFFFF0000u) != 0) return false;
        }
    }
    if (s.two_sided_lighting && !s.gl_spot_model) return false;
    if ((s.window_offset || s.depth_offset || s.gl_texture_sampling) && !s.position_is_homogeneous) return false;
    if (s.color_targets < 1 || s.color_targets > 8) return false;
    for (u32 unit = 0; unit < s.texgen_plane_mode.size(); ++unit) {
        for (u32 coord = 0; coord < 4; ++coord) {
            const FFTexGenMode mode = s.texgen_plane_mode[unit][coord];
            if (mode == FFTexGenMode::None) continue;
            if (pretransformed || unit >= s.texcoord_count()) return false;
            if (mode == FFTexGenMode::SphereMap && (coord >= 2 || (s.fvf_flags & fvf::NORMAL) == 0)) return false;
        }
    }
    return true;
}

// =============================================================================
// MSL code generation helpers
// =============================================================================

static std::string arg_source_name(u32 arg, u32 stage) {
    switch (arg & 0xFu) {
        case 0:  return "v_diffuse";   // D3DTA_DIFFUSE
        case 1:  return "current";     // D3DTA_CURRENT
        case 2:  return "tex_sample";  // D3DTA_TEXTURE
        case 3:  return "cb.texture_factor"; // D3DTA_TFACTOR
        case 4:  return "v_specular";  // D3DTA_SPECULAR
        case 5:  return "temp";        // D3DTA_TEMP
        case 6:  return "cb.stage_constants[" + std::to_string(stage) + "]";
        default: return "current";
    }
}

/// Apply D3DTA_ALPHAREPLICATE then D3DTA_COMPLEMENT (MSDN order).
static std::string modified_arg_expr(u32 arg, u32 stage,
                                     bool premodulate_current = false) {
    std::string expr = premodulate_current &&
                               (arg & 0xFu) ==
                                   static_cast<u32>(TexStageArg::Current)
                           ? "(current * tex_sample)"
                           : arg_source_name(arg, stage);
    if ((arg & 0x20u) != 0) {  // D3DTA_ALPHAREPLICATE
        expr = "float4(" + expr + ".aaaa)";
    }
    if ((arg & 0x10u) != 0) {  // D3DTA_COMPLEMENT
        expr = "(float4(1.0) - (" + expr + "))";
    }
    return expr;
}

static void emit_combiner_op(std::ostringstream& ss,
                              const char* result,
                              const char* component,   // ".rgb" or ".a"
                              TexStageOp op,
                              const char* arg0,
                              const char* arg1,
                              const char* arg2) {
    switch (op) {
        case TexStageOp::Disable:
            break;
        case TexStageOp::SelectArg1:
            ss << "    " << result << component << " = " << arg1 << component << ";\n";
            break;
        case TexStageOp::SelectArg2:
            ss << "    " << result << component << " = " << arg2 << component << ";\n";
            break;
        case TexStageOp::Modulate:
            ss << "    " << result << component << " = " << arg1 << component
               << " * " << arg2 << component << ";\n";
            break;
        case TexStageOp::Modulate2x:
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " * " << arg2 << component << " * 2.0, 0.0, 1.0);\n";
            break;
        case TexStageOp::Modulate4x:
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " * " << arg2 << component << " * 4.0, 0.0, 1.0);\n";
            break;
        case TexStageOp::Add:
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " + " << arg2 << component << ", 0.0, 1.0);\n";
            break;
        case TexStageOp::AddSigned:
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " + " << arg2 << component << " - 0.5, 0.0, 1.0);\n";
            break;
        case TexStageOp::AddSigned2x:
            ss << "    " << result << component << " = clamp((" << arg1 << component
               << " + " << arg2 << component << " - 0.5) * 2.0, 0.0, 1.0);\n";
            break;
        case TexStageOp::Subtract:
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " - " << arg2 << component << ", 0.0, 1.0);\n";
            break;
        case TexStageOp::AddSmooth:
            // Arg1 + Arg2 - Arg1*Arg2
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " + " << arg2 << component << " - " << arg1 << component
               << " * " << arg2 << component << ", 0.0, 1.0);\n";
            break;
        case TexStageOp::BlendDiffuseAlpha:
            ss << "    " << result << component << " = mix(" << arg2 << component
               << ", " << arg1 << component << ", v_diffuse.a);\n";
            break;
        case TexStageOp::BlendTextureAlpha:
            ss << "    " << result << component << " = mix(" << arg2 << component
               << ", " << arg1 << component << ", tex_sample.a);\n";
            break;
        case TexStageOp::BlendFactorAlpha:
            ss << "    " << result << component << " = mix(" << arg2 << component
               << ", " << arg1 << component << ", cb.texture_factor.a);\n";
            break;
        case TexStageOp::BlendTextureAlphaPM:
            // Arg1 is already α-premultiplied: Arg1 + Arg2*(1 - Texture.a)
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " + " << arg2 << component << " * (1.0 - tex_sample.a), 0.0, 1.0);\n";
            break;
        case TexStageOp::BlendCurrentAlpha:
            ss << "    " << result << component << " = mix(" << arg2 << component
               << ", " << arg1 << component << ", current.a);\n";
            break;
        case TexStageOp::Premodulate:
            // Stage n outputs Arg1; next-stage CURRENT substitution is handled
            // by the caller after that stage's texture has been sampled.
            ss << "    " << result << component << " = " << arg1 << component << ";\n";
            break;
        case TexStageOp::BumpEnvMap:
        case TexStageOp::BumpEnvMapLuminance:
            // This stage only computes the coordinate delta consumed by the
            // following stage; it does not replace CURRENT.
            break;
        case TexStageOp::ModulateAlphaAddColor:
            if (component[1] == 'r') {
                ss << "    " << result << ".rgb = clamp(" << arg1 << ".rgb + "
                   << arg1 << ".a * " << arg2 << ".rgb, 0.0, 1.0);\n";
            } else {
                ss << "    " << result << ".a = clamp(" << arg1 << ".a * " << arg2
                   << ".a, 0.0, 1.0);\n";
            }
            break;
        case TexStageOp::ModulateColorAddAlpha:
            if (component[1] == 'r') {
                ss << "    " << result << ".rgb = clamp(" << arg1 << ".rgb * "
                   << arg2 << ".rgb + " << arg1 << ".a, 0.0, 1.0);\n";
            } else {
                ss << "    " << result << ".a = " << arg1 << ".a;\n";
            }
            break;
        case TexStageOp::ModulateInvAlphaAddColor:
            if (component[1] == 'r') {
                ss << "    " << result << ".rgb = clamp(" << arg1 << ".rgb + (1.0 - "
                   << arg1 << ".a) * " << arg2 << ".rgb, 0.0, 1.0);\n";
            } else {
                ss << "    " << result << ".a = clamp((1.0 - " << arg1 << ".a) * "
                   << arg2 << ".a, 0.0, 1.0);\n";
            }
            break;
        case TexStageOp::ModulateInvColorAddAlpha:
            if (component[1] == 'r') {
                ss << "    " << result << ".rgb = clamp((1.0 - " << arg1 << ".rgb) * "
                   << arg2 << ".rgb + " << arg1 << ".a, 0.0, 1.0);\n";
            } else {
                ss << "    " << result << ".a = " << arg1 << ".a;\n";
            }
            break;
        case TexStageOp::DotProduct3:
            if (component[1] == 'r') { // only emit once for .rgb
                ss << "    { float d = clamp(dot(" << arg1 << ".rgb * 2.0 - 1.0, "
                   << arg2 << ".rgb * 2.0 - 1.0), 0.0, 1.0);\n"
                   << "      " << result << ".rgb = float3(d); " << result
                   << ".a = d; }\n";
            }
            break;
        case TexStageOp::MultiplyAdd:
            // Result = Arg1 * Arg2 + Arg0
            ss << "    " << result << component << " = clamp(" << arg1 << component
               << " * " << arg2 << component << " + " << arg0 << component
               << ", 0.0, 1.0);\n";
            break;
        case TexStageOp::Lerp:
            // Result = Arg0 * Arg1 + (1 - Arg0) * Arg2
            ss << "    " << result << component << " = mix(" << arg2 << component
               << ", " << arg1 << component << ", " << arg0 << component << ");\n";
            break;
    }
}

// =============================================================================
// OpenGL fixed-function extensions (ff_emulation.hpp). Each emitter writes
// nothing for a state that leaves its field at the Direct3D default.
// =============================================================================

/// Varyings shared by FFVertexOut and FFFragIn.
static void emit_gl_varyings(std::ostringstream& ss, const FFPipelineState& state) {
    if (state.two_sided_lighting) {
        const char* flat = state.flat_shading ? " [[flat]]" : "";
        ss << "    float4 back_diffuse" << flat << ";\n"
           << "    float4 back_specular" << flat << ";\n";
    }
    if (state.line_stipple.enabled) {
        // The stipple counter runs along the line in window space.
        ss << "    float line_distance [[center_no_perspective]];\n";
    }
    if (state.depth_offset) {
        // Constant per primitive (polygon offset, spec 3.5.5).
        ss << "    float2 depth_offset [[flat]];\n";
    }
}

/// GL 1.1 spec 2.13.1, one light's contribution:
///   att * spot * (a_cm a_cl + (n (.) VP) d_cm d_cl + f (n (.) h)^s s_cm s_cl),
/// where d1 (.) d2 = max(d1 . d2, 0) and f = 1 when n (.) VP != 0, i.e. when
/// n . VP > 0. The spot cone compares PV (.) s (the clamped dot) with
/// cos(cutoff) (eq. 2.5), so cutoff 90 with exponent 0 is 1 behind the light.
/// P = eye-space position (w = 0: directional), D = spot direction + spot
/// exponent, A = k0, k1, k2 + cos(cutoff) (< -1: cutoff 180, no spot).
static void emit_gl_light_function(std::ostringstream& ss) {
    ss << "static inline float gl_power(float x, float e) {\n"
       << "    return e == 0.0 ? 1.0 : (x > 0.0 ? pow(x, e) : 0.0);\n"
       << "}\n"
       << "static inline float3 gl_light_term(float3 n, float3 V, float3 eye_dir,\n"
       << "                                   float4 P, float4 D, float4 A,\n"
       << "                                   float3 la, float3 ld, float3 ls,\n"
       << "                                   float3 ma, float3 md, float3 ms, float shininess) {\n"
       << "    float3 VP;\n"
       << "    float att = 1.0;\n"
       << "    if (P.w != 0.0) {\n"
       << "        float3 to_light = P.xyz / P.w - V;\n"
       << "        float d = length(to_light);\n"
       << "        VP = to_light / max(d, 1e-30);\n"
       << "        att = 1.0 / (A.x + A.y * d + A.z * d * d);\n"
       << "    } else {\n"
       << "        VP = normalize(P.xyz);\n"
       << "    }\n"
       << "    float spot = 1.0;\n"
       << "    if (A.w >= -1.0) {\n"
       << "        float sd = dot(-VP, normalize(D.xyz));\n"
       << "        spot = max(sd, 0.0) >= A.w ? gl_power(max(sd, 0.0), D.w) : 0.0;\n"
       << "    }\n"
       << "    float n_vp = dot(n, VP);\n"
       << "    float3 h = normalize(VP + eye_dir);\n"
       << "    float f = n_vp > 0.0 ? 1.0 : 0.0;\n"
       << "    float spec = f * gl_power(max(dot(n, h), 0.0), shininess);\n"
       << "    return att * spot * (ma * la + max(n_vp, 0.0) * md * ld + spec * ms * ls);\n"
       << "}\n\n";
}

/// The GL lighting body, after world_pos / world_normal (eye space for GL)
/// and the front lit_* sources are declared.
template <typename SourceExpr>
static void emit_gl_lighting(std::ostringstream& ss, const FFPipelineState& state, const SourceExpr& source_expr) {
    ss << "    float3 gl_eye_dir = "
       << (state.local_viewer ? "normalize(-world_pos);\n" : "float3(0.0, 0.0, 1.0);\n");
    const auto side = [&](const char* out_name, const char* normal, const char* diffuse, const char* ambient,
                          const char* specular, const char* emissive, const char* power) {
        ss << "    {\n"
           << "        float3 gl_color = " << emissive << ".rgb + cb.global_ambient.rgb * " << ambient << ".rgb;\n";
        for (u32 i = 0; i < state.active_light_count; ++i) {
            ss << "        gl_color += gl_light_term(" << normal << ", world_pos, gl_eye_dir, cb.light" << i
               << "_position, cb.light" << i << "_direction, cb.light" << i << "_atten, cb.light" << i
               << "_ambient.rgb, cb.light" << i << "_diffuse.rgb, cb.light" << i << "_specular.rgb, " << ambient
               << ".rgb, " << diffuse << ".rgb, " << specular << ".rgb, " << power << ");\n";
        }
        ss << "        out." << out_name << " = float4(clamp(gl_color, 0.0, 1.0), " << diffuse << ".a);\n"
           << "    }\n";
    };
    side("diffuse", "world_normal", "lit_diffuse", "lit_ambient", "lit_specular", "lit_emissive", "cb.mat_power");
    ss << "    out.specular = float4(0.0);\n";
    if (state.two_sided_lighting) {
        ss << "    float4 back_lit_diffuse = "
           << source_expr("back_mat_diffuse", state.back_diffuse_material_source) << ";\n"
           << "    float4 back_lit_ambient = "
           << source_expr("back_mat_ambient", state.back_ambient_material_source) << ";\n"
           << "    float4 back_lit_specular = "
           << source_expr("back_mat_specular", state.back_specular_material_source) << ";\n"
           << "    float4 back_lit_emissive = "
           << source_expr("back_mat_emissive", state.back_emissive_material_source) << ";\n";
        side("back_diffuse", "-world_normal", "back_lit_diffuse", "back_lit_ambient", "back_lit_specular",
             "back_lit_emissive", "cb.back_mat_power.x");
        ss << "    out.back_specular = float4(0.0);\n";
    }
}

/// glTexGen (spec 2.10.4) into out.texcoord<unit>, row-vector form.
static void emit_gl_texgen(std::ostringstream& ss, const FFPipelineState& state) {
    static constexpr const char* kComponent[] = {"x", "y", "z", "w"};
    for (u32 unit = 0; unit < state.texgen_plane_mode.size(); ++unit) {
        const auto& modes = state.texgen_plane_mode[unit];
        bool any = false, eye = false, sphere = false;
        for (FFTexGenMode m : modes) {
            any |= m != FFTexGenMode::None;
            eye |= m == FFTexGenMode::EyeLinear || m == FFTexGenMode::SphereMap;
            sphere |= m == FFTexGenMode::SphereMap;
        }
        if (!any) continue;
        ss << "    { // glTexGen, unit " << unit << "\n"
           << "        float4 gl_obj = "
           << (state.position_is_homogeneous ? "in.position;\n" : "float4(in.position, 1.0);\n");
        if (eye) ss << "        float4 gl_eye = gl_obj * cb.world;\n";
        if (sphere) {
            // Eye w = 0: inf/NaN; the spec leaves u undefined at infinity.
            ss << "        float3 gl_u = normalize(gl_eye.xyz / gl_eye.w);\n"
               << "        float3 gl_n = (float4(in.normal, 0.0) * cb.world_inv_transpose).xyz;\n";
            if (state.normalize_normals) ss << "        gl_n = normalize(gl_n);\n";
            ss << "        float3 gl_r = gl_u - 2.0 * gl_n * dot(gl_n, gl_u);\n"
               << "        float gl_m = 2.0 * sqrt(gl_r.x * gl_r.x + gl_r.y * gl_r.y + "
                  "(gl_r.z + 1.0) * (gl_r.z + 1.0));\n";
        }
        for (u32 c = 0; c < 4; ++c) {
            const std::string target = "out.texcoord" + std::to_string(unit) + "." + kComponent[c];
            const std::string plane = "cb.texgen_planes[" + std::to_string(unit * 4 + c) + "]";
            switch (modes[c]) {
            case FFTexGenMode::None: break;
            case FFTexGenMode::ObjectLinear: ss << "        " << target << " = dot(" << plane << ", gl_obj);\n"; break;
            case FFTexGenMode::EyeLinear: ss << "        " << target << " = dot(" << plane << ", gl_eye);\n"; break;
            case FFTexGenMode::SphereMap:
                ss << "        " << target << " = gl_r." << kComponent[c] << " / gl_m + 0.5;\n";
                break;
            }
        }
        ss << "    }\n";
    }
}

/// GL 1.1 texture sampling (spec 3.8.1-3.8.2; ff_emulation.hpp
/// gl_texture_sampling): the per-stage parameters and the filter functions.
/// GL enumerants appear as their values: NEAREST 0x2600, LINEAR 0x2601,
/// NEAREST_MIPMAP_NEAREST 0x2700, LINEAR_MIPMAP_NEAREST 0x2701,
/// NEAREST_MIPMAP_LINEAR 0x2702, LINEAR_MIPMAP_LINEAR 0x2703, REPEAT 0x2901.
static void emit_gl_texture_helpers(std::ostringstream& ss, const FFPipelineState& state) {
    if (!state.gl_texture_sampling) return;
    ss << "struct GlTexUnit {\n"
       << "    uint4 filter;   // min, mag, wrap s, wrap t\n"
       << "    uint4 size;     // log2 width (n), log2 height (m), top level p, dimensions\n"
       << "    float4 border;  // TEXTURE_BORDER_COLOR in the stored layout\n"
       << "};\n\n"
       // A texel of the level, or the border colour for an index outside it
       // (spec 3.8.1: i < 0, j < 0, i >= w, j >= h with no border texels).
       << "static float4 gl_texel(texture2d<float> t, int i, int j, uint lvl, int w, int h, float4 border) {\n"
       << "    if (i < 0 || j < 0 || i >= w || j >= h) return border;\n"
       << "    return t.read(uint2(uint(i), uint(j)), lvl);\n"
       << "}\n\n"
       // NEAREST (eq. 3.13/3.14) or LINEAR (eq. 3.15) on mipmap array lvl.
       << "static float4 gl_filter(texture2d<float> t, float2 st, uint lvl, bool linear, constant GlTexUnit& u) {\n"
       << "    const bool one_d = u.size.w == 1u;\n"
       << "    const int w = 1 << max(int(u.size.x) - int(lvl), 0);\n"
       << "    const int h = one_d ? 1 : 1 << max(int(u.size.y) - int(lvl), 0);\n"
       << "    const bool rs = u.filter.z == 0x2901u;\n"
       << "    const bool rt = u.filter.w == 0x2901u;\n"
       // REPEAT uses the fractional part s - floor(s); CLAMP clamps to [0, 1].
       << "    const float s = rs ? st.x - floor(st.x) : clamp(st.x, 0.0, 1.0);\n"
       << "    const float tc = rt ? st.y - floor(st.y) : clamp(st.y, 0.0, 1.0);\n"
       << "    const float fu = float(w) * s;\n"
       << "    const float fv = float(h) * tc;\n"
       << "    if (!linear) {\n"
       << "        int i = rs ? (int(floor(fu)) & (w - 1)) : (s < 1.0 ? int(floor(fu)) : w - 1);\n"
       << "        int j = one_d ? 0 : (rt ? (int(floor(fv)) & (h - 1)) : (tc < 1.0 ? int(floor(fv)) : h - 1));\n"
       << "        return gl_texel(t, i, j, lvl, w, h, u.border);\n"
       << "    }\n"
       << "    const float a = fu - 0.5;\n"
       << "    const float b = fv - 0.5;\n"
       << "    int i0 = int(floor(a));\n"
       << "    int i1 = i0 + 1;\n"
       << "    const float alpha = a - floor(a);\n"
       << "    if (rs) { i0 &= w - 1; i1 &= w - 1; }\n"
       << "    if (one_d) {\n"
       << "        return (1.0 - alpha) * gl_texel(t, i0, 0, lvl, w, h, u.border) +\n"
       << "               alpha * gl_texel(t, i1, 0, lvl, w, h, u.border);\n"
       << "    }\n"
       << "    int j0 = int(floor(b));\n"
       << "    int j1 = j0 + 1;\n"
       << "    const float beta = b - floor(b);\n"
       << "    if (rt) { j0 &= h - 1; j1 &= h - 1; }\n"
       << "    return (1.0 - alpha) * (1.0 - beta) * gl_texel(t, i0, j0, lvl, w, h, u.border) +\n"
       << "           alpha * (1.0 - beta) * gl_texel(t, i1, j0, lvl, w, h, u.border) +\n"
       << "           (1.0 - alpha) * beta * gl_texel(t, i0, j1, lvl, w, h, u.border) +\n"
       << "           alpha * beta * gl_texel(t, i1, j1, lvl, w, h, u.border);\n"
       << "}\n\n"
       // Minification or magnification by lambda, and the mipmap selection.
       << "static float4 gl_sample(texture2d<float> t, float2 st, float lambda, constant GlTexUnit& u) {\n"
       << "    const uint minf = u.filter.x;\n"
       << "    const uint magf = u.filter.y;\n"
       << "    const float c = (magf == 0x2601u && (minf == 0x2700u || minf == 0x2702u)) ? 0.5 : 0.0;\n"
       << "    if (!(lambda > c)) return gl_filter(t, st, 0u, magf == 0x2601u, u);\n"
       << "    if (minf == 0x2600u || minf == 0x2601u) return gl_filter(t, st, 0u, minf == 0x2601u, u);\n"
       << "    const bool lin = minf == 0x2701u || minf == 0x2703u;\n"
       << "    const float p = float(u.size.z);\n"
       << "    if (minf == 0x2700u || minf == 0x2701u) {\n"
       // d - 1/2 < lambda <= d + 1/2 for 1 <= d <= p; level 0 up to 1/2.
       << "        const float d = lambda <= 0.5 ? 0.0 : (lambda > p + 0.5 ? p : ceil(lambda - 0.5));\n"
       << "        return gl_filter(t, st, uint(d), lin, u);\n"
       << "    }\n"
       << "    const float d1 = floor(lambda);\n"
       << "    if (d1 >= p) return gl_filter(t, st, uint(p), lin, u);\n"
       << "    const float f = lambda - d1;\n"
       << "    return (1.0 - f) * gl_filter(t, st, uint(d1), lin, u) + f * gl_filter(t, st, uint(d1) + 1u, lin, u);\n"
       << "}\n\n";
}

/// Each GL stage's coordinate (s/q, t/q) and level of detail, computed before
/// any discard so the derivatives see the whole quad. lambda is eq. 3.11 (the
/// polygon formula) for every primitive. GL lines and points arrive as
/// CPU-expanded quads whose corners carry their projection's coordinates, so
/// the derivatives run along the line only; eq. 3.12, the line formula, is
/// the directional derivative along the segment, which equals 3.11 for
/// axis-aligned lines and is up to sqrt(2) larger for diagonal ones. 3.11 is
/// used because the fragment stage does not know the segment's direction.
/// For points both mean magnification (rho = 1, or 0 derivatives here).
static void emit_gl_texture_coordinates(std::ostringstream& ss, const FFPipelineState& state, u32 tex_count) {
    if (!state.gl_texture_sampling) return;
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        const auto& stage = state.texture_stages[i];
        if (stage.color_op == TexStageOp::Disable) break;
        const std::string n = std::to_string(i);
        const u32 set = stage.texcoord_index & 0xFFFFu;
        const std::string tc = set < tex_count ? "in.texcoord" + std::to_string(set) : std::string("float4(0.0, 0.0, 0.0, 1.0)");
        ss << "    float4 gl_tc" << n << " = " << tc;
        if (stage.texture_transform_flags != 0) ss << " * cb.texture_matrix[" << n << "]";
        ss << ";\n"
           << "    float2 gl_st" << n << " = gl_tc" << n << ".xy / gl_tc" << n << ".w;\n"
           << "    float gl_lambda" << n << ";\n"
           << "    {\n"
           << "        float2 gl_scale = float2(float(1u << cb.gl_tex[" << n << "].size.x), "
              "cb.gl_tex[" << n << "].size.w == 1u ? 0.0 : float(1u << cb.gl_tex[" << n << "].size.y));\n"
           << "        float2 gl_uv = gl_st" << n << " * gl_scale;\n"
           << "        float2 gl_dx = dfdx(gl_uv);\n"
           << "        float2 gl_dy = dfdy(gl_uv);\n"
           << "        gl_lambda" << n << " = log2(max(length(gl_dx), length(gl_dy)));\n"
           << "    }\n";
    }
}

/// Line (spec 3.4.2) and polygon (spec 3.5.2) stipple discards.
static void emit_gl_stipple(std::ostringstream& ss, const FFPipelineState& state) {
    if (state.line_stipple.enabled) {
        ss << "    {\n"
           << "        uint gl_bit = (uint(max(in.line_distance, 0.0)) / cb.line_stipple.x) & 15u;\n"
           << "        if (((cb.line_stipple.y >> gl_bit) & 1u) == 0u) discard_fragment();\n"
           << "    }\n";
    }
    if (state.polygon_stipple) {
        ss << "    {\n"
           << "        uint2 gl_pixel = uint2(in.position.xy) & 31u;\n"
           << "        uint gl_row = cb.polygon_stipple[gl_pixel.y >> 2][gl_pixel.y & 3u];\n"
           << "        if (((gl_row >> gl_pixel.x) & 1u) == 0u) discard_fragment();\n"
           << "    }\n";
    }
}

/// The logic op (spec 4.1.10, Table 4.5) on the 8-bit unorm values.
static void emit_gl_logic_op(std::ostringstream& ss, const FFPipelineState& state, const std::string& color,
                             const std::string& dst) {
    if (state.logic_op == FFLogicOp::Copy) return;
    static constexpr const char* kOps[] = {
        "uint4(0u)",   "s & d",  "s & ~d",  "s",      "~s & d",   "d",       "s ^ d",   "s | d",
        "~(s | d)",    "~(s ^ d)", "~d",    "s | ~d", "~s",       "~s | d",  "~(s & d)", "uint4(255u)",
    };
    ss << "    {\n"
       << "        uint4 s = uint4(rint(saturate(" << color << ") * 255.0));\n"
       << "        uint4 d = uint4(rint(saturate(" << dst << ") * 255.0));\n"
       << "        uint4 r = (" << kOps[static_cast<u32>(state.logic_op) & 15u] << ") & 255u;\n"
       << "        " << color << " = float4(r) / 255.0;\n"
       << "    }\n";
}

// =============================================================================
// Vertex shader generation
// =============================================================================

/// The camera-space normal N * (wv^-1)^T, 3x3 part (Learn, "Camera Space
/// Transformations (Direct3D 9)"; "Geometry Blending (Direct3D 9)" for the
/// weighted sum). Each `*_inv_transpose` uniform is packed from world * view,
/// so the row-vector product is the whole transform (ff_emulation.hpp).
[[nodiscard]] static std::string camera_space_normal_expr(const FFPipelineState& state, bool vertex_blending) {
    const std::string n =
        std::string("float4(") + (state.vertex_tweening ? "tween_normal" : "in.normal") + ", 0.0)";
    if (!vertex_blending) return "(" + n + " * cb.world_inv_transpose).xyz";
    static constexpr const char* components[] = {"x", "y", "z", "w"};
    std::string e = "(";
    for (u32 i = 0; i <= state.vertex_blend_weight_count; ++i) {
        if (i != 0) e += " + ";
        e += "blend_weight" + std::to_string(i) + " * (" + n + " * ";
        if (state.indexed_vertex_blend) {
            e += std::string("cb.blend_world_inv_transposes[uint(in.blend_indices.") + components[i] + ")]";
        } else {
            e += "cb.world" + (i != 0 ? std::to_string(i) : std::string()) + "_inv_transpose";
        }
        e += ")";
    }
    return e + ").xyz";
}

static std::string generate_vertex_shader(const FFPipelineState& state) {
    std::ostringstream ss;
    ss << "#include <metal_stdlib>\n"
       << "using namespace metal;\n\n";

    // A D3DFVF_XYZRHW vertex is already in screen space; the world, view, and
    // projection transforms and per-vertex lighting do not apply to it.
    const bool pretransformed =
        (state.fvf_flags & fvf::POSITION_MASK) == fvf::XYZRHW;
    const bool vertex_blending =
        state.vertex_blend_weight_count != 0 || state.indexed_vertex_blend;
    // position_is_homogeneous: the float4 object position, divided by its
    // own w where an eye-space point is needed.
    const bool homogeneous = state.position_is_homogeneous;
    const bool lighting_active =
        ff_lighting_uniforms(state) && (state.fvf_flags & fvf::NORMAL) != 0 && !pretransformed;

    // Uniform buffer for transforms and lighting
    ss << "struct FFUniforms {\n"
       << "    float4x4 world;\n"
       << "    float4x4 view;\n"
       << "    float4x4 projection;\n"
       << "    float4x4 world_view_proj;\n"
       << "    float4x4 world_inv_transpose;\n";
    if (state.indexed_vertex_blend) {
        ss << "    float4x4 blend_worlds[9];\n"
           << "    float4x4 blend_world_inv_transposes[9];\n";
    } else if (state.vertex_blend_weight_count != 0) {
        for (u32 i = 1; i <= state.vertex_blend_weight_count; ++i) {
            ss << "    float4x4 world" << i << ";\n"
               << "    float4x4 world" << i << "_inv_transpose;\n";
        }
    }
    if (state.vertex_tweening) {
        ss << "    float4 tween_params;\n";
    }

    if (pretransformed) {
        // x = render target width, y = height, in pixels.
        ss << "    float4 viewport_params;\n";
    }
    if (state.point_size_output) {
        ss << "    float4 point_params;\n";
        if (state.point_scale_enabled && !pretransformed) {
            ss << "    float4 point_scale_params;\n";
        }
    }
    if (state.clip_planes_enabled && !pretransformed) {
        ss << "    float4 clip_planes[6];\n";
    }

    const bool lighting_uniforms = ff_lighting_uniforms(state);
    if (lighting_uniforms) {
        ss << "    float4 camera_position;\n"
           << "    float4 camera_direction;\n"
           << "    float4 global_ambient;\n"
           << "    float4 mat_diffuse;\n"
           << "    float4 mat_ambient;\n"
           << "    float4 mat_specular;\n"
           << "    float4 mat_emissive;\n"
           << "    float  mat_power;\n"
           << "    float  _pad0[3];\n";

        for (u32 i = 0; i < state.active_light_count; ++i) {
            ss << "    float4 light" << i << "_diffuse;\n"
               << "    float4 light" << i << "_specular;\n"
               << "    float4 light" << i << "_ambient;\n"
               << "    float4 light" << i << "_position;\n"   // .w = type
               << "    float4 light" << i << "_direction;\n"  // .w = range
               << "    float4 light" << i << "_atten;\n";     // const, lin, quad, phi (unused)
            if (!state.gl_spot_model && state.lights[i].type == LightType::Spot) {
                // cos(Theta / 2), cos(Phi / 2), Falloff (ff_uniforms.cpp).
                ss << "    float4 light" << i << "_spot;\n";
            }
        }
    }

    if (state.fog_mode != FogMode::None) {
        ss << "    float4 fog_params;\n"; // x=start, y=end, z=density, w=unused
    }
    if (state.two_sided_lighting && lighting_uniforms) {
        ss << "    float4 back_mat_diffuse;\n"
           << "    float4 back_mat_ambient;\n"
           << "    float4 back_mat_specular;\n"
           << "    float4 back_mat_emissive;\n"
           << "    float4 back_mat_power;\n";   // x = shininess
    }
    if (ff_texgen_needs_planes(state)) {
        ss << "    float4 texgen_planes[16];\n";   // [unit * 4 + coordinate]
    }
    if (state.window_offset) {
        ss << "    float4 gl_window_scale;\n";   // x = 2 / viewport width, y = 2 / height
    }

    ss << "};\n\n";

    // Vertex input
    ss << "struct FFVertexIn {\n";
    if (pretransformed) {
        // x, y in pixels, z in 0..1, w carrying RHW (1/w).
        ss << "    float4 position [[attribute(0)]];\n";
    } else if (homogeneous) {
        // GL object coordinates (x, y, z, w), unchanged (spec 2.10).
        ss << "    float4 position [[attribute(0)]];\n";
    } else {
        ss << "    float3 position [[attribute(0)]];\n";
    }
    u32 attr_idx = 1;
    if (state.vertex_tweening) {
        ss << "    float3 position1 [[attribute(" << attr_idx++ << ")]];\n";
    }
    const u32 fvf_position = state.fvf_flags & fvf::POSITION_MASK;
    const bool packed_blend_indices =
        (state.fvf_flags & fvf::LASTBETA_UBYTE4) != 0;
    const u32 beta_slots =
        fvf_position >= fvf::XYZB1 && fvf_position <= fvf::XYZB5
            ? (fvf_position - fvf::XYZB1) / 2u + 1u : 0u;
    const u32 blend_weight_slots =
        beta_slots - (packed_blend_indices && beta_slots != 0 ? 1u : 0u);
    if (blend_weight_slots != 0) {
        ss << "    float" << (blend_weight_slots == 1 ? "" : std::to_string(blend_weight_slots))
           << " blend_weights [[attribute(" << attr_idx++ << ")]];\n";
    }
    if (packed_blend_indices) {
        ss << "    uchar4 blend_indices [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_normal = (state.fvf_flags & fvf::NORMAL) != 0;
    if (has_normal) {
        ss << "    float3 normal [[attribute(" << attr_idx++ << ")]];\n";
        if (state.vertex_tweening) {
            ss << "    float3 normal1 [[attribute(" << attr_idx++ << ")]];\n";
        }
    }
    const bool has_point_size = (state.fvf_flags & fvf::PSIZE) != 0;
    if (has_point_size) {
        ss << "    float point_size [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_diffuse = (state.fvf_flags & fvf::DIFFUSE) != 0;
    if (has_diffuse) {
        ss << "    float4 color [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_specular = (state.fvf_flags & fvf::SPECULAR) != 0;
    if (has_specular) {
        ss << "    float4 specular_color [[attribute(" << attr_idx++ << ")]];\n";
    }
    const u32 tex_count = state.texcoord_count();
    for (u32 i = 0; i < tex_count; ++i) {
        const u32 comps =
            ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 1u   ? 3u
            : ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 2u ? 4u
            : ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 3u ? 1u
                                                                  : 2u;
        if (comps == 1) {
            ss << "    float texcoord" << i << " [[attribute(" << attr_idx++
               << ")]];\n";
        } else {
            ss << "    float" << comps << " texcoord" << i << " [[attribute("
               << attr_idx++ << ")]];\n";
        }
    }
    if (state.line_stipple.enabled) {
        // The stipple counter (spec 3.4.2): always the last attribute.
        ss << "    float line_distance [[attribute(" << attr_idx++ << ")]];\n";
    }
    if (state.window_offset) {
        // CPU line/point expansion: pixels to move the corner (after line_distance).
        ss << "    float2 window_offset [[attribute(" << attr_idx++ << ")]];\n";
    }
    if (state.depth_offset) {
        // Polygon offset terms: always the last attribute.
        ss << "    float2 depth_offset [[attribute(" << attr_idx++ << ")]];\n";
    }
    ss << "};\n\n";

    // Vertex output / fragment input — float4 so TCI xyz and texture
    // transforms (COUNT3/4 + PROJECTED) survive into the fragment stage.
    ss << "struct FFVertexOut {\n"
       << "    float4 position [[position]];\n"
       << (state.point_size_output
               ? "    float point_size [[point_size]];\n"
               : "")
       << (state.clip_planes_enabled && !pretransformed
               ? "    float clip_distances [[clip_distance]] [6];\n"
               : "")
       << "    float4 diffuse" << (state.flat_shading ? " [[flat]]" : "") << ";\n"
       << "    float4 specular" << (state.flat_shading ? " [[flat]]" : "") << ";\n";
    for (u32 i = 0; i < tex_count; ++i) {
        ss << "    float4 texcoord" << i << ";\n";
    }
    if (state.fog_mode != FogMode::None) {
        ss << "    float fog_factor;\n";
    }
    emit_gl_varyings(ss, state);
    ss << "};\n\n";
    if (state.gl_spot_model && lighting_active) emit_gl_light_function(ss);

    // Vertex function
    ss << "vertex FFVertexOut ff_vertex(\n"
       << "    FFVertexIn in [[stage_in]],\n"
       << "    constant FFUniforms& cb [[buffer(0)]])\n{\n"
       << "    FFVertexOut out;\n";
    if (state.vertex_tweening) {
        ss << "    float3 tween_position = mix(in.position, in.position1, "
              "cb.tween_params.x);\n";
        if (has_normal) {
            ss << "    float3 tween_normal = mix(in.normal, in.normal1, "
                  "cb.tween_params.x);\n";
        }
    }
    if (vertex_blending) {
        static constexpr const char* components[] = {"x", "y", "z", "w"};
        for (u32 i = 0; i < state.vertex_blend_weight_count; ++i) {
            ss << "    float blend_weight" << i << " = in.blend_weights";
            if (blend_weight_slots != 1) ss << "." << components[i];
            ss << ";\n";
        }
        ss << "    float blend_weight" << state.vertex_blend_weight_count
           << " = 1.0";
        for (u32 i = 0; i < state.vertex_blend_weight_count; ++i) {
            ss << " - blend_weight" << i;
        }
        ss << ";\n"
           << "    float4 object_position = float4("
           << (state.vertex_tweening ? "tween_position" : "in.position")
           << ", 1.0);\n"
           << "    float4 blended_world_position = ";
        for (u32 i = 0; i <= state.vertex_blend_weight_count; ++i) {
            if (i != 0) ss << " + ";
            ss << "blend_weight" << i << " * (object_position * ";
            if (state.indexed_vertex_blend) {
                ss << "cb.blend_worlds[uint(in.blend_indices."
                   << components[i] << ")]";
            } else {
                ss << "cb.world";
                if (i != 0) ss << i;
            }
            ss << ")";
        }
        ss << ";\n";
    }
    if (state.point_size_output) {
        if (state.point_scale_enabled && !pretransformed && homogeneous) {
            // Eye w = 0 gives an inf/NaN distance (undefined for a vertex
            // at infinity), like the fog and lighting divides.
            ss << "    float4 point_eye4 = in.position * cb.world * cb.view;\n"
               << "    float3 point_eye = point_eye4.xyz / point_eye4.w;\n";
        } else if (state.point_scale_enabled && !pretransformed) {
            ss << "    float3 point_eye = "
               << (vertex_blending
                       ? "(blended_world_position * cb.view).xyz;\n"
                       : (state.vertex_tweening
                              ? "(float4(tween_position, 1.0) * cb.world * cb.view).xyz;\n"
                              : "(float4(in.position, 1.0) * cb.world * cb.view).xyz;\n"));
        }
        if (state.point_scale_enabled && !pretransformed) {
            ss << "    float point_distance = length(point_eye);\n"
               << "    float point_denominator = cb.point_scale_params.x + "
                  "cb.point_scale_params.y * point_distance + "
                  "cb.point_scale_params.z * point_distance * point_distance;\n"
               << "    float scaled_point_size = "
               << (has_point_size ? "in.point_size" : "cb.point_params.x")
               << " * "
                  "cb.point_scale_params.w * rsqrt(max(point_denominator, "
                  "1.0e-20));\n"
               << "    out.point_size = clamp(scaled_point_size, "
                  "cb.point_params.y, cb.point_params.z);\n";
        } else {
            if (has_point_size) {
                ss << "    out.point_size = clamp(in.point_size, "
                      "cb.point_params.y, cb.point_params.z);\n";
            } else {
                ss << "    out.point_size = cb.point_params.x;\n";
            }
        }
    }
    if (state.clip_planes_enabled && !pretransformed) {
        ss << "    float4 clip_world_position = "
           << (homogeneous ? "in.position * cb.world;\n"
               : vertex_blending
                   ? "blended_world_position;\n"
                   : (state.vertex_tweening
                          ? "float4(tween_position, 1.0) * cb.world;\n"
                          : "float4(in.position, 1.0) * cb.world;\n"))
           << "    for (uint clip_index = 0; clip_index < 6; ++clip_index) {\n"
           << "        out.clip_distances[clip_index] = "
              "dot(cb.clip_planes[clip_index], clip_world_position);\n"
           << "    }\n";
    }

    if (pretransformed) {
        // D3D9 defines integer screen-space coordinates as pixel centres,
        // whereas Metal places pixel centres at (i + 0.5, j + 0.5). Preserve
        // the D3D9 POSITIONT coordinate by translating it into Metal window
        // coordinates before the NDC map. Applications may already author
        // textured quads at -0.5 boundaries for D3D9 texel alignment; that
        // application-provided correction must not be applied a second time.
        // See Microsoft Learn, "Directly Mapping Texels to Pixels
        // (Direct3D 9)", and Apple's Metal Programming Guide, "Working with
        // Viewport and Pixel Coordinate Systems".
        ss << "    float rhw = (in.position.w != 0.0) ? in.position.w : 1.0;\n"
           << "    float clip_w = 1.0 / rhw;\n"
           << "    float2 screen = in.position.xy + float2(0.5, 0.5);\n"
           << "    float2 ndc = float2(\n"
           << "        screen.x / cb.viewport_params.x * 2.0 - 1.0,\n"
           << "        1.0 - screen.y / cb.viewport_params.y * 2.0);\n"
           << "    out.position = float4(ndc * clip_w, in.position.z * clip_w,"
           << " clip_w);\n";
    } else {
        if (homogeneous) {
            ss << "    out.position = in.position * cb.world_view_proj;\n";
            if (state.window_offset) {
                // NDC = window offset * 2 / viewport size; scaled by w so the
                // divide leaves exactly that displacement (Design 6).
                ss << "    out.position.xy += in.window_offset * cb.gl_window_scale.xy * out.position.w;\n";
            }
        } else if (vertex_blending) {
            ss << "    out.position = blended_world_position * cb.view * "
                  "cb.projection;\n";
        } else {
            ss << "    out.position = float4("
               << (state.vertex_tweening ? "tween_position" : "in.position")
               << ", 1.0) * cb.world_view_proj;\n";
        }
    }

    // Pass through mesh UVs as float4, then overwrite any set that a stage
    // requests via D3DTSS_TCI_CAMERASPACE* (projected lightmaps, env maps).
    for (u32 i = 0; i < tex_count; ++i) {
        const u32 comps =
            ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 1u   ? 3u
            : ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 2u ? 4u
            : ((state.fvf_flags >> (16u + i * 2u)) & 0x3u) == 3u ? 1u
                                                                  : 2u;
        if (comps == 1) {
            ss << "    out.texcoord" << i << " = float4(in.texcoord" << i
               << ", 0.0, 0.0, 1.0);\n";
        } else if (comps == 2) {
            ss << "    out.texcoord" << i << " = float4(in.texcoord" << i
               << ", 0.0, 1.0);\n";
        } else if (comps == 3) {
            ss << "    out.texcoord" << i << " = float4(in.texcoord" << i
               << ", 1.0);\n";
        } else {
            ss << "    out.texcoord" << i << " = in.texcoord" << i << ";\n";
        }
    }
    if (!pretransformed) {
        bool need_view = false;
        for (u32 s = 0; s < state.active_texture_stages; ++s) {
            const u32 tci = state.texture_stages[s].texcoord_index & 0xFFFF0000u;
            if (tci != 0) {
                need_view = true;
                break;
            }
        }
        if (need_view) {
            // Prefixed names — lighting below reuses world_pos / view_dir.
            ss << "    float4 tci_world_pos = "
               << (vertex_blending
                       ? "blended_world_position;\n"
                       : (state.vertex_tweening
                              ? "float4(tween_position, 1.0) * cb.world;\n"
                              : "float4(in.position, 1.0) * cb.world;\n"))
               << "    float4 tci_view_pos = tci_world_pos * cb.view;\n";
            if (has_normal) {
                // Camera-space normal N * (wv^-1)^T (Learn, "Camera Space
                // Transformations (Direct3D 9)"): the normal matrices are
                // packed per world-view (ff_uniforms.cpp).
                ss << "    float3 tci_view_n = "
                   << camera_space_normal_expr(state, vertex_blending) << ";\n";
                if (state.normalize_normals) {
                    ss << "    tci_view_n = normalize(tci_view_n);\n";
                }
            }
            for (u32 s = 0; s < state.active_texture_stages; ++s) {
                const u32 raw = state.texture_stages[s].texcoord_index;
                const u32 set = raw & 0xFFFFu;
                const u32 tci = raw & 0xFFFF0000u;
                if (tci == 0 || set >= tex_count) continue;
                // D3DTSS_TCI_CAMERASPACENORMAL = 0x10000
                // D3DTSS_TCI_CAMERASPACEPOSITION = 0x20000
                // D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR = 0x30000
                if (tci == 0x10000u && has_normal) {
                    ss << "    out.texcoord" << set
                       << " = float4(tci_view_n, 0.0);\n";
                } else if (tci == 0x20000u) {
                    ss << "    out.texcoord" << set
                       << " = float4(tci_view_pos.xyz, 1.0);\n";
                } else if (tci == 0x30000u && has_normal) {
                    ss << "    {\n"
                       << "      float3 tci_view_dir = normalize(tci_view_pos.xyz);\n"
                       << "      float3 tci_refl = reflect(tci_view_dir, tci_view_n);\n"
                       << "      out.texcoord" << set
                       << " = float4(tci_refl, 0.0);\n"
                       << "    }\n";
                }
            }
        }
    }

    emit_gl_texgen(ss, state);
    if (state.line_stipple.enabled) {
        ss << "    out.line_distance = in.line_distance;\n";
    }
    if (state.depth_offset) {
        ss << "    out.depth_offset = in.depth_offset;\n";
    }

    // Resolve D3DMCS_* → material or vertex colour (COLORVERTEX).
    // Defaults use COLOR1 for diffuse; a zeroed D3DMATERIAL9 then yields
    // black lit geometry when vertex colour is ignored.
    const auto material_source_expr = [&](const char* mat_field, u32 source) {
        if (!state.vertex_color_enable || source == 0u /* D3DMCS_MATERIAL */) {
            return std::string("cb.") + mat_field;
        }
        if (source == 1u /* D3DMCS_COLOR1 */ && has_diffuse) {
            return std::string("in.color");
        }
        if (source == 2u /* D3DMCS_COLOR2 */ && has_specular) {
            return std::string("in.specular_color");
        }
        return std::string("cb.") + mat_field;
    };

    // Lighting calculation
    if (lighting_active) {
        const std::string src_diffuse = material_source_expr(
            "mat_diffuse", state.diffuse_material_source);
        const std::string src_ambient = material_source_expr(
            "mat_ambient", state.ambient_material_source);
        const std::string src_specular = material_source_expr(
            "mat_specular", state.specular_material_source);
        const std::string src_emissive = material_source_expr(
            "mat_emissive", state.emissive_material_source);

        ss << "\n    // Lighting\n"
           << "    float4 lit_diffuse = " << src_diffuse << ";\n"
           << "    float4 lit_ambient = " << src_ambient << ";\n"
           << "    float4 lit_specular = " << src_specular << ";\n"
           << "    float4 lit_emissive = " << src_emissive << ";\n";
        if (homogeneous) {
            // Eye w = 0 (a vertex at infinity) makes xyz / w inf/NaN; the
            // spec leaves lighting such a vertex undefined (2.13.1 needs
            // the vertex as a point V).
            ss << "    float4 lighting_world4 = in.position * cb.world;\n"
               << "    float3 world_pos = lighting_world4.xyz / lighting_world4.w;\n";
        } else {
            // Direct3D lights in camera space (Learn, "Camera Space
            // Transformations (Direct3D 9)": V * wvMatrix; the packer moves
            // the lights there with Lp * vMatrix). `world_pos` names the
            // lighting space, here Direct3D's camera space.
            ss << "    float3 world_pos = "
               << (vertex_blending
                       ? "(blended_world_position * cb.view).xyz;\n"
                       : (state.vertex_tweening
                              ? "(float4(tween_position, 1.0) * cb.world * cb.view).xyz;\n"
                              : "(float4(in.position, 1.0) * cb.world * cb.view).xyz;\n"));
        }
        if (homogeneous) {
            ss << "    float3 world_normal = (float4(in.normal, 0.0) * "
                  "cb.world_inv_transpose).xyz;\n";
        } else {
            // Camera space (Learn, "Camera Space Transformations (Direct3D
            // 9)"): N * (wv^-1)^T; `world_normal` names the lighting space.
            ss << "    float3 world_normal = "
               << camera_space_normal_expr(state, vertex_blending) << ";\n";
        }
        if (state.normalize_normals) {
            ss << "    world_normal = normalize(world_normal);\n";
        }
        if (state.gl_spot_model) {
            emit_gl_lighting(ss, state, material_source_expr);
        } else {
            ss
               << "    float3 total_diffuse = cb.global_ambient.rgb * lit_ambient.rgb;\n"
               << "    float3 total_specular = float3(0.0);\n";
            if (state.local_viewer) {
                ss << "    float3 view_dir = normalize(cb.camera_position.xyz - world_pos);\n\n";
            } else {
                ss << "    float3 view_dir = cb.camera_direction.xyz;\n\n";
            }

            for (u32 i = 0; i < state.active_light_count; ++i) {
                const auto& light = state.lights[i];
                ss << "    { // Light " << i << "\n";

                if (light.type == LightType::Directional) {
                    ss << "        float3 L = normalize(-cb.light" << i << "_direction.xyz);\n"
                       << "        float atten = 1.0;\n";
                } else {
                    ss << "        float3 L_vec = cb.light" << i << "_position.xyz - world_pos;\n"
                       << "        float dist = length(L_vec);\n"
                       << "        float3 L = L_vec / max(dist, 1e-6);\n"
                       << "        float atten = 1.0 / (cb.light" << i << "_atten.x + "
                       << "cb.light" << i << "_atten.y * dist + "
                       << "cb.light" << i << "_atten.z * dist * dist);\n"
                       // "Atten = 0, if the distance between the light and the
                       // vertex exceeds the light's range" (Learn, "Attenuation
                       // and Spotlight Factor (Direct3D 9)").
                       << "        if (dist > cb.light" << i << "_direction.w) atten = 0.0;\n";
                    if (light.type == LightType::Spot) {
                        // rho = norm(Ldcs) . norm(Ldir), Ldcs the negated
                        // camera-space light direction ("Attenuation and
                        // Spotlight Factor"). Beyond the outer cone (rho <=
                        // cos(Phi / 2)) no light, inside the inner cone (rho >
                        // cos(Theta / 2)) full light, between them
                        // ((rho - cos(Phi/2)) / (cos(Theta/2) - cos(Phi/2)))^Falloff
                        // (Learn, "Light Types (Direct3D 9)": the runtime halves
                        // both cone angles before their cosines).
                        ss << "        float rho = dot(normalize(-cb.light" << i << "_direction.xyz), L);\n"
                           << "        float4 spot_k = cb.light" << i << "_spot;\n"
                           << "        float spot_factor = rho > spot_k.x ? 1.0\n"
                           << "            : (rho <= spot_k.y ? 0.0\n"
                           << "               : pow((rho - spot_k.y) / (spot_k.x - spot_k.y), spot_k.z));\n"
                           << "        atten *= spot_factor;\n";
                    }
                }

                ss << "        float NdotL = max(dot(world_normal, L), 0.0);\n"
                   << "        total_diffuse += atten * NdotL * cb.light" << i
                   << "_diffuse.rgb * lit_diffuse.rgb;\n"
                   << "        total_diffuse += atten * cb.light" << i
                   << "_ambient.rgb * lit_ambient.rgb;\n";

                if (state.specular_enable) {
                    ss << "        float3 H = normalize(L + view_dir);\n"
                       << "        float NdotH = max(dot(world_normal, H), 0.0);\n"
                       << "        float spec = (NdotL > 0.0 && cb.mat_power > 0.0) ? pow(NdotH, cb.mat_power) : 0.0;\n"
                       << "        total_specular += atten * spec * cb.light" << i
                       << "_specular.rgb * lit_specular.rgb;\n";
                }

                ss << "    }\n";
            }

            ss << "    total_diffuse += lit_emissive.rgb;\n"
               << "    out.diffuse = float4(clamp(total_diffuse, 0.0, 1.0), lit_diffuse.a);\n"
               << "    out.specular = float4(clamp(total_specular, 0.0, 1.0), 0.0);\n";
        }

    } else {
        // No lighting — pass vertex color through
        if (has_diffuse && state.vertex_color_enable) {
            ss << "    out.diffuse = in.color;\n";
        } else {
            ss << "    out.diffuse = float4(1.0);\n";
        }
        if (has_specular) {
            ss << "    out.specular = in.specular_color;\n";
        } else {
            ss << "    out.specular = float4(0.0);\n";
        }
        if (state.two_sided_lighting) {
            ss << "    out.back_diffuse = out.diffuse;\n"
               << "    out.back_specular = out.specular;\n";
        }
    }

    // Fog factor
    if (state.fog_mode != FogMode::None && pretransformed) {
        // D3D9 takes the fog factor for a pre-transformed vertex from the
        // specular alpha channel; without a specular colour there is nothing
        // to take it from, so the vertex is unfogged.
        if (has_specular) {
            ss << "\n    out.fog_factor = in.specular_color.a;\n";
        } else {
            ss << "\n    out.fog_factor = 1.0;\n";
        }
    }
    if (state.fog_mode != FogMode::None && !pretransformed) {
        ss << "\n    // Fog\n";
        if (homogeneous) {
            // Eye w = 0 gives an inf/NaN distance: the spec leaves fog (3.9)
            // undefined for a vertex at infinity.
            ss << "    float4 fog_eye4 = in.position * cb.world * cb.view;\n"
               << "    float3 eye_position = fog_eye4.xyz / fog_eye4.w;\n";
        } else {
            ss << "    float3 eye_position = "
               << (vertex_blending
                       ? "(blended_world_position * cb.view).xyz;\n"
                       : (state.vertex_tweening
                              ? "(float4(tween_position, 1.0) * cb.world * cb.view).xyz;\n"
                              : "(float4(in.position, 1.0) * cb.world * cb.view).xyz;\n"));
        }
        if (state.range_fog_enable) {
            ss << "    float fog_distance = length(eye_position);\n";
        } else {
            ss << "    float fog_distance = abs(eye_position.z);\n";
        }
        switch (state.fog_mode) {
            case FogMode::Linear:
                ss << "    out.fog_factor = clamp((cb.fog_params.y - fog_distance) / "
                   << "(cb.fog_params.y - cb.fog_params.x), 0.0, 1.0);\n";
                break;
            case FogMode::Exp:
                ss << "    out.fog_factor = clamp(exp(-cb.fog_params.z * fog_distance), 0.0, 1.0);\n";
                break;
            case FogMode::Exp2:
                ss << "    out.fog_factor = clamp(exp(-cb.fog_params.z * cb.fog_params.z * "
                   << "fog_distance * fog_distance), 0.0, 1.0);\n";
                break;
            default:
                break;
        }
    }

    ss << "    return out;\n}\n";
    return ss.str();
}

// =============================================================================
// Fragment shader generation
// =============================================================================

static bool ff_needs_texture_matrices(const FFPipelineState& state) {
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        if (state.texture_stages[i].texture_transform_flags != 0) return true;
    }
    return false;
}

[[nodiscard]] static bool ff_uses_bump_env(const FFPipelineState& state) {
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        if (state.texture_stages[i].color_op == TexStageOp::BumpEnvMap ||
            state.texture_stages[i].color_op ==
                TexStageOp::BumpEnvMapLuminance) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] static std::string float_from_bits(u32 bits) {
    return "as_type<float>(uint(" + std::to_string(bits) + "u))";
}

static void emit_bump_offset(std::ostringstream& ss,
                             const TextureStageState& stage) {
    ss << "    bump_offset = float2(\n"
       << "        tex_sample.r * " << float_from_bits(stage.bump_env_mat00)
       << " + tex_sample.g * " << float_from_bits(stage.bump_env_mat01)
       << ",\n"
       << "        tex_sample.r * " << float_from_bits(stage.bump_env_mat10)
       << " + tex_sample.g * " << float_from_bits(stage.bump_env_mat11)
       << ");\n";
    if (stage.color_op == TexStageOp::BumpEnvMapLuminance) {
        ss << "    bump_luminance = tex_sample.b * "
           << float_from_bits(stage.bump_env_lscale) << " + "
           << float_from_bits(stage.bump_env_loffset) << ";\n";
    }
}

/// The current fallback samples a bound texture when TEXCOORDINDEX names a set the
/// FVF did not declare (stage 1 defaults to set 1; TEX1 meshes only have 0).
/// Substituting opaque white made albedo × lightmap stay red.
[[nodiscard]] static std::string ff_texcoord_expr(u32 requested,
                                                 u32 tex_count) {
    if (requested < tex_count) {
        return "in.texcoord" + std::to_string(requested);
    }
    if (tex_count > 0) {
        return "in.texcoord0";
    }
    return "float4(0.0)";
}

/// D3DTTFF_PROJECTED: "Each texture coordinate is divided by the last element
/// before being passed to the rasterizer", the last of the D3DTTFF_COUNTn
/// elements (Learn, D3DTEXTURETRANSFORMFLAGS; "Texture Coordinate Processing
/// (Direct3D 9)"). COUNT is 0 under a programmable vertex shader (the same
/// page); all four elements are then written, and the fourth is the last.
[[nodiscard]] static std::string projected_uv_expr(u32 count) {
    static constexpr const char* kLast[] = {"w", "x", "y", "z", "w"};
    return std::string("tc.xy / tc.") + kLast[count <= 4u ? count : 0u];
}

/// Emit a texture sample that applies D3DTS_TEXTUREn + D3DTTFF_* when set.
static void emit_texture_sample(std::ostringstream& ss, u32 stage_index,
                                const TextureStageState& stage,
                                const std::string& tc_expr,
                                bool apply_texture_matrix,
                                bool apply_bump_offset) {
    const u32 ttf = stage.texture_transform_flags;
    const u32 count = ttf & 0xFFu;
    const bool projected = (ttf & 0x100u) != 0;
    ss << "    {\n"
       << "      float4 tc = " << tc_expr << ";\n";
    // tc' = tc * D3DTS_TEXTUREn, row vector like geometry (Learn, "Texture
    // Coordinate Transformations (Direct3D 9)"; ff_emulation.hpp).
    if (apply_texture_matrix && ttf != 0) {
        ss << "      tc = tc * cb.texture_matrix[" << stage_index << "];\n";
    }
    if (projected) {
        ss << "      float2 uv = " << projected_uv_expr(count) << ";\n";
    } else {
        ss << "      float2 uv = tc.xy;\n";
    }
    if (apply_bump_offset) {
        ss << "      uv += bump_offset;\n";
    }
    ss << "      tex_sample = tex" << stage_index << ".sample(samp"
       << stage_index << ", uv, bias(cb.lod_biases[" << stage_index
       << "]));\n"
       << "    }\n";
}

static void emit_table_fog(std::ostringstream& ss,
                           const FFPipelineState& state,
                           const char* position) {
    ss << "    float fog_distance = ";
    if (state.table_fog_w) {
        ss << "1.0 / max(abs(" << position << ".w), 1e-6);\n";
    } else {
        ss << position << ".z;\n";
    }
    switch (state.fog_mode) {
    case FogMode::Linear:
        ss << "    float fog_factor = clamp((cb.fog_params.y - fog_distance) / "
              "max(cb.fog_params.y - cb.fog_params.x, 1e-6), 0.0, 1.0);\n";
        break;
    case FogMode::Exp:
        ss << "    float fog_factor = clamp(exp(-cb.fog_params.z * "
              "fog_distance), 0.0, 1.0);\n";
        break;
    case FogMode::Exp2:
        ss << "    float fog_factor = clamp(exp(-cb.fog_params.z * "
              "cb.fog_params.z * fog_distance * fog_distance), 0.0, 1.0);\n";
        break;
    default:
        ss << "    float fog_factor = 1.0;\n";
        break;
    }
    ss << "    current.rgb = mix(cb.fog_color.rgb, current.rgb, "
          "fog_factor);\n\n";
}

/// The two sRGB transfer functions, emitted only into shaders that need them.
/// Alpha is never converted -- D3D9 converts colour only.
static void emit_srgb_helpers(std::ostringstream& ss,
                              const FFPipelineState& state) {
    if (state.srgb_texture_mask == 0 && !state.srgb_write_enable) return;
    ss << "static inline float4 vitreo_srgb_decode(float4 c) {\n"
       << "    c.rgb = select(c.rgb / 12.92f,\n"
       << "                   powr((c.rgb + 0.055f) / 1.055f, 2.4f),\n"
       << "                   c.rgb > 0.04045f);\n"
       << "    return c;\n"
       << "}\n"
       << "static inline float3 vitreo_srgb_encode(float3 c) {\n"
       << "    c = saturate(c);\n"
       << "    return select(c * 12.92f,\n"
       << "                  1.055f * powr(c, 1.0f / 2.4f) - 0.055f,\n"
       << "                  c > 0.0031308f);\n"
       << "}\n\n";
}

static std::string generate_fragment_shader(const FFPipelineState& state) {
    std::ostringstream ss;
    ss << "#include <metal_stdlib>\n"
       << "using namespace metal;\n\n";
    emit_srgb_helpers(ss, state);

    const u32 tex_count = state.texcoord_count();
    const bool need_tex_matrices = ff_needs_texture_matrices(state);
    const bool use_bump_env = ff_uses_bump_env(state);
    emit_gl_texture_helpers(ss, state);

    // Fragment uniform buffer (texture_factor, fog_color, alpha_ref)
    ss << "struct FFFragUniforms {\n"
       << "    float4 texture_factor;\n"
       << "    float4 stage_constants[8];\n"
       << "    float lod_biases[8];\n";
    if (state.fog_mode != FogMode::None) {
        ss << "    float4 fog_color;\n";
        if (state.table_fog) ss << "    float4 fog_params;\n";
    }
    if (state.alpha_test_enable) {
        ss << "    float alpha_ref;\n"
           << "    float _pad_alpha[3];\n";
    }
    if (need_tex_matrices) {
        ss << "    float4x4 texture_matrix[" << state.active_texture_stages
           << "];\n";
    }
    if (state.line_stipple.enabled) {
        ss << "    uint4 line_stipple;\n";   // x = factor, y = pattern
    }
    if (state.polygon_stipple) {
        ss << "    uint4 polygon_stipple[8];\n";   // row r = [r / 4][r % 4]
    }
    if (state.gl_texture_sampling && state.active_texture_stages != 0) {
        ss << "    GlTexUnit gl_tex[" << state.active_texture_stages << "];\n";
    }
    ss << "};\n\n";

    // Fragment input (matches vertex output)
    ss << "struct FFFragIn {\n"
       << "    float4 position [[position]];\n"
       << "    float4 diffuse" << (state.flat_shading ? " [[flat]]" : "") << ";\n"
       << "    float4 specular" << (state.flat_shading ? " [[flat]]" : "") << ";\n";
    for (u32 i = 0; i < tex_count; ++i) {
        ss << "    float4 texcoord" << i << ";\n";
    }
    if (state.fog_mode != FogMode::None) {
        ss << "    float fog_factor;\n";
    }
    emit_gl_varyings(ss, state);
    ss << "};\n\n";

    // Fragment function. A GL state writing several targets or the polygon-
    // offset depth returns FFFragOut; every other state returns the colour.
    const bool struct_out = state.color_targets > 1 || state.depth_offset;
    if (struct_out) {
        ss << "struct FFFragOut {\n";
        for (u32 i = 0; i < state.color_targets; ++i) {
            ss << "    float4 color" << i << " [[color(" << i << ")]];\n";
        }
        if (state.depth_offset) ss << "    float depth [[depth(any)]];\n";
        ss << "};\n\n";
    }
    ss << (struct_out ? "fragment FFFragOut ff_fragment(\n" : "fragment float4 ff_fragment(\n")
       << "    FFFragIn in [[stage_in]],\n"
       << "    constant FFFragUniforms& cb [[buffer(0)]]";

    // Texture arguments
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        ss << ",\n    texture2d<float> tex" << i << " [[texture(" << i << ")]]";
        // GL sampling fetches texels itself (gl_texture_sampling).
        if (!state.gl_texture_sampling) ss << ",\n    sampler samp" << i << " [[sampler(" << i << ")]]";
    }
    // Spec 2.13.1: points and lines always take the front colour. A point
    // pipeline is known here (point_size_output); a line pipeline is not,
    // so the caller leaves two_sided_lighting off for lines (ff_emulation.hpp).
    const bool select_back = state.two_sided_lighting && !state.point_size_output;
    if (select_back) {
        ss << ",\n    bool ff_front_facing [[front_facing]]";
    }
    if (state.logic_op != FFLogicOp::Copy) {
        // Framebuffer fetch: the destination colour the logic op reads.
        ss << ",\n    float4 ff_dst [[color(0)]]";
        for (u32 i = 1; i < state.color_targets; ++i) {
            ss << ",\n    float4 ff_dst" << i << " [[color(" << i << ")]]";
        }
    }
    ss << ")\n{\n";
    if (state.depth_offset) {
        // Spec 3.5.5: o = m * factor + r * units, m = max(|dz/dx|, |dz/dy|)
        // (eq. 3.6) of the primitive's window-depth plane; the result is
        // clamped to [0, 1].
        ss << "    float gl_z = in.position.z;\n"
           << "    float gl_depth = clamp(gl_z + in.depth_offset.x + in.depth_offset.y * "
              "max(abs(dfdx(gl_z)), abs(dfdy(gl_z))), 0.0, 1.0);\n";
    }
    emit_gl_texture_coordinates(ss, state, tex_count);
    emit_gl_stipple(ss, state);

    // Start with diffuse vertex color as default "current"
    if (select_back) {
        ss << "    float4 v_diffuse = ff_front_facing ? in.diffuse : in.back_diffuse;\n"
           << "    float4 v_specular = ff_front_facing ? in.specular : in.back_specular;\n";
    } else {
        ss << "    float4 v_diffuse = in.diffuse;\n"
           << "    float4 v_specular = in.specular;\n";
    }
    ss << "    float4 current = v_diffuse;\n"
       << "    float4 temp = float4(0.0);\n"
       << "    float4 tex_sample;\n";
    if (use_bump_env) {
        ss << "    float2 bump_offset = float2(0.0);\n"
           << "    float bump_luminance = 1.0;\n";
    }
    ss << "\n";

    // Texture stage combiners
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        const auto& stage = state.texture_stages[i];
        if (stage.color_op == TexStageOp::Disable) break;
        const bool premodulate_current =
            i > 0 && state.texture_stages[i - 1].color_op ==
                         TexStageOp::Premodulate;
        const bool apply_bump_offset =
            i > 0 && (state.texture_stages[i - 1].color_op ==
                          TexStageOp::BumpEnvMap ||
                      state.texture_stages[i - 1].color_op ==
                          TexStageOp::BumpEnvMapLuminance);
        const bool apply_bump_luminance =
            i > 0 && state.texture_stages[i - 1].color_op ==
                         TexStageOp::BumpEnvMapLuminance;

        ss << "    // Texture stage " << i << "\n";
        ss << "    {\n";
        ss << "    float4 stage_result = current;\n";

        // Sample the texture. Low 16 bits of TEXCOORDINDEX select the set;
        // TCI_* high bits are applied in the FF vertex shader. D3DTS_TEXTUREn
        // and D3DTTFF_* are applied here (per stage). A missing set still
        // samples through the coordinate fallback above.
        const u32 tc = stage.texcoord_index & 0xFFFFu;
        if (state.gl_texture_sampling) {
            ss << "    tex_sample = gl_sample(tex" << i << ", gl_st" << i << ", gl_lambda" << i
               << ", cb.gl_tex[" << i << "]);\n";
        } else {
            emit_texture_sample(ss, i, stage, ff_texcoord_expr(tc, tex_count),
                                /*apply_texture_matrix=*/true, apply_bump_offset);
        }
        if (((state.srgb_texture_mask >> i) & 1u) != 0) {
            ss << "    tex_sample = vitreo_srgb_decode(tex_sample);\n";
        }
        if (stage.color_op == TexStageOp::BumpEnvMap ||
            stage.color_op == TexStageOp::BumpEnvMapLuminance) {
            emit_bump_offset(ss, stage);
        }

        ss << "    float4 arg0c = "
           << modified_arg_expr(stage.color_arg0, i, premodulate_current)
           << ";\n";
        ss << "    float4 arg1c = "
           << modified_arg_expr(stage.color_arg1, i, premodulate_current)
           << ";\n";
        ss << "    float4 arg2c = "
           << modified_arg_expr(stage.color_arg2, i, premodulate_current)
           << ";\n";
        emit_combiner_op(ss, "stage_result", ".rgb", stage.color_op,
                         "arg0c", "arg1c", "arg2c");
        if (apply_bump_luminance) {
            ss << "    stage_result.rgb = clamp(stage_result.rgb * bump_luminance, "
                  "0.0, 1.0);\n";
        }

        if (stage.alpha_op != TexStageOp::Disable) {
            ss << "    float4 arg0a = "
               << modified_arg_expr(stage.alpha_arg0, i, premodulate_current)
               << ";\n";
            ss << "    float4 arg1a = "
               << modified_arg_expr(stage.alpha_arg1, i, premodulate_current)
               << ";\n";
            ss << "    float4 arg2a = "
               << modified_arg_expr(stage.alpha_arg2, i, premodulate_current)
               << ";\n";
            emit_combiner_op(ss, "stage_result", ".a", stage.alpha_op,
                             "arg0a", "arg1a", "arg2a");
        }
        if (stage.color_op != TexStageOp::BumpEnvMap &&
            stage.color_op != TexStageOp::BumpEnvMapLuminance) {
            if ((stage.result_arg & 0xFu) ==
                static_cast<u32>(TexStageArg::Temp)) {
                ss << "    temp = stage_result;\n";
            } else {
                ss << "    current = stage_result;\n";
            }
        }

        ss << "    }\n\n";
    }

    // Add specular after texture stages (D3D9 behavior)
    if (state.specular_enable) {
        ss << "    current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);\n\n";
    }

    // Alpha test
    if (state.alpha_test_enable) {
        ss << "    // Alpha test\n";
        switch (state.alpha_test_func) {
            case 1: // D3DCMP_NEVER
                ss << "    discard_fragment();\n";
                break;
            case 2: // D3DCMP_LESS
                ss << "    if (current.a >= cb.alpha_ref) discard_fragment();\n";
                break;
            case 3: // D3DCMP_EQUAL
                ss << "    if (abs(current.a - cb.alpha_ref) > 0.004) discard_fragment();\n";
                break;
            case 4: // D3DCMP_LESSEQUAL
                ss << "    if (current.a > cb.alpha_ref) discard_fragment();\n";
                break;
            case 5: // D3DCMP_GREATER
                ss << "    if (current.a <= cb.alpha_ref) discard_fragment();\n";
                break;
            case 6: // D3DCMP_NOTEQUAL
                ss << "    if (abs(current.a - cb.alpha_ref) <= 0.004) discard_fragment();\n";
                break;
            case 7: // D3DCMP_GREATEREQUAL
                ss << "    if (current.a < cb.alpha_ref) discard_fragment();\n";
                break;
            case 8: // D3DCMP_ALWAYS
            default:
                break;
        }
        ss << "\n";
    }

    // Fog blending
    if (state.fog_mode != FogMode::None) {
        ss << "    // Fog\n";
        if (state.table_fog) {
            emit_table_fog(ss, state, "in.position");
        } else {
            ss << "    current.rgb = mix(cb.fog_color.rgb, current.rgb, "
                  "in.fog_factor);\n\n";
        }
    }

    if (state.srgb_write_enable) {
        ss << "    current.rgb = vitreo_srgb_encode(current.rgb);\n";
    }
    if (!struct_out) {
        emit_gl_logic_op(ss, state, "current", "ff_dst");
        ss << "    return current;\n}\n";
        return ss.str();
    }
    ss << "    FFFragOut gl_out;\n";
    for (u32 i = 0; i < state.color_targets; ++i) {
        const std::string target = "gl_out.color" + std::to_string(i);
        ss << "    " << target << " = current;\n";
        emit_gl_logic_op(ss, state, target, i == 0 ? std::string("ff_dst") : "ff_dst" + std::to_string(i));
    }
    if (state.depth_offset) ss << "    gl_out.depth = gl_depth;\n";
    ss << "    return gl_out;\n}\n";
    return ss.str();
}

// =============================================================================
// Public API
// =============================================================================

FFGeneratedShaders generate_ff_shaders(const FFPipelineState& state) {
    FFGeneratedShaders result;
    result.state_hash = state.hash();
    // An unsupported combination of the OpenGL fields fails closed: empty
    // shaders, which valid() reports.
    if (!ff_gl_fields_valid(state)) return result;
    result.vertex_msl = generate_vertex_shader(state);
    result.fragment_msl = generate_fragment_shader(state);
    return result;
}

namespace {

void append_sm3_varying_fields(std::ostringstream& ss, bool fragment_position,
                               bool flat_colors,
                               bool point_size_output,
                               const std::vector<u32>& color_varyings,
                               const std::vector<u32>& texcoord_varyings) {
    if (fragment_position) {
        ss << "    float4 frag_position [[position]];\n";
    } else {
        ss << "    float4 position [[position]];\n";
        if (point_size_output) {
            ss << "    float point_size [[point_size]];\n";
        }
    }
    for (u32 color : color_varyings) {
        ss << "    float4 color" << color << " [[user(color" << color
           << ")" << (flat_colors ? ", flat" : "") << "]];\n";
    }
    for (u32 texcoord : texcoord_varyings) {
        ss << "    float4 texcoord" << texcoord << " [[user(texcoord"
           << texcoord << ")]];\n";
    }
}

[[nodiscard]] bool has_varying(const std::vector<u32>& list, u32 index) {
    for (u32 value : list) {
        if (value == index) return true;
    }
    return false;
}

} // namespace

std::string generate_ff_fragment_for_sm3_varyings(
    const FFPipelineState& state,
    const std::vector<u32>& color_varyings,
    const std::vector<u32>& texcoord_varyings,
    bool use_vertex_fog_factor) {
    std::ostringstream ss;
    ss << "#include <metal_stdlib>\n"
       << "using namespace metal;\n\n";
    emit_srgb_helpers(ss, state);

    bool need_tex_matrices = false;
    const bool use_bump_env = ff_uses_bump_env(state);
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        if (state.texture_stages[i].texture_transform_flags != 0) {
            need_tex_matrices = true;
            break;
        }
    }

    // Layout must match pack_fragment_uniforms / pure-FF fragment uniforms.
    // With a programmable VS, D3D9 ignores D3DTS_TEXTUREn (VS owns UVs); we
    // still declare the matrices so the constant-buffer size matches, and
    // only honour D3DTTFF_PROJECTED divides at sample time.
    ss << "struct FFFragUniforms {\n"
       << "    float4 texture_factor;\n"
       << "    float4 stage_constants[8];\n"
       << "    float lod_biases[8];\n";
    if (state.fog_mode != FogMode::None) {
        ss << "    float4 fog_color;\n";
        if (state.table_fog) ss << "    float4 fog_params;\n";
    }
    if (state.alpha_test_enable) {
        ss << "    float alpha_ref;\n"
           << "    float _pad_alpha[3];\n";
    }
    if (need_tex_matrices) {
        ss << "    float4x4 texture_matrix[" << state.active_texture_stages
           << "];\n";
    }
    ss << "};\n\n";

    ss << "struct Sm3Varyings {\n";
    append_sm3_varying_fields(ss, true, state.flat_shading, false,
                              color_varyings, texcoord_varyings);
    if (use_vertex_fog_factor) {
        ss << "    float4 fog [[user(fog)]];\n";
    }
    ss << "};\n\n";

    ss << "fragment float4 ff_fragment(\n"
       << "    Sm3Varyings in [[stage_in]],\n"
       << "    constant FFFragUniforms& cb [[buffer(0)]]";
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        ss << ",\n    texture2d<float> tex" << i << " [[texture(" << i << ")]]"
           << ",\n    sampler samp" << i << " [[sampler(" << i << ")]]";
    }
    ss << ")\n{\n";

    if (has_varying(color_varyings, 0)) {
        // D3D9 diffuse/specular inputs to the pixel pipeline have unsigned
        // colour precision and are saturated to [0,1] before use.
        ss << "    float4 v_diffuse = clamp(in.color0, 0.0, 1.0);\n";
    } else {
        ss << "    float4 v_diffuse = float4(1.0);\n";
    }
    if (has_varying(color_varyings, 1)) {
        ss << "    float4 v_specular = clamp(in.color1, 0.0, 1.0);\n";
    } else {
        ss << "    float4 v_specular = float4(0.0);\n";
    }
    ss << "    float4 current = v_diffuse;\n"
       << "    float4 temp = float4(0.0);\n"
       << "    float4 tex_sample;\n";
    if (use_bump_env) {
        ss << "    float2 bump_offset = float2(0.0);\n"
           << "    float bump_luminance = 1.0;\n";
    }
    ss << "\n";

    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        const auto& stage = state.texture_stages[i];
        if (stage.color_op == TexStageOp::Disable) break;
        const bool premodulate_current =
            i > 0 && state.texture_stages[i - 1].color_op ==
                         TexStageOp::Premodulate;
        const bool apply_bump_offset =
            i > 0 && (state.texture_stages[i - 1].color_op ==
                          TexStageOp::BumpEnvMap ||
                      state.texture_stages[i - 1].color_op ==
                          TexStageOp::BumpEnvMapLuminance);
        const bool apply_bump_luminance =
            i > 0 && state.texture_stages[i - 1].color_op ==
                         TexStageOp::BumpEnvMapLuminance;

        ss << "    // Texture stage " << i << "\n";
        ss << "    {\n";
        ss << "    float4 stage_result = current;\n";
        const u32 tc = stage.texcoord_index & 0xFFFFu;
        u32 sample_tc = tc;
        if (!has_varying(texcoord_varyings, tc)) {
            sample_tc = texcoord_varyings.empty() ? ~0u
                                                 : texcoord_varyings.front();
        }
        if (sample_tc != ~0u) {
            // D3DTTFF_PROJECTED only — D3DTS_TEXTUREn is not applied when a
            // programmable VS is bound.
            const u32 ttf = stage.texture_transform_flags;
            const u32 count = ttf & 0xFFu;
            const bool projected = (ttf & 0x100u) != 0;
            if (projected) {
                ss << "    {\n"
                   << "      float4 tc = in.texcoord" << sample_tc << ";\n";
                ss << "      float2 uv = " << projected_uv_expr(count) << ";\n";
                if (apply_bump_offset) {
                    ss << "      uv += bump_offset;\n";
                }
                ss << "      tex_sample = tex" << i << ".sample(samp" << i
                   << ", uv, bias(cb.lod_biases[" << i << "]));\n"
                   << "    }\n";
            } else {
                if (apply_bump_offset) {
                    ss << "    {\n"
                       << "      float2 uv = in.texcoord" << sample_tc
                       << ".xy + bump_offset;\n"
                       << "      tex_sample = tex" << i << ".sample(samp" << i
                       << ", uv, bias(cb.lod_biases[" << i << "]));\n"
                       << "    }\n";
                } else {
                    ss << "    tex_sample = tex" << i << ".sample(samp" << i
                       << ", in.texcoord" << sample_tc
                       << ".xy, bias(cb.lod_biases[" << i << "]));\n";
                }
            }
        } else {
            ss << "    tex_sample = tex" << i << ".sample(samp" << i
               << ", float2(0.0)"
               << (apply_bump_offset ? " + bump_offset" : "")
               << ", bias(cb.lod_biases[" << i << "]));\n";
        }
        if (((state.srgb_texture_mask >> i) & 1u) != 0) {
            ss << "    tex_sample = vitreo_srgb_decode(tex_sample);\n";
        }
        if (stage.color_op == TexStageOp::BumpEnvMap ||
            stage.color_op == TexStageOp::BumpEnvMapLuminance) {
            emit_bump_offset(ss, stage);
        }

        ss << "    float4 arg0c = "
           << modified_arg_expr(stage.color_arg0, i, premodulate_current)
           << ";\n";
        ss << "    float4 arg1c = "
           << modified_arg_expr(stage.color_arg1, i, premodulate_current)
           << ";\n";
        ss << "    float4 arg2c = "
           << modified_arg_expr(stage.color_arg2, i, premodulate_current)
           << ";\n";
        emit_combiner_op(ss, "stage_result", ".rgb", stage.color_op,
                         "arg0c", "arg1c", "arg2c");
        if (apply_bump_luminance) {
            ss << "    stage_result.rgb = clamp(stage_result.rgb * bump_luminance, "
                  "0.0, 1.0);\n";
        }

        if (stage.alpha_op != TexStageOp::Disable) {
            ss << "    float4 arg0a = "
               << modified_arg_expr(stage.alpha_arg0, i, premodulate_current)
               << ";\n";
            ss << "    float4 arg1a = "
               << modified_arg_expr(stage.alpha_arg1, i, premodulate_current)
               << ";\n";
            ss << "    float4 arg2a = "
               << modified_arg_expr(stage.alpha_arg2, i, premodulate_current)
               << ";\n";
            emit_combiner_op(ss, "stage_result", ".a", stage.alpha_op,
                             "arg0a", "arg1a", "arg2a");
        }
        if (stage.color_op != TexStageOp::BumpEnvMap &&
            stage.color_op != TexStageOp::BumpEnvMapLuminance) {
            if ((stage.result_arg & 0xFu) ==
                static_cast<u32>(TexStageArg::Temp)) {
                ss << "    temp = stage_result;\n";
            } else {
                ss << "    current = stage_result;\n";
            }
        }
        ss << "    }\n\n";
    }

    if (state.specular_enable) {
        ss << "    current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);\n\n";
    }

    if (state.alpha_test_enable) {
        ss << "    // Alpha test\n";
        switch (state.alpha_test_func) {
            case 1:
                ss << "    discard_fragment();\n";
                break;
            case 2:
                ss << "    if (current.a >= cb.alpha_ref) discard_fragment();\n";
                break;
            case 3:
                ss << "    if (abs(current.a - cb.alpha_ref) > 0.004) discard_fragment();\n";
                break;
            case 4:
                ss << "    if (current.a > cb.alpha_ref) discard_fragment();\n";
                break;
            case 5:
                ss << "    if (current.a <= cb.alpha_ref) discard_fragment();\n";
                break;
            case 6:
                ss << "    if (abs(current.a - cb.alpha_ref) <= 0.004) discard_fragment();\n";
                break;
            case 7:
                ss << "    if (current.a < cb.alpha_ref) discard_fragment();\n";
                break;
            default:
                break;
        }
        ss << "\n";
    }

    if (state.table_fog) {
        emit_table_fog(ss, state, "in.position");
    } else if (state.fog_mode != FogMode::None && use_vertex_fog_factor) {
        ss << "    float fog_factor = clamp(in.fog.x, 0.0, 1.0);\n"
           << "    current.rgb = mix(cb.fog_color.rgb, current.rgb, fog_factor);\n\n";
    }

    if (state.srgb_write_enable) {
        ss << "    current.rgb = vitreo_srgb_encode(current.rgb);\n";
    }
    ss << "    return current;\n}\n";
    return ss.str();
}

std::string generate_ff_vertex_for_sm3_varyings(
    const FFPipelineState& state,
    const std::vector<u32>& color_varyings,
    const std::vector<u32>& texcoord_varyings) {
    // Reuse the native FF vertex body by generating it, then wrap is too
    // heavy; emit a parallel shader that writes Sm3Varyings instead.
    std::ostringstream ss;
    ss << "#include <metal_stdlib>\n"
       << "using namespace metal;\n\n";

    const bool pretransformed =
        (state.fvf_flags & fvf::POSITION_MASK) == fvf::XYZRHW;
    const bool vertex_blending =
        state.vertex_blend_weight_count != 0 || state.indexed_vertex_blend;

    ss << "struct FFUniforms {\n"
       << "    float4x4 world;\n"
       << "    float4x4 view;\n"
       << "    float4x4 projection;\n"
       << "    float4x4 world_view_proj;\n"
       << "    float4x4 world_inv_transpose;\n";
    if (state.indexed_vertex_blend) {
        ss << "    float4x4 blend_worlds[9];\n"
           << "    float4x4 blend_world_inv_transposes[9];\n";
    } else if (state.vertex_blend_weight_count != 0) {
        for (u32 i = 1; i <= state.vertex_blend_weight_count; ++i) {
            ss << "    float4x4 world" << i << ";\n"
               << "    float4x4 world" << i << "_inv_transpose;\n";
        }
    }
    if (state.vertex_tweening) {
        ss << "    float4 tween_params;\n";
    }
    if (pretransformed) {
        ss << "    float4 viewport_params;\n";
    }
    if (state.point_size_output) {
        ss << "    float4 point_params;\n";
        if (state.point_scale_enabled && !pretransformed) {
            ss << "    float4 point_scale_params;\n";
        }
    }
    if (state.clip_planes_enabled && !pretransformed) {
        ss << "    float4 clip_planes[6];\n";
    }
    if (state.lighting_enabled && state.active_light_count > 0) {
        ss << "    float4 global_ambient;\n"
           << "    float4 mat_diffuse;\n"
           << "    float4 mat_ambient;\n"
           << "    float4 mat_specular;\n"
           << "    float4 mat_emissive;\n"
           << "    float  mat_power;\n"
           << "    float  _pad0[3];\n";
        for (u32 i = 0; i < state.active_light_count; ++i) {
            ss << "    float4 light" << i << "_diffuse;\n"
               << "    float4 light" << i << "_specular;\n"
               << "    float4 light" << i << "_ambient;\n"
               << "    float4 light" << i << "_position;\n"
               << "    float4 light" << i << "_direction;\n"
               << "    float4 light" << i << "_atten;\n";
        }
    }
    ss << "};\n\n";

    ss << "struct FFVertexIn {\n";
    if (pretransformed) {
        ss << "    float4 position [[attribute(0)]];\n";
    } else {
        ss << "    float3 position [[attribute(0)]];\n";
    }
    u32 attr_idx = 1;
    if (state.vertex_tweening) {
        ss << "    float3 position1 [[attribute(" << attr_idx++ << ")]];\n";
    }
    const u32 fvf_position = state.fvf_flags & fvf::POSITION_MASK;
    const bool packed_blend_indices =
        (state.fvf_flags & fvf::LASTBETA_UBYTE4) != 0;
    const u32 beta_slots =
        fvf_position >= fvf::XYZB1 && fvf_position <= fvf::XYZB5
            ? (fvf_position - fvf::XYZB1) / 2u + 1u : 0u;
    const u32 blend_weight_slots =
        beta_slots - (packed_blend_indices && beta_slots != 0 ? 1u : 0u);
    if (blend_weight_slots != 0) {
        ss << "    float" << (blend_weight_slots == 1 ? "" : std::to_string(blend_weight_slots))
           << " blend_weights [[attribute(" << attr_idx++ << ")]];\n";
    }
    if (packed_blend_indices) {
        ss << "    uchar4 blend_indices [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_normal = (state.fvf_flags & fvf::NORMAL) != 0;
    if (has_normal) {
        ss << "    float3 normal [[attribute(" << attr_idx++ << ")]];\n";
        if (state.vertex_tweening) {
            ss << "    float3 normal1 [[attribute(" << attr_idx++ << ")]];\n";
        }
    }
    const bool has_point_size = (state.fvf_flags & fvf::PSIZE) != 0;
    if (has_point_size) {
        ss << "    float point_size [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_diffuse = (state.fvf_flags & fvf::DIFFUSE) != 0;
    if (has_diffuse) {
        ss << "    float4 color [[attribute(" << attr_idx++ << ")]];\n";
    }
    const bool has_specular = (state.fvf_flags & fvf::SPECULAR) != 0;
    if (has_specular) {
        ss << "    float4 specular_color [[attribute(" << attr_idx++ << ")]];\n";
    }
    const u32 tex_count = state.texcoord_count();
    for (u32 i = 0; i < tex_count; ++i) {
        ss << "    float2 texcoord" << i << " [[attribute(" << attr_idx++
           << ")]];\n";
    }
    ss << "};\n\n";

    ss << "struct Sm3Varyings {\n";
    append_sm3_varying_fields(ss, false, state.flat_shading,
                              state.point_size_output, color_varyings,
                              texcoord_varyings);
    if (state.clip_planes_enabled && !pretransformed) {
        ss << "    float clip_distances [[clip_distance]] [6];\n";
    }
    ss << "};\n\n";

    ss << "vertex Sm3Varyings ff_vertex(\n"
       << "    FFVertexIn in [[stage_in]],\n"
       << "    constant FFUniforms& cb [[buffer(0)]])\n{\n"
       << "    Sm3Varyings out;\n";
    if (state.vertex_tweening) {
        ss << "    float3 tween_position = mix(in.position, in.position1, "
              "cb.tween_params.x);\n";
        if (has_normal) {
            ss << "    float3 tween_normal = mix(in.normal, in.normal1, "
                  "cb.tween_params.x);\n";
        }
    }
    if (vertex_blending) {
        static constexpr const char* components[] = {"x", "y", "z", "w"};
        for (u32 i = 0; i < state.vertex_blend_weight_count; ++i) {
            ss << "    float blend_weight" << i << " = in.blend_weights";
            if (blend_weight_slots != 1) ss << "." << components[i];
            ss << ";\n";
        }
        ss << "    float blend_weight" << state.vertex_blend_weight_count
           << " = 1.0";
        for (u32 i = 0; i < state.vertex_blend_weight_count; ++i) {
            ss << " - blend_weight" << i;
        }
        ss << ";\n"
           << "    float4 object_position = float4("
           << (state.vertex_tweening ? "tween_position" : "in.position")
           << ", 1.0);\n"
           << "    float4 blended_world_position = ";
        for (u32 i = 0; i <= state.vertex_blend_weight_count; ++i) {
            if (i != 0) ss << " + ";
            ss << "blend_weight" << i << " * (object_position * ";
            if (state.indexed_vertex_blend) {
                ss << "cb.blend_worlds[uint(in.blend_indices."
                   << components[i] << ")]";
            } else {
                ss << "cb.world";
                if (i != 0) ss << i;
            }
            ss << ")";
        }
        ss << ";\n";
    }
    if (state.point_size_output) {
        if (state.point_scale_enabled && !pretransformed) {
            ss << "    float3 point_eye = "
               << (vertex_blending
                       ? "(blended_world_position * cb.view).xyz;\n"
                       : (state.vertex_tweening
                              ? "(float4(tween_position, 1.0) * cb.world * cb.view).xyz;\n"
                              : "(float4(in.position, 1.0) * cb.world * cb.view).xyz;\n"))
               << "    float point_distance = length(point_eye);\n"
               << "    float point_denominator = cb.point_scale_params.x + "
                  "cb.point_scale_params.y * point_distance + "
                  "cb.point_scale_params.z * point_distance * point_distance;\n"
               << "    float scaled_point_size = "
               << (has_point_size ? "in.point_size" : "cb.point_params.x")
               << " * "
                  "cb.point_scale_params.w * rsqrt(max(point_denominator, "
                  "1.0e-20));\n"
               << "    out.point_size = clamp(scaled_point_size, "
                  "cb.point_params.y, cb.point_params.z);\n";
        } else {
            if (has_point_size) {
                ss << "    out.point_size = clamp(in.point_size, "
                      "cb.point_params.y, cb.point_params.z);\n";
            } else {
                ss << "    out.point_size = cb.point_params.x;\n";
            }
        }
    }
    if (state.clip_planes_enabled && !pretransformed) {
        ss << "    float4 clip_world_position = "
           << (vertex_blending
                   ? "blended_world_position;\n"
                   : (state.vertex_tweening
                          ? "float4(tween_position, 1.0) * cb.world;\n"
                          : "float4(in.position, 1.0) * cb.world;\n"))
           << "    for (uint clip_index = 0; clip_index < 6; ++clip_index) {\n"
           << "        out.clip_distances[clip_index] = "
              "dot(cb.clip_planes[clip_index], clip_world_position);\n"
           << "    }\n";
    }

    if (pretransformed) {
        ss << "    float rhw = (in.position.w != 0.0) ? in.position.w : 1.0;\n"
           << "    float clip_w = 1.0 / rhw;\n"
           << "    float2 screen = in.position.xy + float2(0.5, 0.5);\n"
           << "    float2 ndc = float2(\n"
           << "        screen.x / cb.viewport_params.x * 2.0 - 1.0,\n"
           << "        1.0 - screen.y / cb.viewport_params.y * 2.0);\n"
           << "    out.position = float4(ndc * clip_w, in.position.z * clip_w,"
           << " clip_w);\n";
    } else {
        if (vertex_blending) {
            ss << "    out.position = blended_world_position * cb.view * "
                  "cb.projection;\n";
        } else {
            ss << "    out.position = float4("
               << (state.vertex_tweening ? "tween_position" : "in.position")
               << ", 1.0) * cb.world_view_proj;\n";
        }
    }

    ss << "    float4 diffuse = "
       << (has_diffuse ? "in.color" : "float4(1.0)") << ";\n"
       << "    float4 specular = "
       << (has_specular ? "in.specular_color" : "float4(0.0)") << ";\n";

    for (u32 color : color_varyings) {
        if (color == 0) {
            ss << "    out.color0 = diffuse;\n";
        } else if (color == 1) {
            ss << "    out.color1 = specular;\n";
        } else {
            ss << "    out.color" << color << " = float4(0.0);\n";
        }
    }
    for (u32 texcoord : texcoord_varyings) {
        if (texcoord < tex_count) {
            ss << "    out.texcoord" << texcoord << " = float4(in.texcoord"
               << texcoord << ", 0.0, 1.0);\n";
        } else {
            ss << "    out.texcoord" << texcoord << " = float4(0.0, 0.0, 0.0, 1.0);\n";
        }
    }

    ss << "    return out;\n}\n";
    return ss.str();
}

} // namespace vitreo::graphics
