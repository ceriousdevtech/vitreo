// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "graphics/shaders/sm3_to_msl.hpp"
#include <array>
#include <cstring>
#include <vector>

namespace vitreo::graphics::sm3_def_test {
using Bits = std::array<u32, 4>;
inline u32 float_bits(float value) { u32 bits; std::memcpy(&bits, &value, sizeof(bits)); return bits; }
inline Bits defined_bits(u32 ordinal, bool raw_edges = true) {
    return {float_bits(static_cast<float>(ordinal) + 0.25f),
            float_bits(-static_cast<float>(ordinal) - 0.5f),
            ordinal == 0 && raw_edges ? 0x80000000u : float_bits(static_cast<float>(ordinal) + 0.75f),
            ordinal == 0 && raw_edges ? 0x7fc12345u : float_bits(static_cast<float>(ordinal) + 1.0f)};
}
inline Bits api_bits(u32 slot) {
    return {float_bits(static_cast<float>(slot) + 512.0f), float_bits(-static_cast<float>(slot) - 512.0f),
            float_bits(static_cast<float>(slot) + 1024.0f), float_bits(1.0f)};
}
inline u32 def_slot(u32 ordinal, bool sparse) { return sparse ? (ordinal * 73u + 7u) % 256u : ordinal; }
inline u32 reg(u32 type) { return ((type & 7u) << 28) | (((type >> 3) & 3u) << 11); }
inline u32 dest(u32 type, u32 slot, u32 mask = 15u) { return 0x80000000u | reg(type) | slot | (mask << 16); }
inline u32 source(u32 type, u32 slot, u32 swizzle = 0xe4u, u32 modifier = 0) {
    return 0x80000000u | reg(type) | slot | (swizzle << 16) | (modifier << 24);
}
inline std::vector<u32> vertex(u32 model, u32 count, bool sparse,
                               u32 base = 0, u32 component = 0,
                               u32 swizzle = 0xe4u, u32 modifier = 0, bool loop = false) {
    std::vector<u32> w{0xfffe0000u | (model << 8) | (model == 1 ? 1u : 0u)};
    const auto op = [&](u32 opcode, u32 length) { return opcode | (model == 1 ? 0u : length << 24); };
    for (u32 i = 0; i < count; ++i) {
        // Raw-payload checks apply to the LUT representation; the retained
        // small-switch float formatter makes no NaN-payload guarantee.
        const auto bits = defined_bits(i, count >= 96u);
        w.insert(w.end(), {op(81, 5), dest(2, def_slot(i, sparse)), bits[0], bits[1], bits[2], bits[3]});
    }
    w.insert(w.end(), {op(31, 2), 0u, dest(1, 0), op(31, 2), 5u, dest(1, 1)});
    if (model == 3) w.insert(w.end(), {op(31, 2), 0u, dest(6, 0), op(31, 2), 5u, dest(6, 1)});
    if (loop) w.insert(w.end(), {op(48, 5), dest(7, 0), 1, 0, 1, 0});
    w.insert(w.end(), {op(1, 2), dest(model == 3 ? 6u : 4u, 0), source(1, 0)});
    if (loop) {
        w.insert(w.end(), {op(27, 2), dest(15, 0), source(7, 0)});
    } else {
        w.insert(w.end(), {op(model == 1 ? 1u : 46u, 2), dest(3, 0, 1u << component),
                          source(1, 1, component * 0x55u)});
    }
    w.insert(w.end(), {op(1, model == 1 ? 2u : 3u), dest(6, model == 3 ? 1u : 0u),
                      source(2, base, swizzle, modifier) | (1u << 13)});
    if (model != 1) w.push_back(loop ? source(15, 0, 0) : source(3, 0, component * 0x55u));
    if (loop) w.push_back(op(29, 0));
    w.push_back(0xffffu);
    return w;
}
}
