// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "graphics/shaders/dxbc_parser.hpp"
#include <array>
#include <vector>
#include <cstring>

namespace vitreo::graphics::dxbc_spirv_test {
using IcbRow = std::array<u32, 4>;
// Authored token stream, not captured application bytecode. The input carries
// untyped index bits; MOV must preserve every ICB payload bit into a UINT target.
inline DxbcContainer make_icb_fixture(const std::vector<IcbRow>& rows, bool dynamic,
                               u32 base = 0, u32 component = 0,
                               u32 swizzle = 0xE4, u32 reads = 1, bool indexable = false) {
    DxbcContainer c{};
    c.shader_model_major = 5;
    c.stage = ShaderStage::Fragment;
    c.input_signature.push_back({"INDEX", 0, 0, 0xFu, 1});
    c.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 1});
    std::vector<u32> words{0x50, 0, 104u | (2u << 24), 1,
                           0x00001835u, static_cast<u32>(2 + rows.size() * 4)};
    for (const auto& row : rows) words.insert(words.end(), row.begin(), row.end());
    if (indexable) {
        words.insert(words.end(), {105u | (4u << 24), 0, 2, 4,
            54u | (6u << 24), 0x002030F2u, 0, 1, 0x00101E46u, 0});
    } else {
        words.insert(words.end(), {54u | (5u << 24), 0x001000F2u, 0, 0x00101E46u, 0});
    }
    for (u32 i = 0; i < reads; ++i) {
        words.insert(words.end(), {54u | ((dynamic ? (indexable ? 8u : 7u) : 5u) << 24),
            0x001020F2u, 0,
            (dynamic ? 0x00D09006u : 0x00109006u) | (swizzle << 4), base});
        if (dynamic) {
            words.insert(words.end(), {(indexable ? 0x0020300Au : 0x0010000Au) | (component << 4), 0});
            if (indexable) words.push_back(1);
        }
    }
    words.push_back(62u | (1u << 24));
    words[1] = static_cast<u32>(words.size());
    c.bytecode.resize(words.size() * sizeof(u32));
    std::memcpy(c.bytecode.data(), words.data(), c.bytecode.size());
    return c;
}
}
