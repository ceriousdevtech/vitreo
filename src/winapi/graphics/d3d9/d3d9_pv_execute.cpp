// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_pv_execute.cpp
/// @brief ProcessVertices CPU interpreter: runs a `PvCpuPlan` for one vertex.

#include "winapi/graphics/d3d9/d3d9_pv_cpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vitreo::winapi::d3d9_pv {

[[nodiscard]] bool run_vs_cpu_process_vertices(
    const PvCpuPlan& plan, const std::array<float, 4>* inputs,
    const float* vs_const_f, const i32* vs_const_i, const u32* vs_const_b,
    std::array<float, 4>& opos,
    std::array<std::array<float, 4>, PV_MAX_COLORS>& ocolor,
    std::array<std::array<float, 4>, PV_MAX_TEXCOORDS>& otexcoord) {
    std::array<std::array<float, 4>, PV_MAX_TEMPS> temps{};
    // aL defaults to 0; loop shadows/restores it (sm3_to_msl); mova fills a0.
    int aL = 0;
    std::array<int, 4> a0{0, 0, 0, 0};
    // Learn / sm3_to_msl: p0 starts false until setp writes it.
    std::array<bool, 4> p0{false, false, false, false};
    opos = {0, 0, 0, 1};
    for (auto& c : ocolor) c = {0, 0, 0, 1};
    for (auto& t : otexcoord) t = {0, 0, 0, 1};

    const auto load_int4 = [&](u32 index) -> std::array<i32, 4> {
        if (index < D3D9_MAX_SHADER_CONST_I && plan.def_int_set[index]) {
            return plan.def_int[index];
        }
        const i32* base = vs_const_i + index * 4;
        return {base[0], base[1], base[2], base[3]};
    };

    struct LoopFrame {
        bool is_loop{false};
        u32 end_pc{0};
        u32 body_pc{0};
        int count{0};
        int iter{0};
        int step{0};
        int saved_aL{0};
    };
    std::vector<LoopFrame> loop_frames;
    std::vector<u32> call_stack;

    const auto compare_comp = [](float left, float right, u8 comp) -> bool {
        switch (comp) {
        case 1: return left > right;
        case 2: return left == right;
        case 3: return left >= right;
        case 4: return left < right;
        case 5: return left != right;
        case 6: return left <= right;
        default: return false;
        }
    };

    const auto read_reg = [&](const PvParam& p,
                              std::array<float, 4>& out) -> bool {
        switch (p.type) {
        case PV_REG_TEMP:
            out = temps[p.index];
            return true;
        case PV_REG_INPUT:
            out = inputs[p.index];
            return true;
        case PV_REG_CONST: {
            u32 index = p.index;
            if (p.relative) {
                const int offset =
                    p.relative_loop
                        ? aL
                        : a0[p.relative_component < 4 ? p.relative_component
                                                      : 0];
                const i64 resolved =
                    static_cast<i64>(p.index) + static_cast<i64>(offset);
                if (resolved < 0 ||
                    resolved >=
                        static_cast<i64>(
                            graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS)) {
                    return false;
                }
                index = static_cast<u32>(resolved);
            }
            if (plan.def_const_set[index]) {
                out = plan.def_const[index];
            } else {
                const float* base = vs_const_f + index * 4;
                out = {base[0], base[1], base[2], base[3]};
            }
            return true;
        }
        default:
            return false;
        }
    };

    const auto swizzle_mod = [&](const PvParam& p,
                                 const std::array<float, 4>& raw)
        -> std::array<float, 4> {
        // Order matches sm3_to_msl: swizzle first, then source modifier.
        std::array<float, 4> value{};
        for (u32 i = 0; i < 4; ++i) {
            value[i] = raw[(p.swizzle >> (i * 2)) & 0x3u];
        }
        switch (p.source_modifier) {
        case PV_SRCMOD_NONE:
            break;
        case PV_SRCMOD_NEG:
            for (float& c : value) c = -c;
            break;
        case PV_SRCMOD_ABS:
            for (float& c : value) c = std::fabs(c);
            break;
        case PV_SRCMOD_ABSNEG:
            for (float& c : value) c = -std::fabs(c);
            break;
        case PV_SRCMOD_BIAS:
            for (float& c : value) c = c - 0.5f;
            break;
        case PV_SRCMOD_BIASNEG:
            for (float& c : value) c = 0.5f - c;
            break;
        case PV_SRCMOD_SIGN: // _bx2
            for (float& c : value) c = (c - 0.5f) * 2.0f;
            break;
        case PV_SRCMOD_SIGNNEG:
            for (float& c : value) c = (0.5f - c) * 2.0f;
            break;
        default:
            break;
        }
        return value;
    };

    const auto write_dst = [&](const PvParam& p,
                               const std::array<float, 4>& value) -> bool {
        std::array<float, 4> stored = value;
        if ((p.result_modifier & PV_DSTMOD_SATURATE) != 0) {
            for (float& c : stored) c = std::clamp(c, 0.0f, 1.0f);
        }
        std::array<float, 4>* dest = nullptr;
        switch (p.type) {
        case PV_REG_TEMP:
            dest = &temps[p.index];
            break;
        case PV_REG_RASTOUT:
            if (p.index != 0) return false;
            dest = &opos;
            break;
        case PV_REG_ATTROUT:
            dest = &ocolor[p.index];
            break;
        case PV_REG_OUTPUT: {
            const auto declared = plan.output_decls.find(p.index);
            if (declared != plan.output_decls.end()) {
                if (declared->second.usage == graphics::Sm3Usage::Position) {
                    dest = &opos;
                } else if (declared->second.usage ==
                           graphics::Sm3Usage::Color) {
                    dest = &ocolor[declared->second.usage_index];
                } else if (declared->second.usage ==
                           graphics::Sm3Usage::TexCoord) {
                    dest = &otexcoord[declared->second.usage_index];
                }
            } else {
                dest = &otexcoord[p.index];
            }
            break;
        }
        default:
            return false;
        }
        if (dest == nullptr) return false;
        for (u32 i = 0; i < 4; ++i) {
            if ((p.write_mask & (1u << i)) != 0) (*dest)[i] = stored[i];
        }
        return true;
    };

    const auto eval_if_condition = [&](const PvParam& src) -> bool {
        bool bit = false;
        if (src.type == PV_REG_PREDICATE) {
            bit = p0[src.swizzle & 0x3u];
        } else if (src.type == PV_REG_CONSTBOOL) {
            if (plan.def_bool_set[src.index]) {
                bit = plan.def_bool[src.index];
            } else {
                bit = vs_const_b[src.index] != 0;
            }
        }
        if (src.source_modifier == PV_SRCMOD_NOT) bit = !bit;
        return bit;
    };

    const auto skip_to_else_or_endif = [&](u32& pc) {
        int depth = 1;
        ++pc;
        while (pc < plan.code.size() && depth > 0) {
            const PvOp op = plan.code[pc].op;
            if (op == PvOp::If) {
                ++depth;
            } else if (op == PvOp::Endif) {
                --depth;
                if (depth == 0) {
                    ++pc; // land after endif
                    return;
                }
            } else if (op == PvOp::Else && depth == 1) {
                ++pc; // enter else body
                return;
            }
            ++pc;
        }
    };

    const auto skip_to_endif = [&](u32& pc) {
        int depth = 1;
        ++pc;
        while (pc < plan.code.size() && depth > 0) {
            const PvOp op = plan.code[pc].op;
            if (op == PvOp::If) {
                ++depth;
            } else if (op == PvOp::Endif) {
                --depth;
            }
            ++pc;
        }
    };

    for (u32 pc = 0; pc < plan.code.size();) {
        const PvInstr& instr = plan.code[pc];

        // Control flow — Learn / sm3_to_msl loop·rep·if (not predicated).
        if (instr.op == PvOp::Loop) {
            const auto i4 = load_int4(instr.int_reg);
            const int count =
                std::clamp(i4[0], 0, static_cast<i32>(PV_MAX_LOOP_COUNT));
            const int init =
                std::clamp(i4[1], 0, static_cast<i32>(PV_MAX_LOOP_COUNT));
            const int step = i4[2];
            LoopFrame frame{};
            frame.is_loop = true;
            frame.end_pc = instr.partner;
            frame.body_pc = pc + 1;
            frame.count = count;
            frame.iter = 0;
            frame.step = step;
            frame.saved_aL = aL;
            aL = init;
            if (count == 0) {
                aL = frame.saved_aL;
                pc = instr.partner + 1;
                continue;
            }
            loop_frames.push_back(frame);
            ++pc;
            continue;
        }
        if (instr.op == PvOp::EndLoop) {
            if (loop_frames.empty() || !loop_frames.back().is_loop) {
                return false;
            }
            LoopFrame& frame = loop_frames.back();
            aL += frame.step;
            ++frame.iter;
            if (frame.iter < frame.count) {
                pc = frame.body_pc;
                continue;
            }
            aL = frame.saved_aL;
            loop_frames.pop_back();
            ++pc;
            continue;
        }
        if (instr.op == PvOp::Rep) {
            const auto i4 = load_int4(instr.int_reg);
            const int count =
                std::clamp(i4[0], 0, static_cast<i32>(PV_MAX_LOOP_COUNT));
            LoopFrame frame{};
            frame.is_loop = false;
            frame.end_pc = instr.partner;
            frame.body_pc = pc + 1;
            frame.count = count;
            frame.iter = 0;
            if (count == 0) {
                pc = instr.partner + 1;
                continue;
            }
            loop_frames.push_back(frame);
            ++pc;
            continue;
        }
        if (instr.op == PvOp::EndRep) {
            if (loop_frames.empty() || loop_frames.back().is_loop) {
                return false;
            }
            LoopFrame& frame = loop_frames.back();
            ++frame.iter;
            if (frame.iter < frame.count) {
                pc = frame.body_pc;
                continue;
            }
            loop_frames.pop_back();
            ++pc;
            continue;
        }
        if (instr.op == PvOp::Break) {
            if (loop_frames.empty()) return false;
            LoopFrame frame = loop_frames.back();
            loop_frames.pop_back();
            if (frame.is_loop) aL = frame.saved_aL;
            pc = instr.partner;
            continue;
        }
        if (instr.op == PvOp::BreakC) {
            // Learn: break_comp — scalar compare (replicate swizzle → [0]).
            if (loop_frames.empty()) return false;
            std::array<float, 4> left_raw{}, right_raw{};
            if (!read_reg(instr.src0, left_raw) ||
                !read_reg(instr.src1, right_raw)) {
                return false;
            }
            const auto left = swizzle_mod(instr.src0, left_raw);
            const auto right = swizzle_mod(instr.src1, right_raw);
            if (compare_comp(left[0], right[0], instr.setp_comp)) {
                LoopFrame frame = loop_frames.back();
                loop_frames.pop_back();
                if (frame.is_loop) aL = frame.saved_aL;
                pc = instr.partner;
            } else {
                ++pc;
            }
            continue;
        }
        if (instr.op == PvOp::BreakP) {
            // Learn: breakp [!]p0.comp
            if (loop_frames.empty()) return false;
            bool bit = p0[instr.src0.swizzle & 0x3u];
            if (instr.src0.source_modifier == PV_SRCMOD_NOT) bit = !bit;
            if (bit) {
                LoopFrame frame = loop_frames.back();
                loop_frames.pop_back();
                if (frame.is_loop) aL = frame.saved_aL;
                pc = instr.partner;
            } else {
                ++pc;
            }
            continue;
        }
        if (instr.op == PvOp::Call || instr.op == PvOp::CallNz) {
            // Learn: push return PC, jump to label (callnz gated on b#/p0).
            bool take_call = true;
            if (instr.op == PvOp::CallNz) {
                take_call = eval_if_condition(instr.src0);
            }
            if (take_call) {
                if (call_stack.size() >= plan.max_call_depth) {
                    return false;
                }
                call_stack.push_back(pc + 1);
                pc = instr.partner;
            } else {
                ++pc;
            }
            continue;
        }
        if (instr.op == PvOp::Ret) {
            // Learn: ret from call → pop; ret from main → end shader.
            if (call_stack.empty()) {
                return true;
            }
            pc = call_stack.back();
            call_stack.pop_back();
            continue;
        }
        if (instr.op == PvOp::Label) {
            // Fall-through past a subroutine definition (sm3 separates bodies).
            if (call_stack.empty()) {
                pc = instr.partner;
            } else {
                ++pc;
            }
            continue;
        }
        if (instr.op == PvOp::If) {
            bool take = false;
            if (instr.setp_comp != 0) {
                // Learn: if_comp — scalar compare (replicate swizzle → [0]).
                std::array<float, 4> left_raw{}, right_raw{};
                if (!read_reg(instr.src0, left_raw) ||
                    !read_reg(instr.src1, right_raw)) {
                    return false;
                }
                const auto left = swizzle_mod(instr.src0, left_raw);
                const auto right = swizzle_mod(instr.src1, right_raw);
                take = compare_comp(left[0], right[0], instr.setp_comp);
            } else {
                take = eval_if_condition(instr.src0);
            }
            if (take) {
                ++pc;
            } else {
                skip_to_else_or_endif(pc);
            }
            continue;
        }
        if (instr.op == PvOp::Else) {
            // True-branch fell into else — skip the false branch.
            skip_to_endif(pc);
            continue;
        }
        if (instr.op == PvOp::Endif) {
            ++pc;
            continue;
        }

        if (instr.predicated) {
            // Match sm3_to_msl: if (any(predicate)) { ... }.
            bool any = false;
            for (u32 i = 0; i < 4; ++i) {
                const u32 comp = (instr.predicate.swizzle >> (i * 2)) & 0x3u;
                bool bit = p0[comp];
                if (instr.predicate.source_modifier == PV_SRCMOD_NOT) {
                    bit = !bit;
                }
                if (bit) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                ++pc;
                continue;
            }
        }

        std::array<float, 4> a_raw{}, b_raw{}, c_raw{};
        if (!read_reg(instr.src0, a_raw)) return false;
        const std::array<float, 4> a = swizzle_mod(instr.src0, a_raw);

        std::array<float, 4> result{};
        switch (instr.op) {
        case PvOp::Mov:
            result = a;
            break;
        case PvOp::Add: {
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
            break;
        }
        case PvOp::Sub: {
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
            break;
        }
        case PvOp::Mul: {
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]};
            break;
        }
        case PvOp::Rcp:
            // Learn: rcp — reciprocal after swizzle; write_mask selects
            // channels. 1/0 → IEEE +inf (matches common GPU paths).
            result = {1.0f / a[0], 1.0f / a[1], 1.0f / a[2], 1.0f / a[3]};
            break;
        case PvOp::Rsq:
            // Learn: rsq — rsqrt(abs(src)) per channel (SM3 takes abs).
            result = {1.0f / std::sqrt(std::fabs(a[0])),
                      1.0f / std::sqrt(std::fabs(a[1])),
                      1.0f / std::sqrt(std::fabs(a[2])),
                      1.0f / std::sqrt(std::fabs(a[3]))};
            break;
        case PvOp::Min: {
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {std::min(a[0], b[0]), std::min(a[1], b[1]),
                      std::min(a[2], b[2]), std::min(a[3], b[3])};
            break;
        }
        case PvOp::Max: {
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {std::max(a[0], b[0]), std::max(a[1], b[1]),
                      std::max(a[2], b[2]), std::max(a[3], b[3])};
            break;
        }
        case PvOp::Dp3: {
            // Learn: dp3 — scalar = xyz dot; replicate to all channels; mask.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            result = {d, d, d, d};
            break;
        }
        case PvOp::Dp4: {
            // Learn: dp4 — scalar = xyzw dot; replicate to all channels; mask.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            const float d =
                a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            result = {d, d, d, d};
            break;
        }
        case PvOp::Mad: {
            if (!read_reg(instr.src1, b_raw) || !read_reg(instr.src2, c_raw)) {
                return false;
            }
            const auto b = swizzle_mod(instr.src1, b_raw);
            const auto c = swizzle_mod(instr.src2, c_raw);
            result = {a[0] * b[0] + c[0], a[1] * b[1] + c[1],
                      a[2] * b[2] + c[2], a[3] * b[3] + c[3]};
            break;
        }
        case PvOp::Lrp: {
            // Learn: lrp - vs / sm3_to_msl mix(src2, src1, src0).
            if (!read_reg(instr.src1, b_raw) || !read_reg(instr.src2, c_raw)) {
                return false;
            }
            const auto b = swizzle_mod(instr.src1, b_raw);
            const auto c = swizzle_mod(instr.src2, c_raw);
            result = {a[0] * (b[0] - c[0]) + c[0], a[1] * (b[1] - c[1]) + c[1],
                      a[2] * (b[2] - c[2]) + c[2], a[3] * (b[3] - c[3]) + c[3]};
            break;
        }
        case PvOp::Cmp: {
            // Learn: cmp — (src0 >= 0) ? src1 : src2; sm3_to_msl select().
            if (!read_reg(instr.src1, b_raw) || !read_reg(instr.src2, c_raw)) {
                return false;
            }
            const auto b = swizzle_mod(instr.src1, b_raw);
            const auto c = swizzle_mod(instr.src2, c_raw);
            result = {a[0] >= 0.0f ? b[0] : c[0], a[1] >= 0.0f ? b[1] : c[1],
                      a[2] >= 0.0f ? b[2] : c[2], a[3] >= 0.0f ? b[3] : c[3]};
            break;
        }
        case PvOp::Matrix: {
            // Learn / sm3_to_msl: each row is src1+i; result[i] = dp3/dp4.
            const u32 comps = instr.matrix_vector_size;
            for (u32 row = 0; row < instr.matrix_row_count; ++row) {
                PvParam row_param = instr.src1;
                row_param.index = instr.src1.index + row;
                std::array<float, 4> row_raw{};
                if (!read_reg(row_param, row_raw)) return false;
                const auto row_v = swizzle_mod(row_param, row_raw);
                float d = 0.0f;
                for (u32 i = 0; i < comps; ++i) {
                    d += a[i] * row_v[i];
                }
                result[row] = d;
            }
            break;
        }
        case PvOp::Sincos: {
            // Learn: sincos - vs — V = scalar after swizzle; .x=cos, .y=sin.
            const float angle = a[0];
            result = {std::cos(angle), std::sin(angle), 0.0f, 0.0f};
            break;
        }
        case PvOp::Abs:
            // Learn: abs — component-wise absolute value.
            result = {std::fabs(a[0]), std::fabs(a[1]), std::fabs(a[2]),
                      std::fabs(a[3])};
            break;
        case PvOp::Frc:
            // Learn: frc — x - floor(x) per channel (fraction in [0, 1)).
            result = {a[0] - std::floor(a[0]), a[1] - std::floor(a[1]),
                      a[2] - std::floor(a[2]), a[3] - std::floor(a[3])};
            break;
        case PvOp::Nrm: {
            // Learn / sm3_to_msl: reciprocal length from xyz; scale all four
            // components (including w). Zero-length xyz → FLT_MAX scale.
            const float length_sq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
            const float scale =
                (length_sq == 0.0f) ? 3.402823466e+38f
                                    : 1.0f / std::sqrt(length_sq);
            result = {a[0] * scale, a[1] * scale, a[2] * scale, a[3] * scale};
            break;
        }
        case PvOp::Sgn: {
            // Learn: sgn - vs / sm3_to_msl sign() — -1 if <0, 0 if ==0, else 1.
            const auto sign_of = [](float v) -> float {
                if (v < 0.0f) return -1.0f;
                if (v == 0.0f) return 0.0f;
                return 1.0f;
            };
            result = {sign_of(a[0]), sign_of(a[1]), sign_of(a[2]),
                      sign_of(a[3])};
            break;
        }
        case PvOp::Slt: {
            // Learn: slt — (src0 < src1) ? 1.0f : 0.0f per channel.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[0] < b[0] ? 1.0f : 0.0f, a[1] < b[1] ? 1.0f : 0.0f,
                      a[2] < b[2] ? 1.0f : 0.0f, a[3] < b[3] ? 1.0f : 0.0f};
            break;
        }
        case PvOp::Sge: {
            // Learn: sge — (src0 >= src1) ? 1.0f : 0.0f per channel.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[0] >= b[0] ? 1.0f : 0.0f, a[1] >= b[1] ? 1.0f : 0.0f,
                      a[2] >= b[2] ? 1.0f : 0.0f, a[3] >= b[3] ? 1.0f : 0.0f};
            break;
        }
        case PvOp::Exp: {
            // Learn / sm3_to_msl: scalar exp2(src.x after swizzle); replicate.
            const float e = std::exp2(a[0]);
            result = {e, e, e, e};
            break;
        }
        case PvOp::Log: {
            // Learn: log2(|src|); sm3_to_msl guards zero with max(abs, 1e-30).
            const float e = std::log2(std::max(std::fabs(a[0]), 1e-30f));
            result = {e, e, e, e};
            break;
        }
        case PvOp::Lit: {
            // Learn: lit - vs — lighting coefficients from N·L / N·H / power.
            constexpr float kMaxPower = 127.9961f;
            const float power = std::clamp(a[3], -kMaxPower, kMaxPower);
            result = {1.0f, 0.0f, 0.0f, 1.0f};
            if (a[0] > 0.0f) {
                result[1] = a[0];
                if (a[1] > 0.0f) {
                    result[2] = std::pow(a[1], power);
                }
            }
            break;
        }
        case PvOp::Dst: {
            // Learn: dst - vs — (1, src0.y*src1.y, src0.z, src1.w).
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {1.0f, a[1] * b[1], a[2], b[3]};
            break;
        }
        case PvOp::Pow: {
            // Learn / sm3_to_msl: scalar pow(abs(src0.x), src1.x); replicate.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            const float e = std::pow(std::fabs(a[0]), b[0]);
            result = {e, e, e, e};
            break;
        }
        case PvOp::Crs: {
            // Learn / sm3_to_msl: right-hand cross of xyz (swizzled srcs).
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            result = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                      a[0] * b[1] - a[1] * b[0], 0.0f};
            break;
        }
        case PvOp::Mova: {
            // Learn: round-to-nearest into integer a0 (write mask selects).
            for (u32 i = 0; i < 4; ++i) {
                if ((instr.dst.write_mask & (1u << i)) == 0) continue;
                a0[i] = static_cast<int>(std::lround(a[i]));
            }
            ++pc;
            continue;
        }
        case PvOp::Setp: {
            // Learn: setp_comp - vs — dst.channel = src0 cmp src1.
            if (!read_reg(instr.src1, b_raw)) return false;
            const auto b = swizzle_mod(instr.src1, b_raw);
            for (u32 i = 0; i < 4; ++i) {
                if ((instr.dst.write_mask & (1u << i)) == 0) continue;
                bool bit = false;
                switch (instr.setp_comp) {
                case 1: bit = a[i] > b[i]; break;   // GT
                case 2: bit = a[i] == b[i]; break;  // EQ
                case 3: bit = a[i] >= b[i]; break;  // GE
                case 4: bit = a[i] < b[i]; break;   // LT
                case 5: bit = a[i] != b[i]; break;  // NE
                case 6: bit = a[i] <= b[i]; break;  // LE
                default: return false;
                }
                p0[i] = bit;
            }
            ++pc;
            continue;
        }
        case PvOp::Loop:
        case PvOp::EndLoop:
        case PvOp::Rep:
        case PvOp::EndRep:
        case PvOp::Break:
        case PvOp::BreakC:
        case PvOp::BreakP:
        case PvOp::Call:
        case PvOp::CallNz:
        case PvOp::Ret:
        case PvOp::Label:
        case PvOp::If:
        case PvOp::Else:
        case PvOp::Endif:
            // Handled above; keep the switch exhaustive for -Wswitch.
            return false;
        }
        if (!write_dst(instr.dst, result)) return false;
        ++pc;
    }
    return true;
}

} // namespace vitreo::winapi::d3d9_pv
