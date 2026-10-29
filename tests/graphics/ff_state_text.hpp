// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ff_state_text.hpp
/// @brief A by-name text form of the Direct3D fields of `FFPipelineState`,
///        for the D3D golden corpus (test_ff_emulation_d3d_golden.cpp).
///
/// Only the fields that existed before the GL extensions (Task 8 of the
/// OpenGL shared-backend plan) are listed: every field added for GL defaults
/// to the Direct3D behaviour, so a golden entry parsed into a
/// default-constructed state is the Direct3D state it was recorded from.
/// Floats are written as their IEEE-754 bits so a round trip is exact.
/// Unknown keys fail the parse, so a fixture cannot silently drop a field.

#include "graphics/shaders/ff_emulation.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::graphics::ff_text {

/// Visits every Direct3D field of @p s with a stable name.
template <typename State, typename V>
void visit_state(State& s, V&& v) {
    v.u32_("fvf", s.fvf_flags);
    v.b("flat", s.flat_shading);
    v.b("psize_out", s.point_size_output);
    v.b("pscale", s.point_scale_enabled);
    v.b("clip", s.clip_planes_enabled);
    v.u32_("blend_n", s.vertex_blend_weight_count);
    v.b("blend_idx", s.indexed_vertex_blend);
    v.b("tween", s.vertex_tweening);
    v.f("tween_f", &s.tween_factor, 1);
    v.b("light", s.lighting_enabled);
    v.b("normalize", s.normalize_normals);
    v.b("local_viewer", s.local_viewer);
    v.u32_("lights_n", s.active_light_count);
    for (usize i = 0; i < s.lights.size(); ++i) {
        auto& l = s.lights[i];
        const std::string p = "l" + std::to_string(i) + ".";
        v.e8(p + "type", l.type);
        v.b(p + "on", l.enabled);
        v.f(p + "diffuse", l.diffuse, 4);
        v.f(p + "specular", l.specular, 4);
        v.f(p + "ambient", l.ambient, 4);
        v.f(p + "position", l.position, 3);
        v.f(p + "direction", l.direction, 3);
        v.f(p + "range", &l.range, 1);
        v.f(p + "atten", l.attenuation, 3);
        v.f(p + "falloff", &l.spot_falloff, 1);
        v.f(p + "theta", &l.spot_theta, 1);
        v.f(p + "phi", &l.spot_phi, 1);
    }
    v.f("m.diffuse", s.material.diffuse, 4);
    v.f("m.ambient", s.material.ambient, 4);
    v.f("m.specular", s.material.specular, 4);
    v.f("m.emissive", s.material.emissive, 4);
    v.f("m.power", &s.material.power, 1);
    v.f("global_ambient", s.global_ambient, 4);
    for (usize i = 0; i < s.texture_stages.size(); ++i) {
        auto& t = s.texture_stages[i];
        const std::string p = "t" + std::to_string(i) + ".";
        v.e8(p + "cop", t.color_op);
        v.u32_(p + "ca0", t.color_arg0);
        v.u32_(p + "ca1", t.color_arg1);
        v.u32_(p + "ca2", t.color_arg2);
        v.e8(p + "aop", t.alpha_op);
        v.u32_(p + "aa0", t.alpha_arg0);
        v.u32_(p + "aa1", t.alpha_arg1);
        v.u32_(p + "aa2", t.alpha_arg2);
        v.u32_(p + "res", t.result_arg);
        v.u32_(p + "tci", t.texcoord_index);
        v.u32_(p + "ttf", t.texture_transform_flags);
        v.u32_(p + "b00", t.bump_env_mat00);
        v.u32_(p + "b01", t.bump_env_mat01);
        v.u32_(p + "b10", t.bump_env_mat10);
        v.u32_(p + "b11", t.bump_env_mat11);
        v.u32_(p + "bls", t.bump_env_lscale);
        v.u32_(p + "blo", t.bump_env_loffset);
    }
    v.u32_("stages_n", s.active_texture_stages);
    v.e8("fog", s.fog_mode);
    v.b("table_fog", s.table_fog);
    v.b("table_fog_w", s.table_fog_w);
    v.b("range_fog", s.range_fog_enable);
    v.f("fog_start", &s.fog_start, 1);
    v.f("fog_end", &s.fog_end, 1);
    v.f("fog_density", &s.fog_density, 1);
    v.f("fog_color", s.fog_color, 4);
    v.b("specular", s.specular_enable);
    v.b("vcolor", s.vertex_color_enable);
    v.u32_("src_d", s.diffuse_material_source);
    v.u32_("src_a", s.ambient_material_source);
    v.u32_("src_s", s.specular_material_source);
    v.u32_("src_e", s.emissive_material_source);
    v.b("atest", s.alpha_test_enable);
    v.u32_("afunc", s.alpha_test_func);
    v.u32_("aref", s.alpha_ref);
    v.u32_("srgb_tex", s.srgb_texture_mask);
    v.b("srgb_write", s.srgb_write_enable);
}

/// Writes only the fields that differ from a default-constructed state, so
/// entries stay short and readable.
struct Writer {
    const FFPipelineState& defaults;
    const FFPipelineState& actual;
    std::ostringstream out;

    template <typename T>
    static const T& peer(const FFPipelineState& from, const FFPipelineState& base, const T& field) {
        const auto offset = reinterpret_cast<const u8*>(&field) - reinterpret_cast<const u8*>(&base);
        return *reinterpret_cast<const T*>(reinterpret_cast<const u8*>(&from) + offset);
    }
    void u32_(const std::string& n, const u32& x) {
        if (peer(defaults, actual, x) != x) out << ' ' << n << '=' << x;
    }
    void b(const std::string& n, const bool& x) {
        if (peer(defaults, actual, x) != x) out << ' ' << n << '=' << (x ? 1 : 0);
    }
    template <typename E>
    void e8(const std::string& n, const E& x) {
        if (peer(defaults, actual, x) != x) out << ' ' << n << '=' << static_cast<unsigned>(x);
    }
    void f(const std::string& n, const float* x, int count) {
        if (std::memcmp(&peer(defaults, actual, *x), x, sizeof(float) * count) == 0) return;
        out << ' ' << n << '=';
        for (int i = 0; i < count; ++i) {
            u32 bits;
            std::memcpy(&bits, &x[i], 4);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s%08x", i ? "," : "", bits);
            out << buf;
        }
    }
};

[[nodiscard]] inline std::string write_state(const FFPipelineState& s) {
    const FFPipelineState defaults{};
    Writer w{defaults, s, {}};
    visit_state(s, w);
    return w.out.str();
}

struct Reader {
    std::map<std::string, std::string>& kv;
    void u32_(const std::string& n, u32& x) {
        if (auto it = kv.find(n); it != kv.end()) {
            x = static_cast<u32>(std::stoul(it->second));
            kv.erase(it);
        }
    }
    void b(const std::string& n, bool& x) {
        if (auto it = kv.find(n); it != kv.end()) {
            x = it->second == "1";
            kv.erase(it);
        }
    }
    template <typename E>
    void e8(const std::string& n, E& x) {
        if (auto it = kv.find(n); it != kv.end()) {
            x = static_cast<E>(std::stoul(it->second));
            kv.erase(it);
        }
    }
    void f(const std::string& n, float* x, int count) {
        if (auto it = kv.find(n); it != kv.end()) {
            std::stringstream ss(it->second);
            std::string item;
            for (int i = 0; i < count && std::getline(ss, item, ','); ++i) {
                const u32 bits = static_cast<u32>(std::stoul(item, nullptr, 16));
                std::memcpy(&x[i], &bits, 4);
            }
            kv.erase(it);
        }
    }
};

/// Parses `key=value` tokens into @p s (starting from defaults). Returns
/// false when a key is unknown.
[[nodiscard]] inline bool read_state(const std::string& text, FFPipelineState& s, std::string& unknown) {
    std::map<std::string, std::string> kv;
    std::stringstream ss(text);
    std::string token;
    while (ss >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            unknown = token;
            return false;
        }
        kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
    s = FFPipelineState{};
    Reader r{kv};
    visit_state(s, r);
    if (!kv.empty()) {
        unknown = kv.begin()->first;
        return false;
    }
    return true;
}

}  // namespace vitreo::graphics::ff_text
