// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"
#include <cstring>
#include <vector>

namespace vitreo::graphics::dxil_constant_test {
constexpr u32 kReadonlySlot = 0x80000000u;
inline DxbcContainer readonly_words_fixture(const std::vector<u32>& data) {
    DxbcContainer c{};
    c.shader_model_major = 5;
    c.shader_model_minor = 1;
    c.stage = ShaderStage::Fragment;
    c.input_signature.push_back({"INDEX", 0, 0, 0xFu, 1});
    c.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 1});
    std::vector<u32> words{0x51u, 0u,
        53u | (dxbc_vitreo_ext::kReadonlyIndexableDataClass << 11),
        static_cast<u32>(data.size()) + 3u, kReadonlySlot};
    words.insert(words.end(), data.begin(), data.end());
    words.insert(words.end(), {104u | (2u << 24), 1u,
        54u | (5u << 24), 0x001000F2u, 0u, 0x00101E46u, 0u});
    for (u32 lane = 0; lane < 4; ++lane) {
        words.insert(words.end(), {54u | (8u << 24),
            0x00102002u | (1u << (4u + lane)), 0u,
            0x0620300Au, kReadonlySlot, lane, 0x0010000Au, 0u});
    }
    words.push_back(62u | (1u << 24));
    words[1] = static_cast<u32>(words.size());
    c.bytecode.resize(words.size() * sizeof(u32));
    std::memcpy(c.bytecode.data(), words.data(), c.bytecode.size());
    return c;
}
inline void insert_before_body(DxbcContainer& c, const std::vector<u32>& data, usize readonly_words) {
    const usize at = (5u + readonly_words) * sizeof(u32);
    const auto* bytes = reinterpret_cast<const u8*>(data.data());
    c.bytecode.insert(c.bytecode.begin() + at, bytes, bytes + data.size() * sizeof(u32));
    const u32 length = static_cast<u32>(c.bytecode.size() / sizeof(u32));
    std::memcpy(c.bytecode.data() + sizeof(u32), &length, sizeof(length));
}
}
