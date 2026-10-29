// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ff_emulation_d3d_golden.cpp
/// @brief The Direct3D output of the shared fixed-function generator is pinned
///        byte for byte.
///
/// The generator (ff_emulation / ff_uniforms) is shared by D3D7, D3D9 and
/// OpenGL. GL-only features are new FFPipelineState fields whose defaults are
/// the Direct3D behaviour, so a Direct3D state must keep producing exactly
/// the MSL, state hash and uniform bytes it produced before them.
///
/// The fixture tests/fixtures/ff_d3d_golden.txt was recorded from the
/// generator as it was before the GL extensions (commit 53fa4cae). It holds
/// two corpora, tagged per line:
///   - `captured`: every distinct state that the D3D9/D3D7/generator test
///     suites (vitreo_graphics_metal_tests, vitreo_graphics_tests,
///     vitreo_ddraw_metal_tests D3D7DrawTest, vitreo_winapi_tests
///     D3D9*:DirectDraw*:Direct3D*) passed to generate_ff_shaders and the two
///     SM3-hybrid generators, captured by a temporary hook;
///   - `derived`: `derived_corpus()` below, every existing field toggled
///     across its values on three base states, so every existing generator
///     branch is covered and not only those the tests happen to build.
/// Each line stores the state (ff_state_text.hpp), the state hash, and a
/// length + two independent 64-bit digests of the vertex MSL, the fragment
/// MSL, and the packed vertex and fragment uniform bytes for a fixed,
/// non-trivial FFUniformInputs.
///
/// Re-recording (only when Direct3D output is changed deliberately, e.g. the
/// matrix-convention fix of Task 8b):
///   VITREO_FF_GOLDEN_RECORD=<capture file or -> vitreo_graphics_tests
///       --gtest_filter=FFEmulationD3DGolden.*
/// rewrites the fixture from the capture lines in that file
/// (`kind|colors|texcoords|fog|state`, the hook's format) plus the derived
/// corpus. With `-` it keeps the fixture's own captured entries.
///
/// Proving a re-record touches only what it should: set
/// VITREO_FF_GOLDEN_DUMP=<file> to dump every entry's full outputs, once with
/// the generator sources (src/graphics/shaders/ff_*) of the base commit
/// checked out over the current tree and once with the change, then run
///   python3 tests/tools/ff_golden_proof.py <base dump> <new dump>
/// which attributes every changed MSL line and uniform member to a listed
/// fixed site and exits non-zero on any it cannot attribute.

#include <gtest/gtest.h>

#include "ff_state_text.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

const std::string kFixture = std::string(VITREO_SOURCE_DIR) + "/tests/fixtures/ff_d3d_golden.txt";

std::string digest(const void* data, usize len) {
    const auto* p = static_cast<const u8*>(data);
    u64 fnv = 14695981039346656037ULL;
    u64 djb = 5381;
    for (usize i = 0; i < len; ++i) {
        fnv = (fnv ^ p[i]) * 1099511628211ULL;
        djb = djb * 33 + p[i];
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu:%016llx:%016llx", static_cast<size_t>(len),
                  static_cast<unsigned long long>(fnv), static_cast<unsigned long long>(djb));
    return buf;
}
std::string digest(const std::string& s) { return digest(s.data(), s.size()); }
std::string digest(const std::vector<u8>& v) { return digest(v.data(), v.size()); }

std::vector<u32> parse_list(const std::string& s) {
    std::vector<u32> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(static_cast<u32>(std::stoul(item)));
    }
    return out;
}
std::string list_text(const std::vector<u32>& v) {
    std::string s;
    for (u32 x : v) s += std::to_string(x) + ",";
    return s;
}

/// Fixed uniform inputs with distinct, invertible values everywhere.
FFUniformInputs golden_inputs() {
    FFUniformInputs in;
    for (u32 m = 0; m < FF_MAX_WORLD_MATRICES; ++m) {
        FFMatrix w = FF_IDENTITY_MATRIX;
        w[0] = 1.0f + 0.1f * m;
        w[1] = 0.05f * m;
        w[5] = 0.9f;
        w[10] = 1.1f;
        w[12] = 0.5f + m;
        w[13] = -0.25f * m;
        w[14] = 2.0f;
        in.world_matrices[m] = w;
    }
    in.view = {0.8f, 0.0f, -0.6f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.6f, 0.0f, 0.8f, 0.0f, 1.0f, -2.0f, 5.0f, 1.0f};
    in.projection = {1.5f, 0, 0, 0, 0, 2.0f, 0, 0, 0, 0, 1.01f, 1.0f, 0, 0, -0.101f, 0};
    for (u32 t = 0; t < FF_MAX_TEXTURE_STAGES; ++t) {
        FFMatrix m = FF_IDENTITY_MATRIX;
        m[0] = 0.5f + t;
        m[4] = 0.25f;
        m[13] = 0.125f * t;
        in.texture_transforms[t] = m;
        in.stage_constants[t] = {0.1f * t, 0.2f, 0.3f, 0.4f + 0.05f * t};
        in.mipmap_lod_bias[t] = -0.5f + 0.25f * t;
    }
    in.viewport_width = 640.0f;
    in.viewport_height = 480.0f;
    in.point_size = 3.0f;
    in.point_size_min = 0.5f;
    in.point_size_max = 300.0f;
    in.point_scale_a = 0.5f;
    in.point_scale_b = 0.25f;
    in.point_scale_c = -1.0f;
    in.clip_plane_enable_mask = 0x2D;
    for (u32 i = 0; i < FF_MAX_CLIP_PLANES; ++i) in.clip_planes[i] = {1.0f * i, -0.5f, 0.25f, 3.0f};
    in.texture_factor = {0.9f, 0.8f, 0.7f, 0.6f};
    return in;
}

struct Entry {
    std::string source;  // captured / derived
    std::string kind;    // full / sm3frag / sm3vert
    std::vector<u32> colors, texcoords;
    bool fog = false;
    FFPipelineState state;
    std::string state_text;
};

/// The recorded columns for one entry.
std::vector<std::string> outputs(const Entry& e) {
    std::vector<std::string> out;
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(e.state.hash()));
    out.push_back(hash);
    if (e.kind == "full") {
        const FFGeneratedShaders g = generate_ff_shaders(e.state);
        char gh[32];
        std::snprintf(gh, sizeof(gh), "%016llx", static_cast<unsigned long long>(g.state_hash));
        out.push_back(gh);
        out.push_back(digest(g.vertex_msl));
        out.push_back(digest(g.fragment_msl));
        const FFUniformInputs inputs = golden_inputs();
        std::vector<u8> vs, fs;
        pack_ff_vertex_uniforms(e.state, inputs, vs);
        pack_ff_fragment_uniforms(e.state, inputs, fs);
        out.push_back(digest(vs));
        out.push_back(digest(fs));
    } else if (e.kind == "sm3frag") {
        out.push_back(digest(generate_ff_fragment_for_sm3_varyings(e.state, e.colors, e.texcoords, e.fog)));
        std::vector<u8> fs;
        pack_ff_fragment_uniforms(e.state, golden_inputs(), fs);
        out.push_back(digest(fs));
    } else {
        out.push_back(digest(generate_ff_vertex_for_sm3_varyings(e.state, e.colors, e.texcoords)));
    }
    return out;
}

// ---- The derived corpus --------------------------------------------------------------

FFPipelineState base_unlit() {
    FFPipelineState s{};
    s.fvf_flags = fvf::XYZ | fvf::DIFFUSE | fvf::TEX1;
    s.lighting_enabled = false;
    s.active_texture_stages = 1;
    s.texture_stages[0].color_op = TexStageOp::Modulate;
    s.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    return s;
}

FFPipelineState base_lit() {
    FFPipelineState s{};
    s.fvf_flags = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE | fvf::SPECULAR | fvf::TEX2;
    s.lighting_enabled = true;
    s.specular_enable = true;
    s.active_light_count = 3;
    s.lights[0].type = LightType::Directional;
    s.lights[1].type = LightType::Point;
    s.lights[2].type = LightType::Spot;
    for (u32 i = 0; i < 8; ++i) {
        FFLight& l = s.lights[i];
        l.enabled = true;
        l.diffuse[0] = 0.1f * i;
        l.specular[1] = 0.5f;
        l.ambient[2] = 0.25f;
        l.position[0] = 1.0f + i;
        l.direction[1] = -1.0f;
        l.range = 100.0f;
        l.attenuation[1] = 0.1f;
        l.spot_falloff = 1.0f;
        l.spot_theta = 0.3f;
        l.spot_phi = 0.6f;
    }
    s.material.diffuse[0] = 0.7f;
    s.material.specular[2] = 0.3f;
    s.material.emissive[1] = 0.05f;
    s.material.power = 12.0f;
    s.global_ambient[0] = 0.2f;
    s.active_texture_stages = 2;
    s.texture_stages[0].color_op = TexStageOp::Modulate;
    s.texture_stages[0].alpha_op = TexStageOp::Modulate;
    s.texture_stages[1].color_op = TexStageOp::Add;
    s.texture_stages[1].texcoord_index = 1;
    s.fog_start = 2.0f;
    s.fog_end = 50.0f;
    s.fog_density = 0.3f;
    s.fog_color[0] = 0.5f;
    s.alpha_ref = 0x80;
    s.tween_factor = 0.25f;
    return s;
}

FFPipelineState base_pretransformed() {
    FFPipelineState s{};
    s.fvf_flags = fvf::XYZRHW | fvf::DIFFUSE | fvf::SPECULAR | fvf::TEX1;
    s.lighting_enabled = false;
    s.active_texture_stages = 1;
    s.texture_stages[0].color_op = TexStageOp::SelectArg1;
    return s;
}

std::vector<FFPipelineState> derived_states() {
    std::vector<FFPipelineState> out;
    const FFPipelineState bases[] = {base_unlit(), base_lit(), base_pretransformed()};
    for (const FFPipelineState& base : bases) {
        out.push_back(base);
        auto add = [&](auto&& mutate) {
            FFPipelineState s = base;
            mutate(s);
            out.push_back(s);
        };
        // Position formats, blend weights and packed indices.
        const u32 positions[] = {fvf::XYZ, fvf::XYZRHW, fvf::XYZB1, fvf::XYZB2, fvf::XYZB3, fvf::XYZB4, fvf::XYZB5};
        for (u32 p : positions) {
            add([&](FFPipelineState& s) { s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | p; });
            if (p >= fvf::XYZB2 && p <= fvf::XYZB5) {
                add([&](FFPipelineState& s) {
                    s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | p | fvf::LASTBETA_UBYTE4;
                });
            }
        }
        for (u32 bit : {fvf::NORMAL, fvf::PSIZE, fvf::DIFFUSE, fvf::SPECULAR}) {
            add([&](FFPipelineState& s) { s.fvf_flags ^= bit; });
        }
        for (u32 n = 0; n <= 8; ++n) {
            add([&](FFPipelineState& s) {
                s.fvf_flags = (s.fvf_flags & ~fvf::TEXCOUNT_MASK) | (n << fvf::TEXCOUNT_SHIFT);
            });
        }
        for (u32 set = 0; set < 2; ++set) {
            for (u32 code = 1; code <= 3; ++code) {
                add([&](FFPipelineState& s) {
                    s.fvf_flags = (s.fvf_flags & ~fvf::TEXCOUNT_MASK) | fvf::TEX2;
                    s.fvf_flags |= code << (16u + set * 2u);
                });
            }
        }
        // Booleans.
        add([](FFPipelineState& s) { s.flat_shading = !s.flat_shading; });
        add([](FFPipelineState& s) { s.point_size_output = true; });
        add([](FFPipelineState& s) { s.point_size_output = true; s.fvf_flags |= fvf::PSIZE; });
        add([](FFPipelineState& s) { s.point_size_output = true; s.point_scale_enabled = true; });
        add([](FFPipelineState& s) {
            s.point_size_output = true;
            s.point_scale_enabled = true;
            s.fvf_flags |= fvf::PSIZE;
        });
        add([](FFPipelineState& s) { s.clip_planes_enabled = true; });
        add([](FFPipelineState& s) { s.vertex_tweening = true; });
        add([](FFPipelineState& s) { s.vertex_tweening = true; s.fvf_flags |= fvf::NORMAL; });
        add([](FFPipelineState& s) { s.lighting_enabled = !s.lighting_enabled; });
        add([](FFPipelineState& s) { s.normalize_normals = !s.normalize_normals; });
        add([](FFPipelineState& s) { s.local_viewer = !s.local_viewer; });
        add([](FFPipelineState& s) { s.specular_enable = !s.specular_enable; });
        add([](FFPipelineState& s) { s.vertex_color_enable = !s.vertex_color_enable; });
        add([](FFPipelineState& s) { s.srgb_write_enable = !s.srgb_write_enable; });
        // Vertex blending (with and without palette indices, tweening, clip, TCI).
        for (u32 n = 0; n <= 3; ++n) {
            add([&](FFPipelineState& s) {
                s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB3;
                s.vertex_blend_weight_count = n;
            });
            add([&](FFPipelineState& s) {
                s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB4 | fvf::LASTBETA_UBYTE4 | fvf::NORMAL;
                s.vertex_blend_weight_count = n;
                s.indexed_vertex_blend = true;
                s.clip_planes_enabled = true;
                s.point_size_output = true;
                s.point_scale_enabled = true;
                s.fog_mode = FogMode::Linear;
                s.texture_stages[0].texcoord_index = 0x30000u;
            });
            add([&](FFPipelineState& s) {
                s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB3 | fvf::NORMAL;
                s.vertex_blend_weight_count = n;
                s.texture_stages[0].texcoord_index = 0x10000u;
            });
        }
        // Lights.
        for (u32 n = 0; n <= 8; ++n) {
            add([&](FFPipelineState& s) {
                s.active_light_count = n;
                s.fvf_flags |= fvf::NORMAL;
                s.lighting_enabled = true;
                for (u32 i = 0; i < 8; ++i) s.lights[i].type = static_cast<LightType>(1 + (i + n) % 3);
            });
        }
        for (u8 t = 1; t <= 3; ++t) {
            add([&](FFPipelineState& s) {
                s.active_light_count = 1;
                s.fvf_flags |= fvf::NORMAL;
                s.lighting_enabled = true;
                s.lights[0].type = static_cast<LightType>(t);
            });
            add([&](FFPipelineState& s) {
                s.active_light_count = 1;
                s.fvf_flags |= fvf::NORMAL;
                s.lighting_enabled = true;
                s.lights[0].type = static_cast<LightType>(t);
                s.vertex_tweening = true;
                s.local_viewer = false;
            });
        }
        // Material sources, with and without COLORVERTEX.
        for (u32 which = 0; which < 4; ++which) {
            for (u32 src = 0; src <= 2; ++src) {
                for (bool vc : {true, false}) {
                    add([&](FFPipelineState& s) {
                        s.vertex_color_enable = vc;
                        u32* f[] = {&s.diffuse_material_source, &s.ambient_material_source,
                                    &s.specular_material_source, &s.emissive_material_source};
                        *f[which] = src;
                    });
                }
            }
        }
        // Texture stages: count, every op on colour and alpha, args, results.
        for (u32 n = 0; n <= 8; ++n) {
            add([&](FFPipelineState& s) {
                s.active_texture_stages = n;
                for (u32 i = 0; i < 8; ++i) {
                    s.texture_stages[i].color_op = i < n ? TexStageOp::Modulate : TexStageOp::Disable;
                }
            });
        }
        for (u8 op = 1; op <= 26; ++op) {
            for (u32 stage = 0; stage < 2; ++stage) {
                add([&](FFPipelineState& s) {
                    s.active_texture_stages = 3;
                    s.texture_stages[1].color_op = TexStageOp::Modulate;
                    s.texture_stages[2].color_op = TexStageOp::SelectArg1;
                    s.texture_stages[stage].color_op = static_cast<TexStageOp>(op);
                    s.texture_stages[stage].bump_env_mat00 = 0x3f000000u;
                    s.texture_stages[stage].bump_env_mat11 = 0xbf000000u;
                    s.texture_stages[stage].bump_env_lscale = 0x3f800000u;
                    s.texture_stages[stage].bump_env_loffset = 0x3e800000u;
                });
            }
            add([&](FFPipelineState& s) { s.texture_stages[0].alpha_op = static_cast<TexStageOp>(op); });
        }
        for (u32 src = 0; src <= 6; ++src) {
            for (u32 mod : {0x00u, 0x10u, 0x20u, 0x30u}) {
                add([&](FFPipelineState& s) {
                    s.texture_stages[0].color_op = TexStageOp::MultiplyAdd;
                    s.texture_stages[0].color_arg1 = src | mod;
                });
                add([&](FFPipelineState& s) {
                    s.texture_stages[0].color_op = TexStageOp::Lerp;
                    s.texture_stages[0].color_arg2 = src | mod;
                    s.texture_stages[0].color_arg0 = (6 - src) | mod;
                });
                add([&](FFPipelineState& s) {
                    s.texture_stages[0].alpha_op = TexStageOp::MultiplyAdd;
                    s.texture_stages[0].alpha_arg0 = src | mod;
                    s.texture_stages[0].alpha_arg1 = (src + 1) % 7;
                    s.texture_stages[0].alpha_arg2 = src;
                });
            }
        }
        add([](FFPipelineState& s) { s.texture_stages[0].result_arg = 5; });
        add([](FFPipelineState& s) {
            s.active_texture_stages = 2;
            s.texture_stages[0].color_op = TexStageOp::Premodulate;
            s.texture_stages[1].color_op = TexStageOp::Modulate;
        });
        for (u32 tci : {0x00000u, 0x10000u, 0x20000u, 0x30000u}) {
            for (u32 set = 0; set < 3; ++set) {
                add([&](FFPipelineState& s) { s.texture_stages[0].texcoord_index = tci | set; });
                add([&](FFPipelineState& s) {
                    s.fvf_flags |= fvf::NORMAL;
                    s.normalize_normals = true;
                    s.texture_stages[0].texcoord_index = tci | set;
                });
                add([&](FFPipelineState& s) {
                    s.fvf_flags |= fvf::NORMAL;
                    s.vertex_tweening = true;
                    s.texture_stages[0].texcoord_index = tci | set;
                });
            }
        }
        for (u32 count = 0; count <= 4; ++count) {
            for (u32 projected : {0u, 0x100u}) {
                add([&](FFPipelineState& s) { s.texture_stages[0].texture_transform_flags = count | projected; });
                add([&](FFPipelineState& s) {
                    s.texture_stages[0].texture_transform_flags = count | projected;
                    s.texture_stages[0].color_op = TexStageOp::Modulate;
                    s.active_texture_stages = 2;
                    s.texture_stages[1].color_op = TexStageOp::Modulate;
                    s.texture_stages[0].color_op = TexStageOp::BumpEnvMap;
                });
            }
        }
        for (u32 mask : {0x1u, 0x2u, 0xFFu}) {
            add([&](FFPipelineState& s) { s.srgb_texture_mask = mask; });
        }
        // Fog.
        for (u8 mode : {u8{0}, u8{1}, u8{2}, u8{3}}) {
            for (u32 variant = 0; variant < 8; ++variant) {
                add([&](FFPipelineState& s) {
                    s.fog_mode = static_cast<FogMode>(mode);
                    s.table_fog = (variant & 1) != 0;
                    s.table_fog_w = (variant & 2) != 0;
                    s.range_fog_enable = (variant & 4) != 0;
                });
            }
            add([&](FFPipelineState& s) {
                s.fog_mode = static_cast<FogMode>(mode);
                s.vertex_tweening = true;
            });
            add([&](FFPipelineState& s) {
                s.fog_mode = static_cast<FogMode>(mode);
                s.fvf_flags ^= fvf::SPECULAR;
            });
        }
        // Alpha test.
        for (u32 func = 1; func <= 8; ++func) {
            add([&](FFPipelineState& s) {
                s.alpha_test_enable = true;
                s.alpha_test_func = func;
            });
        }
    }
    return out;
}

std::string entry_line(const Entry& e, const std::vector<std::string>& outs) {
    std::string line = e.source + "|" + e.kind + "|" + list_text(e.colors) + "|" + list_text(e.texcoords) + "|" +
                       (e.fog ? "1" : "0");
    for (const std::string& o : outs) line += "|" + o;
    line += "|" + e.state_text;
    return line;
}

bool split(const std::string& line, std::vector<std::string>& cols, usize n) {
    cols.clear();
    usize at = 0;
    for (usize i = 0; i + 1 < n; ++i) {
        const usize bar = line.find('|', at);
        if (bar == std::string::npos) return false;
        cols.push_back(line.substr(at, bar - at));
        at = bar + 1;
    }
    cols.push_back(line.substr(at));
    return true;
}

usize output_columns(const std::string& kind) { return kind == "full" ? 6 : kind == "sm3frag" ? 3 : 2; }

std::string entry_key(const Entry& e) {
    return e.kind + "|" + list_text(e.colors) + "|" + list_text(e.texcoords) + "|" + (e.fog ? "1" : "0") + "|" +
           e.state_text;
}

/// The derived corpus, each state also run through both SM3-hybrid
/// generators, minus entries already in @p seen (the captured ones).
std::vector<Entry> derived_entries(std::set<std::string>& seen) {
    std::vector<Entry> entries;
    auto push = [&](Entry e) {
        e.state_text = ff_text::write_state(e.state);
        if (seen.insert(entry_key(e)).second) entries.push_back(std::move(e));
    };
    for (const FFPipelineState& s : derived_states()) {
        Entry e;
        e.source = "derived";
        e.state = s;
        e.kind = "full";
        push(e);
        e.kind = "sm3frag";
        e.colors = {0, 1};
        e.texcoords = {0, 1};
        push(e);
        e.fog = true;
        e.texcoords = {1};
        push(e);
        e.fog = false;
        e.kind = "sm3vert";
        e.colors = {0, 1};
        e.texcoords = {0, 1, 2};
        push(e);
    }
    return entries;
}

/// Parses `kind|colors|texcoords|fog|state` (the capture hook's format).
bool parse_capture(const std::string& line, Entry& e) {
    std::vector<std::string> c;
    if (!split(line, c, 5)) return false;
    e.source = "captured";
    e.kind = c[0];
    e.colors = parse_list(c[1]);
    e.texcoords = parse_list(c[2]);
    e.fog = c[3] == "1";
    std::string unknown;
    if (!ff_text::read_state(c[4], e.state, unknown)) {
        ADD_FAILURE() << "unparsable state key " << unknown;
        return false;
    }
    e.state_text = ff_text::write_state(e.state);
    return true;
}

/// A fixture line: source|kind|colors|texcoords|fog|outputs...|state. Derived
/// lines store `@` + the digest of the state text instead of the text (the
/// state is rebuilt by derived_states()), which keeps the fixture small.
bool parse_fixture_line(const std::string& line, Entry& e, std::vector<std::string>& expected) {
    std::vector<std::string> head;
    if (!split(line, head, 6)) return false;
    const usize outs = output_columns(head[1]);
    std::vector<std::string> cols;
    if (!split(line, cols, 5 + outs + 1)) return false;
    e.source = cols[0];
    e.kind = cols[1];
    e.colors = parse_list(cols[2]);
    e.texcoords = parse_list(cols[3]);
    e.fog = cols[4] == "1";
    expected.assign(cols.begin() + 5, cols.begin() + 5 + static_cast<long>(outs));
    e.state_text = cols[5 + outs];
    if (e.source == "captured") {
        std::string unknown;
        if (!ff_text::read_state(e.state_text, e.state, unknown)) {
            ADD_FAILURE() << "unknown key " << unknown;
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(FFEmulationD3DGolden, D3DConfigurationsAreByteIdentical) {
    if (const char* record = std::getenv("VITREO_FF_GOLDEN_RECORD")) {
        std::vector<std::string> capture_lines;
        if (std::string(record) == "-") {
            std::ifstream in(kFixture);
            std::string line;
            while (std::getline(in, line)) {
                Entry e;
                std::vector<std::string> expected;
                if (line.rfind("captured|", 0) == 0 && parse_fixture_line(line, e, expected)) {
                    capture_lines.push_back(e.kind + "|" + list_text(e.colors) + "|" + list_text(e.texcoords) + "|" +
                                            (e.fog ? "1" : "0") + "|" + e.state_text);
                }
            }
        } else {
            std::ifstream in(record);
            std::string line;
            while (std::getline(in, line)) capture_lines.push_back(line);
        }
        std::set<std::string> seen;
        std::vector<Entry> entries;
        for (const std::string& line : capture_lines) {
            Entry e;
            if (parse_capture(line, e) && seen.insert(entry_key(e)).second) entries.push_back(e);
        }
        for (Entry& e : derived_entries(seen)) entries.push_back(std::move(e));
        std::ofstream out(kFixture);
        out << "# Direct3D fixed-function generator goldens; see tests/graphics/test_ff_emulation_d3d_golden.cpp.\n"
            << "# source|kind|colors|texcoords|fog|state_hash|outputs...|state (derived: @digest of the state text)\n";
        for (const Entry& e : entries) {
            Entry stored = e;
            if (e.source == "derived") stored.state_text = "@" + digest(e.state_text);
            out << entry_line(stored, outputs(e)) << "\n";
        }
        ASSERT_TRUE(out.good()) << kFixture;
        std::printf("recorded %zu entries into %s\n", entries.size(), kFixture.c_str());
        return;
    }

    std::ifstream in(kFixture);
    ASSERT_TRUE(in.good()) << "missing golden fixture " << kFixture;
    std::vector<Entry> all, derived_lines;
    std::vector<std::vector<std::string>> all_expected, derived_expected;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        Entry e;
        std::vector<std::string> expected;
        ASSERT_TRUE(parse_fixture_line(line, e, expected)) << line;
        if (e.source == "captured") {
            all.push_back(e);
            all_expected.push_back(expected);
        } else {
            derived_lines.push_back(e);
            derived_expected.push_back(expected);
        }
    }
    const usize captured = all.size();
    std::set<std::string> seen;
    for (Entry& e : all) {
        e.state_text = ff_text::write_state(e.state);
        seen.insert(entry_key(e));
    }
    const std::vector<Entry> derived = derived_entries(seen);
    ASSERT_EQ(derived.size(), derived_lines.size()) << "derived_states() no longer matches the fixture";
    for (usize i = 0; i < derived.size(); ++i) {
        const Entry& rebuilt = derived[i];
        const Entry& stored = derived_lines[i];
        ASSERT_EQ(stored.state_text, "@" + digest(rebuilt.state_text))
            << "derived entry " << i << " no longer builds the recorded state:" << rebuilt.state_text;
        ASSERT_EQ(stored.kind, rebuilt.kind);
        ASSERT_EQ(stored.colors, rebuilt.colors);
        ASSERT_EQ(stored.texcoords, rebuilt.texcoords);
        ASSERT_EQ(stored.fog, rebuilt.fog);
        all.push_back(rebuilt);
        all_expected.push_back(derived_expected[i]);
    }

    // VITREO_FF_GOLDEN_DUMP=<file> writes every entry's full outputs (MSL
    // text and uniform bytes, hex) so a deliberate re-record can be diffed
    // against the generator it replaces with tests/tools/ff_golden_proof.py.
    if (const char* dump_path = std::getenv("VITREO_FF_GOLDEN_DUMP")) {
        std::ofstream d(dump_path);
        const auto hex = [](const void* p, usize n) {
            static const char* k = "0123456789abcdef";
            std::string h;
            for (usize i = 0; i < n; ++i) {
                const u8 b = static_cast<const u8*>(p)[i];
                h += k[b >> 4];
                h += k[b & 15];
            }
            return h;
        };
        for (usize n = 0; n < all.size(); ++n) {
            const Entry& e = all[n];
            d << "E|" << n << "|" << e.source << "|" << e.kind << "|" << list_text(e.colors) << "|"
              << list_text(e.texcoords) << "|" << (e.fog ? 1 : 0) << "|" << e.state_text << "\n";
            std::string vsm, fsm;
            std::vector<u8> vs, fs;
            if (e.kind == "full") {
                const FFGeneratedShaders g = generate_ff_shaders(e.state);
                vsm = g.vertex_msl;
                fsm = g.fragment_msl;
                pack_ff_vertex_uniforms(e.state, golden_inputs(), vs);
                pack_ff_fragment_uniforms(e.state, golden_inputs(), fs);
            } else if (e.kind == "sm3frag") {
                fsm = generate_ff_fragment_for_sm3_varyings(e.state, e.colors, e.texcoords, e.fog);
                pack_ff_fragment_uniforms(e.state, golden_inputs(), fs);
            } else {
                vsm = generate_ff_vertex_for_sm3_varyings(e.state, e.colors, e.texcoords);
            }
            d << "VM|" << hex(vsm.data(), vsm.size()) << "\nFM|" << hex(fsm.data(), fsm.size()) << "\nVU|"
              << hex(vs.data(), vs.size()) << "\nFU|" << hex(fs.data(), fs.size()) << "\n";
        }
        ASSERT_TRUE(d.good()) << dump_path;
    }

    static const char* kFullNames[] = {"state hash",   "generated state_hash", "vertex MSL",
                                       "fragment MSL", "vertex uniforms",      "fragment uniforms"};
    usize mismatched = 0;
    for (usize n = 0; n < all.size(); ++n) {
        const Entry& e = all[n];
        const std::vector<std::string> actual = outputs(e);
        ASSERT_EQ(actual.size(), all_expected[n].size());
        for (usize i = 0; i < actual.size(); ++i) {
            if (actual[i] == all_expected[n][i]) continue;
            const char* what = e.kind == "full" ? kFullNames[i]
                               : i == 0         ? "state hash"
                               : i == 1         ? "SM3-hybrid MSL"
                                                : "fragment uniforms";
            if (++mismatched <= 20) {
                ADD_FAILURE() << e.source << " " << e.kind << " entry: " << what << " changed\n  expected "
                              << all_expected[n][i] << "\n  actual   " << actual[i] << "\n  state:" << e.state_text;
            }
        }
    }
    EXPECT_EQ(mismatched, 0u);
    // The fixture must not have been emptied or truncated.
    EXPECT_GE(captured, 100u);
    EXPECT_GE(all.size(), 2000u);
    std::printf("checked %zu D3D generator configurations (%zu captured from the D3D tests)\n", all.size(), captured);
}
