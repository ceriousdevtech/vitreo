// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_device_create_methods.cpp
/// @brief The ID3D10Device Create* methods: the D3D10 descriptions
///        converted to D3D11's and forwarded. Split out of
///        d3d10_device.cpp as verbatim code motion.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// ID3D10Device1: the Create* methods
// -----------------------------------------------------------------------------

void D3D11Manager::d3d10_create_method(Marshaler& m, usize slot,
                                       Device& device) {
    const GuestAddr d3d11_device = device.device_interface;
    const usize mark = d3d10_scratch_mark();
    const auto device_call = [&](usize d3d11_slot,
                                 std::initializer_list<u64> args) {
        return static_cast<u32>(d3d10_forward(&D3D11Manager::impl_DeviceMethod,
                                              d3d11_slot, d3d11_device,
                                              args));
    };
    const auto a = [&](usize index) { return m.arg(index); };
    // The created object's D3D11 pointer, as the out-parameter now holds it,
    // becomes its D3D10 facet.
    const auto finish = [&](u32 hr, GuestAddr out) {
        if (hr == kSOk && out != 0) d3d10_objects_out(out, 1);
        d3d10_scratch_release(mark);
        m.set_return(hr);
    };
    // A blend state is written only on success: measured, a refused
    // CreateBlendState(1) leaves *ppBlendState untouched (every other
    // Create* NULLs its out-parameter).
    const auto blend_state = [&](GuestAddr desc) {
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr cell = out != 0 ? d3d10_scratch(8) : 0;
        const u32 hr = device_call(20, {desc, cell});
        if (hr == kSOk && out != 0) {
            GuestAddr state = 0;
            (void)read_pointer(cell, state);
            const GuestAddr facet = d3d10_facet_of(state);
            (void)m.write_ptr(out, facet != 0 ? facet : state);
        }
        d3d10_scratch_release(mark);
        m.set_return(hr);
    };
    const auto refuse = [&](GuestAddr out, u32 hr) {
        if (out != 0) (void)m.write_ptr(out, 0);
        d3d10_scratch_release(mark);
        m.set_return(hr);
    };
    // The D3D10 texture descriptions are D3D11's, the misc flags renumbered
    // and the D3D11-only bind flags refused: copy with those two words
    // translated.
    const auto texture_desc = [&](GuestAddr desc, usize bytes, usize bind_at,
                                  usize misc_at, GuestAddr& converted) {
        converted = 0;
        std::vector<u8> words(bytes);
        if (m_memory.read_bytes(desc, words.data(), bytes).is_err()) {
            return false;
        }
        u32 bind = 0, misc10 = 0, misc11 = 0;
        std::memcpy(&bind, words.data() + bind_at, 4);
        std::memcpy(&misc10, words.data() + misc_at, 4);
        if ((bind & ~kD3D10BindMask) != 0 || !misc_to_d3d11(misc10, misc11)) {
            return false;
        }
        std::memcpy(words.data() + misc_at, &misc11, 4);
        converted = d3d10_scratch(bytes);
        return converted != 0 &&
               m_memory.write_bytes(converted, words.data(), bytes).is_ok();
    };

    switch (slot) {
    case 71: {
        // CreateBuffer(const D3D10_BUFFER_DESC*, pInitialData, ppBuffer):
        // ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags -- no
        // StructureByteStride.
        const GuestAddr desc = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(3);
        if (desc == 0) {
            finish(device_call(3, {0, a(2), a(3)}), 0);
            return;
        }
        std::array<u32, 6> words{};
        if (m_memory.read_bytes(desc, reinterpret_cast<u8*>(words.data()), 20)
                .is_err()) {
            refuse(out, kEInvalidArg);
            return;
        }
        u32 misc11 = 0;
        if ((words[2] & ~kD3D10BindMask) != 0 ||
            !misc_to_d3d11(words[4], misc11)) {
            refuse(out, kEInvalidArg);
            return;
        }
        words[4] = misc11;
        words[5] = 0;
        const GuestAddr converted = d3d10_scratch(sizeof(words));
        if (converted == 0 ||
            m_memory.write_bytes(converted,
                                 reinterpret_cast<const u8*>(words.data()),
                                 sizeof(words)).is_err()) {
            refuse(out, kEOutOfMemory);
            return;
        }
        finish(device_call(3, {converted, a(2), a(3)}), out);
        return;
    }
    case 72:
    case 73:
    case 74: {
        // CreateTexture1D / 2D / 3D: D3D10_TEXTURE{1,2,3}D_DESC.
        static constexpr std::array<std::array<usize, 3>, 3> layout{{
            {32, 20, 28}, {44, 32, 40}, {36, 24, 32}}};
        const auto& shape = layout[slot - 72];
        const GuestAddr desc = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(3);
        const usize d3d11_slot = 4 + (slot - 72);
        if (desc == 0) {
            finish(device_call(d3d11_slot, {0, a(2), a(3)}), 0);
            return;
        }
        GuestAddr converted = 0;
        if (!texture_desc(desc, shape[0], shape[1], shape[2], converted)) {
            refuse(out, kEInvalidArg);
            return;
        }
        finish(device_call(d3d11_slot, {converted, a(2), a(3)}), out);
        return;
    }
    case 75:
    case 98: {
        // CreateShaderResourceView(pResource, desc, ppView) and the 10.1
        // CreateShaderResourceView1: D3D11's description layout, with the
        // D3D10 dimensions only -- TEXTURECUBEARRAY (10) through the `1`
        // entry point alone, D3D11's BUFFEREX (11) never (measured).
        const GuestAddr desc = m.arg_ptr(2);
        const GuestAddr out = m.arg_ptr(3);
        if (desc != 0) {
            const auto dimension = m_memory.read_u32(desc + 4);
            const u32 limit = slot == 98 ? 10u : 9u;
            if (dimension.is_err() || dimension.value() > limit) {
                refuse(out, kEInvalidArg);
                return;
            }
        }
        finish(device_call(7, {d3d10_owner_of(m.arg_ptr(1)), a(2), a(3)}),
               out);
        return;
    }
    case 76:
        // CreateRenderTargetView: D3D11's description layout.
        finish(device_call(9, {d3d10_owner_of(m.arg_ptr(1)), a(2), a(3)}),
               m.arg_ptr(3));
        return;
    case 77: {
        // CreateDepthStencilView: D3D10_DEPTH_STENCIL_VIEW_DESC is Format,
        // ViewDimension and the union -- D3D11's without its Flags word.
        const GuestAddr desc = m.arg_ptr(2);
        const GuestAddr out = m.arg_ptr(3);
        GuestAddr converted = 0;
        if (desc != 0) {
            std::array<u32, 5> d3d10{};
            if (m_memory.read_bytes(desc, reinterpret_cast<u8*>(d3d10.data()),
                                    sizeof(d3d10)).is_err()) {
                refuse(out, kEInvalidArg);
                return;
            }
            const std::array<u32, 6> d3d11{d3d10[0], d3d10[1], 0,
                                           d3d10[2], d3d10[3], d3d10[4]};
            converted = d3d10_scratch(sizeof(d3d11));
            if (converted == 0 ||
                m_memory.write_bytes(converted,
                                     reinterpret_cast<const u8*>(d3d11.data()),
                                     sizeof(d3d11)).is_err()) {
                refuse(out, kEOutOfMemory);
                return;
            }
        }
        finish(device_call(10, {d3d10_owner_of(m.arg_ptr(1)), converted,
                                a(3)}),
               out);
        return;
    }
    case 78:
        // CreateInputLayout: D3D10_INPUT_ELEMENT_DESC is D3D11's.
        finish(device_call(11, {a(1), a(2), a(3), a(4), a(5)}),
               m.arg_ptr(5));
        return;
    case 79:  // CreateVertexShader(pBytecode, Length, ppVS): no linkage.
        finish(device_call(12, {a(1), a(2), 0, a(3)}), m.arg_ptr(3));
        return;
    case 80:  // CreateGeometryShader
        finish(device_call(13, {a(1), a(2), 0, a(3)}), m.arg_ptr(3));
        return;
    case 81: {
        // CreateGeometryShaderWithStreamOutput(pBytecode, Length,
        // pSODeclaration, NumEntries, OutputStreamStride, ppGS). D3D10's
        // entry has no Stream; OutputStreamStride is the one buffer's stride
        // when every entry writes slot 0. With several slots each takes one
        // element (Learn; measured: a slot with two entries is
        // E_INVALIDARG) and its stride is that element's size.
        const GuestAddr decl = m.arg_ptr(3);
        const u32 entries = m.arg32(4);
        const u32 stride = m.arg32(5);
        const GuestAddr out = m.arg_ptr(6);
        const usize entry10 = m_is_64bit ? 16u : 12u;
        const usize entry11 = m_is_64bit ? 24u : 16u;
        const usize name_at11 = m_is_64bit ? 8u : 4u;
        const usize pointer = m_is_64bit ? 8u : 4u;
        if (decl == 0 || entries == 0 || entries > 64) {
            // No declaration: the D3D11 rules decide.
            finish(device_call(14, {a(1), a(2), a(3), a(4), 0, 0, 0, 0,
                                    a(6)}),
                   out);
            return;
        }
        const GuestAddr converted = d3d10_scratch(entries * entry11);
        const GuestAddr strides = d3d10_scratch(4 * 4);
        if (converted == 0 || strides == 0) {
            refuse(out, kEOutOfMemory);
            return;
        }
        std::array<u32, 4> slot_entries{};
        std::array<u32, 4> slot_components{};
        u32 highest_slot = 0;
        for (u32 i = 0; i < entries; ++i) {
            std::array<u8, 16> record{};
            if (m_memory.read_bytes(decl + i * entry10, record.data(),
                                    entry10).is_err()) {
                refuse(out, kEInvalidArg);
                return;
            }
            GuestAddr name = 0;
            std::memcpy(&name, record.data(), pointer);
            u32 index = 0;
            std::memcpy(&index, record.data() + pointer, 4);
            const u8 start = record[pointer + 4];
            const u8 count = record[pointer + 5];
            const u8 output = record[pointer + 6];
            if (output < 4) {
                ++slot_entries[output];
                slot_components[output] += count;
                highest_slot = std::max<u32>(highest_slot, output);
            }
            std::array<u8, 24> d3d11{};
            std::memcpy(d3d11.data() + name_at11, &name, pointer);
            std::memcpy(d3d11.data() + name_at11 + pointer, &index, 4);
            d3d11[name_at11 + pointer + 4] = start;
            d3d11[name_at11 + pointer + 5] = count;
            d3d11[name_at11 + pointer + 6] = output;
            (void)m_memory.write_bytes(converted + i * entry11, d3d11.data(),
                                       entry11);
        }
        u32 stride_count = 1;
        if (highest_slot == 0) {
            (void)m_memory.write_u32(strides, stride);
        } else {
            for (u32 s = 0; s <= highest_slot; ++s) {
                if (slot_entries[s] > 1) {
                    refuse(out, kEInvalidArg);
                    return;
                }
                (void)m_memory.write_u32(strides + s * 4u,
                                         slot_components[s] * 4u);
            }
            stride_count = highest_slot + 1;
        }
        finish(device_call(14, {a(1), a(2), converted, entries, strides,
                                stride_count, 0, 0, a(6)}),
               out);
        return;
    }
    case 82:  // CreatePixelShader
        finish(device_call(15, {a(1), a(2), 0, a(3)}), m.arg_ptr(3));
        return;
    case 83: {
        // CreateBlendState(const D3D10_BLEND_DESC*, ppState): per-target
        // BlendEnable and write mask over one blend function. As D3D11
        // sees it (measured through the ID3D11BlendState facet): an
        // enabled target takes the function, a disabled one D3D11's
        // defaults (ONE, ZERO, ADD), and IndependentBlendEnable is set only
        // when the targets then differ.
        const GuestAddr desc = m.arg_ptr(1);
        if (desc == 0) {
            // Measured: E_INVALIDARG with *ppBlendState untouched.
            d3d10_scratch_release(mark);
            m.set_return(kEInvalidArg);
            return;
        }
        std::array<u8, 68> d3d10{};
        if (m_memory.read_bytes(desc, d3d10.data(), d3d10.size()).is_err()) {
            d3d10_scratch_release(mark);
            m.set_return(kEInvalidArg);
            return;
        }
        const auto word = [&](usize at) {
            u32 value = 0;
            std::memcpy(&value, d3d10.data() + at, 4);
            return value;
        };
        std::array<u8, 8 + 8 * 32> d3d11{};
        const u32 alpha_to_coverage = word(0);
        std::memcpy(d3d11.data(), &alpha_to_coverage, 4);
        std::array<std::array<u32, 8>, 8> targets{};
        for (usize rt = 0; rt < 8; ++rt) {
            const u32 enable = word(4 + rt * 4) != 0 ? 1u : 0u;
            const u32 mask = d3d10[60 + rt];
            targets[rt] = enable != 0
                ? std::array<u32, 8>{1u, word(36), word(40), word(44),
                                     word(48), word(52), word(56), mask}
                : std::array<u32, 8>{0u, 2u, 1u, 1u, 2u, 1u, 1u, mask};
        }
        u32 independent = 0;
        for (usize rt = 1; rt < 8; ++rt) {
            if (targets[rt] != targets[0]) independent = 1;
        }
        std::memcpy(d3d11.data() + 4, &independent, 4);
        for (usize rt = 0; rt < 8; ++rt) {
            u8* target = d3d11.data() + 8 + rt * 32;
            for (usize field = 0; field < 7; ++field) {
                std::memcpy(target + field * 4, &targets[rt][field], 4);
            }
            target[28] = static_cast<u8>(targets[rt][7]);
        }
        const GuestAddr converted = d3d10_scratch(d3d11.size());
        if (converted == 0 ||
            m_memory.write_bytes(converted, d3d11.data(), d3d11.size())
                .is_err()) {
            d3d10_scratch_release(mark);
            m.set_return(kEOutOfMemory);
            return;
        }
        blend_state(converted);
        return;
    }
    case 99:
        // CreateBlendState1(const D3D10_BLEND_DESC1*, ppState): D3D11's
        // D3D11_BLEND_DESC layout.
        if (m.arg_ptr(1) == 0) {
            d3d10_scratch_release(mark);
            m.set_return(kEInvalidArg);
            return;
        }
        blend_state(m.arg_ptr(1));
        return;
    case 84:  // CreateDepthStencilState: D3D11's description.
        finish(device_call(21, {a(1), a(2)}), m.arg_ptr(2));
        return;
    case 85:  // CreateRasterizerState: D3D11's description.
        finish(device_call(22, {a(1), a(2)}), m.arg_ptr(2));
        return;
    case 86:  // CreateSamplerState: D3D11's description.
        finish(device_call(23, {a(1), a(2)}), m.arg_ptr(2));
        return;
    case 87:
    case 88: {
        // CreateQuery / CreatePredicate(const D3D10_QUERY_DESC*, pp): the
        // D3D10 query types are D3D11's first eight; a predicate is
        // OCCLUSION_PREDICATE (5) or SO_OVERFLOW_PREDICATE (7) (measured:
        // 8 and every other type E_INVALIDARG).
        const GuestAddr desc = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (desc != 0) {
            const auto query = m_memory.read_u32(desc);
            const bool valid =
                query.is_ok() &&
                (slot == 87 ? query.value() <= 7u
                            : query.value() == 5u || query.value() == 7u);
            if (!valid) {
                refuse(out, kEInvalidArg);
                return;
            }
        }
        finish(device_call(slot == 87 ? 24 : 25, {a(1), a(2)}), out);
        return;
    }
    default:
        d3d11_diag("ID3D10Device slot %zu -> E_NOTIMPL", slot);
        d3d10_scratch_release(mark);
        m.set_return(0x80004001u);
        return;
    }
}

} // namespace vitreo::winapi
