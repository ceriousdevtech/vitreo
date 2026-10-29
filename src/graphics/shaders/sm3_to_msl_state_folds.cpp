// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_state_folds.cpp
/// @brief D3D9 render- and sampler-state folds over translated SM1-3 MSL.

#include "sm3_to_msl.hpp"

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace vitreo::graphics {

namespace {

/// The two sRGB transfer functions, inserted once into a shader that needs
/// them. Alpha is never converted -- D3D9 converts colour only.
constexpr const char* kSrgbHelpers =
    "static inline float4 vitreo_srgb_decode(float4 c) {\n"
    "    c.rgb = select(c.rgb / 12.92f,\n"
    "                   powr((c.rgb + 0.055f) / 1.055f, 2.4f),\n"
    "                   c.rgb > 0.04045f);\n"
    "    return c;\n"
    "}\n"
    "static inline float3 vitreo_srgb_encode(float3 c) {\n"
    "    c = saturate(c);\n"
    "    return select(c * 12.92f,\n"
    "                  1.055f * powr(c, 1.0f / 2.4f) - 0.055f,\n"
    "                  c > 0.0031308f);\n"
    "}\n\n";

/// Insert the transfer functions after `using namespace metal;`, once.
[[nodiscard]] bool insert_srgb_helpers(std::string& msl) {
    if (msl.find("vitreo_srgb_decode") != std::string::npos ||
        msl.find("vitreo_srgb_encode") != std::string::npos) {
        return true;
    }
    constexpr std::string_view kAnchor = "using namespace metal;\n";
    const auto at = msl.find(kAnchor);
    if (at == std::string::npos) return false;
    msl.insert(at + kAnchor.size(), kSrgbHelpers);
    return true;
}

/// Index of the ')' matching the '(' at `open`.
[[nodiscard]] usize matching_paren(const std::string& msl, usize open) {
    unsigned depth = 0;
    for (usize i = open; i < msl.size(); ++i) {
        if (msl[i] == '(') {
            ++depth;
        } else if (msl[i] == ')' && --depth == 0) {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace

std::string apply_sm3_srgb_texture(std::string msl, u32 sampler_mask) {
    if (sampler_mask == 0 || msl.empty()) return msl;
    if (!insert_srgb_helpers(msl)) return msl;

    for (u32 sampler = 0; sampler < 16u; ++sampler) {
        if ((sampler_mask & (1u << sampler)) == 0) continue;
        const std::string index = std::to_string(sampler);
        const std::string marker = "tex" + index + ".sample(samp" + index + ",";
        usize search = 0;
        while ((search = msl.find(marker, search)) != std::string::npos) {
            // "tex10" also contains "tex1", so reject a longer identifier.
            const char after = msl[search + 3 + index.size()];
            if (after >= '0' && after <= '9') {
                search += marker.size();
                continue;
            }
            const usize open = msl.find('(', search);
            const usize close = matching_paren(msl, open);
            if (close == std::string::npos) break;
            msl.insert(close + 1, ")");
            msl.insert(search, "vitreo_srgb_decode(");
            search = close + std::strlen("vitreo_srgb_decode(") + 2;
        }
    }
    return msl;
}

std::string apply_sm3_srgb_write(std::string msl) {
    if (msl.empty()) return msl;
    if (!insert_srgb_helpers(msl)) return msl;
    const auto ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    msl.insert(ret, "    out.color.rgb = vitreo_srgb_encode(out.color.rgb);\n");
    return msl;
}

std::string apply_sm3_alpha_test(std::string msl, u32 cmp_func) {
    // D3DCMP_ALWAYS (8) and the unset-zero default keep every pixel.
    if (cmp_func == 8u || cmp_func == 0u || msl.empty()) return msl;

    const char* condition = nullptr;
    switch (cmp_func) {
    case 1: // NEVER
        condition = "true";
        break;
    case 2: // LESS — discard when a >= ref
        condition = "out.color.a >= cb.alpha_test.x";
        break;
    case 3: // EQUAL
        condition = "abs(out.color.a - cb.alpha_test.x) > 0.004";
        break;
    case 4: // LESSEQUAL
        condition = "out.color.a > cb.alpha_test.x";
        break;
    case 5: // GREATER
        condition = "out.color.a <= cb.alpha_test.x";
        break;
    case 6: // NOTEQUAL
        condition = "abs(out.color.a - cb.alpha_test.x) <= 0.004";
        break;
    case 7: // GREATEREQUAL
        condition = "out.color.a < cb.alpha_test.x";
        break;
    default:
        return msl;
    }

    const auto struct_pos = msl.find("struct Sm3Constants");
    if (struct_pos == std::string::npos) return msl;
    const auto close = msl.find("};", struct_pos);
    if (close == std::string::npos) return msl;
    msl.insert(close, "    float4 alpha_test;\n");

    const auto ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    if (cmp_func == 1u) {
        msl.insert(ret, "    discard_fragment();\n");
    } else {
        msl.insert(ret, std::string("    if (") + condition +
                            ") discard_fragment();\n");
    }
    return msl;
}

std::string apply_sm3_fog(std::string msl, bool vertex_fog) {
    if (!vertex_fog || msl.empty()) return msl;

    const auto varyings = msl.find("struct Sm3Varyings");
    if (varyings == std::string::npos) return msl;
    const auto varyings_close = msl.find("};", varyings);
    if (varyings_close == std::string::npos) return msl;
    if (msl.find("float4 fog [[user(fog)]];", varyings) > varyings_close) {
        msl.insert(varyings_close, "    float4 fog [[user(fog)]];\n");
    }

    const auto constants = msl.find("struct Sm3Constants");
    if (constants == std::string::npos) return msl;
    const auto constants_close = msl.find("};", constants);
    if (constants_close == std::string::npos) return msl;
    msl.insert(constants_close, "    float4 fog_color;\n");

    const auto ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    msl.insert(ret,
               "    out.color.rgb = mix(cb.fog_color.rgb, out.color.rgb, "
               "clamp(in.fog.x, 0.0, 1.0));\n");
    return msl;
}

std::string apply_sm3_table_fog(std::string msl, u32 fog_mode, bool w_fog) {
    if (msl.empty() || fog_mode == 0 ||
        msl.find("fragment Sm3FragmentOut") == std::string::npos) {
        return msl;
    }
    const usize constants = msl.find("struct Sm3Constants");
    if (constants == std::string::npos) return msl;
    const usize constants_close = msl.find("};", constants);
    if (constants_close == std::string::npos) return msl;
    msl.insert(constants_close,
               "    float4 table_fog_params;\n"
               "    float4 table_fog_color;\n");

    const usize ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    std::ostringstream fog;
    fog << "    float table_fog_distance = ";
    if (w_fog) {
        fog << "1.0 / max(abs(in.frag_position.w), 1e-6);\n";
    } else {
        fog << "in.frag_position.z;\n";
    }
    if (fog_mode == 3u) {
        fog << "    float table_fog_factor = clamp((cb.table_fog_params.y - "
               "table_fog_distance) / max(cb.table_fog_params.y - "
               "cb.table_fog_params.x, 1e-6), 0.0, 1.0);\n";
    } else if (fog_mode == 1u) {
        fog << "    float table_fog_factor = clamp(exp(-cb.table_fog_params.z "
               "* table_fog_distance), 0.0, 1.0);\n";
    } else {
        fog << "    float table_fog_factor = clamp(exp(-cb.table_fog_params.z "
               "* cb.table_fog_params.z * table_fog_distance * "
               "table_fog_distance), 0.0, 1.0);\n";
    }
    fog << "    out.color.rgb = mix(cb.table_fog_color.rgb, out.color.rgb, "
           "table_fog_factor);\n";
    msl.insert(ret, fog.str());
    return msl;
}

std::string apply_sm3_flat_shading(std::string msl) {
    if (msl.empty()) return msl;
    constexpr std::string_view marker{"[[user(color"};
    usize search = 0;
    while ((search = msl.find(marker, search)) != std::string::npos) {
        const usize close = msl.find(")]]", search + marker.size());
        if (close == std::string::npos) break;
        msl.replace(close, 3, "), flat]]");
        search = close + 9;
    }
    return msl;
}

std::string apply_sm3_point_size(std::string msl,
                                 bool shader_writes_point_size) {
    if (msl.empty()) return msl;

    const usize varyings = msl.find("struct Sm3Varyings");
    if (varyings == std::string::npos) return msl;
    const usize varyings_close = msl.find("};", varyings);
    if (varyings_close == std::string::npos) return msl;
    msl.insert(varyings_close, "    float point_size [[point_size]];\n");

    const usize constants = msl.find("struct Sm3Constants");
    if (constants == std::string::npos) return msl;
    const usize constants_close = msl.find("};", constants);
    if (constants_close == std::string::npos) return msl;
    msl.insert(constants_close, "    float4 point_params;\n");

    const usize ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    if (shader_writes_point_size) {
        msl.insert(ret,
                   "    out.point_size = clamp(point_size_value.x, "
                   "cb.point_params.y, cb.point_params.z);\n");
    } else {
        msl.insert(ret, "    out.point_size = cb.point_params.x;\n");
    }
    return msl;
}

std::string apply_sm3_clip_planes(std::string msl) {
    if (msl.empty()) return msl;

    const usize varyings = msl.find("struct Sm3Varyings");
    if (varyings == std::string::npos) return msl;
    const usize varyings_close = msl.find("};", varyings);
    if (varyings_close == std::string::npos) return msl;
    msl.insert(varyings_close,
               "    float clip_distances [[clip_distance]] [6];\n");

    const usize constants = msl.find("struct Sm3Constants");
    if (constants == std::string::npos) return msl;
    const usize constants_close = msl.find("};", constants);
    if (constants_close == std::string::npos) return msl;
    msl.insert(constants_close, "    float4 clip_planes[6];\n");

    const usize ret = msl.rfind("    return out;");
    if (ret == std::string::npos) return msl;
    msl.insert(ret,
               "    for (uint clip_index = 0; clip_index < 6; ++clip_index) {\n"
               "        out.clip_distances[clip_index] = "
               "dot(cb.clip_planes[clip_index], out.position);\n"
               "    }\n");
    return msl;
}

std::string apply_sm3_sampler_lod_bias(
    std::string msl,
    const std::array<float, SM3_MAX_SAMPLERS>& lod_biases) {
    const auto literal = [](float value) {
        std::ostringstream out;
        out << std::setprecision(9) << value;
        std::string result = out.str();
        if (result.find_first_of(".eE") == std::string::npos) result += ".0";
        return result;
    };
    const auto matching_paren = [&](usize open) {
        unsigned depth = 0;
        for (usize i = open; i < msl.size(); ++i) {
            if (msl[i] == '(') {
                ++depth;
            } else if (msl[i] == ')' && --depth == 0) {
                return i;
            }
        }
        return std::string::npos;
    };

    for (u32 sampler = 0; sampler < lod_biases.size(); ++sampler) {
        if (lod_biases[sampler] == 0.0f) continue;
        const std::string value = literal(lod_biases[sampler]);
        const std::string marker =
            ".sample(samp" + std::to_string(sampler) + ",";
        usize search = 0;
        while ((search = msl.find(marker, search)) != std::string::npos) {
            const usize sample_open = msl.find('(', search);
            usize sample_close = matching_paren(sample_open);
            if (sample_close == std::string::npos) break;
            const auto add_to_argument = [&](std::string_view name) {
                const usize argument = msl.find(name, sample_open);
                if (argument == std::string::npos || argument > sample_close) {
                    return false;
                }
                const usize open = argument + name.size() - 1;
                const usize close = matching_paren(open);
                if (close == std::string::npos || close > sample_close) {
                    return false;
                }
                msl.insert(close, " + " + value);
                sample_close += value.size() + 3;
                return true;
            };
            if (!add_to_argument("bias(") && !add_to_argument("level(")) {
                const std::string addition = ", bias(" + value + ")";
                msl.insert(sample_close, addition);
                sample_close += addition.size();
            }
            search = sample_close + 1;
        }
    }
    return msl;
}

} // namespace vitreo::graphics
