// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxbc_to_spirv_int_machine.hpp
/// @brief The 32-bit integer machine soft_double.hpp and soft_float.hpp run
///        on to emit SPIR-V (dxbc_to_spirv_double.cpp,
///        dxbc_to_spirv_denorm.cpp).

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

#include <array>
#include <utility>

namespace vitreo::graphics::dxbc_spirv_detail {

/// The 32-bit integer machine soft_double::SoftDouble runs on to emit the
/// SM5 double instructions (soft_double.hpp): each operation is one SPIR-V
/// instruction appended to the function being built; values are result ids.
class SpirvIntMachine {
public:
    using U = u32;
    using B = u32;

    SpirvIntMachine(SpirvBuilder& builder, u32 type_uint, u32 type_bool,
                    u32 type_uint_pair, u32 glsl, u32 block)
        : m_b(builder), m_uint(type_uint), m_bool(type_bool),
          m_pair(type_uint_pair), m_glsl(glsl), m_block(block) {}

    U k(u32 value) { return m_b.emit_constant_int(m_uint, value); }
    U add(U a, U b) { return m_b.emit_iadd(m_uint, a, b); }
    U sub(U a, U b) { return m_b.emit_isub(m_uint, a, b); }
    U band(U a, U b) { return m_b.emit_bitwise_and(m_uint, a, b); }
    U bor(U a, U b) { return m_b.emit_bitwise_or(m_uint, a, b); }
    U bxor(U a, U b) { return m_b.emit_bitwise_xor(m_uint, a, b); }
    U shl(U a, U n) { return m_b.emit_shift_left_logical(m_uint, a, n); }
    U shr(U a, U n) { return m_b.emit_shift_right_logical(m_uint, a, n); }
    std::pair<U, U> mul_wide(U a, U b) {
        const u32 product = m_b.emit_umul_extended(m_pair, a, b);
        return {m_b.emit_composite_extract(m_uint, product, {0u}),
                m_b.emit_composite_extract(m_uint, product, {1u})};
    }
    U msb(U x) {
        return m_b.emit_ext_inst(m_uint, m_glsl, kGlslStd450FindUMsb, {x});
    }
    B ult(U a, U b) { return compare(SpvOp::OpULessThan, a, b); }
    B slt(U a, U b) { return compare(SpvOp::OpSLessThan, a, b); }
    B eq(U a, U b) { return compare(SpvOp::OpIEqual, a, b); }
    B ne(U a, U b) { return compare(SpvOp::OpINotEqual, a, b); }
    B land(B a, B b) {
        return m_b.emit_logical(SpvOp::OpLogicalAnd, m_bool, a, b);
    }
    B lor(B a, B b) { return m_b.emit_logical(SpvOp::OpLogicalOr, m_bool, a, b); }
    B lnot(B a) { return m_b.emit_logical_not(m_bool, a); }
    U sel(B c, U a, U b) { return m_b.emit_select(m_uint, c, a, b); }

    /// A structured loop run @p count times: the carried state (and the
    /// trip counter) are OpPhis of the header, whose back-edge operands are
    /// patched in once the body has produced them.
    template <std::size_t N, class Body>
    std::array<U, N> loop(u32 count, const std::array<U, N>& init,
                          Body&& body) {
        const u32 header = m_b.allocate_id();
        const u32 body_block = m_b.allocate_id();
        const u32 continue_block = m_b.allocate_id();
        const u32 merge = m_b.allocate_id();
        const u32 pre = m_block;
        m_b.emit_branch(header);
        m_b.emit_label_id(header);
        std::array<usize, N + 1> back_edges{};
        const auto phi = [&](u32 initial, usize& slot) {
            // OpPhi words: opcode, type, id, (initial, pre), (next, cont).
            slot = m_b.function_word_count() + 5u;
            return m_b.emit_phi(m_uint, {{initial, pre}, {initial, continue_block}});
        };
        const U counter = phi(k(0), back_edges[N]);
        std::array<U, N> state{};
        for (std::size_t i = 0; i < N; ++i) {
            state[i] = phi(init[i], back_edges[i]);
        }
        // OpLoopMerge must immediately precede the header's branch.
        const B go = ult(counter, k(count));
        m_b.emit_loop_merge(merge, continue_block);
        m_b.emit_branch_conditional(go, body_block, merge);
        m_b.emit_label_id(body_block);
        m_block = body_block;
        std::array<U, N> next = state;
        body(next);
        m_b.emit_branch(continue_block);
        m_b.emit_label_id(continue_block);
        const U counter_next = add(counter, k(1));
        m_b.emit_branch(header);
        for (std::size_t i = 0; i < N; ++i) {
            m_b.patch_function_word(back_edges[i], next[i]);
        }
        m_b.patch_function_word(back_edges[N], counter_next);
        m_b.emit_label_id(merge);
        m_block = merge;
        return state;
    }

public:
    /// The block being emitted (a loop moves it): the predecessor an OpPhi
    /// after this machine's code names.
    [[nodiscard]] u32 block() const { return m_block; }

private:
    B compare(SpvOp op, U a, U b) {
        return m_b.emit_integer_compare(op, m_bool, a, b);
    }

    SpirvBuilder& m_b;
    u32 m_uint;
    u32 m_bool;
    u32 m_pair;
    u32 m_glsl;
    u32 m_block;  // the block being emitted (a loop's pre-header)
};

}  // namespace vitreo::graphics::dxbc_spirv_detail
