// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_draw_dump.cpp
/// @brief /tmp/vitreo-draw-dump (see d3d11_draw_dump.hpp). Log only.

#include "winapi/graphics/d3d11/d3d11_draw_dump.hpp"

#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace vitreo::winapi {

namespace d3d11 {

namespace {

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

DrawDumpConfig parse_draw_dump_config(std::string_view text) {
    DrawDumpConfig config;
    const auto note_ignored = [&](std::string_view token) {
        if (!config.ignored.empty()) config.ignored += ' ';
        config.ignored.append(token);
    };
    usize pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[pos])) != 0 ||
                text[pos] == ',')) {
            ++pos;
        }
        const usize begin = pos;
        while (pos < text.size() &&
               std::isspace(static_cast<unsigned char>(text[pos])) == 0 &&
               text[pos] != ',') {
            ++pos;
        }
        if (pos == begin) break;
        const std::string_view token = text.substr(begin, pos - begin);
        const usize equals = token.find('=');
        if (equals == std::string_view::npos) {
            note_ignored(token);
            continue;
        }
        const std::string_view key = token.substr(0, equals);
        const std::string_view value = token.substr(equals + 1);
        if (equal_ignoring_case(key, "ps_semantic") && !value.empty()) {
            config.ps_semantic.assign(value);
        } else if (equal_ignoring_case(key, "max") && !value.empty()) {
            // A decimal count; anything else is ignored rather than guessed.
            u64 parsed = 0;
            bool digits = true;
            for (const char c : value) {
                if (std::isdigit(static_cast<unsigned char>(c)) == 0 ||
                    parsed > 0xFFFFFFFFull) {
                    digits = false;
                    break;
                }
                parsed = parsed * 10 + static_cast<u64>(c - '0');
            }
            if (digits && parsed <= 0xFFFFFFFFull) {
                config.max_draws = static_cast<u32>(parsed);
            } else {
                note_ignored(token);
            }
        } else {
            note_ignored(token);
        }
    }
    return config;
}

bool signature_names_semantic(
    std::span<const graphics::DxbcSignatureElement> signature,
    std::string_view semantic) {
    if (semantic.empty()) return false;
    for (const auto& element : signature) {
        if (equal_ignoring_case(element.semantic_name, semantic)) return true;
        // "TEXCOORD3": the name, then the index.
        if (semantic.size() > element.semantic_name.size() &&
            equal_ignoring_case(
                element.semantic_name,
                semantic.substr(0, element.semantic_name.size()))) {
            const std::string index =
                std::to_string(element.semantic_index);
            if (semantic.substr(element.semantic_name.size()) == index) {
                return true;
            }
        }
    }
    return false;
}

bool DrawDumpBudget::take(u64 generation, u32 max) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (generation != m_generation) {
        m_generation = generation;
        m_taken = 0;
    }
    if (m_taken >= max) {
        m_full_generation.store(generation, std::memory_order_relaxed);
        return false;
    }
    ++m_taken;
    if (m_taken >= max) {
        m_full_generation.store(generation, std::memory_order_relaxed);
    }
    return true;
}

u32 DrawDumpBudget::taken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_taken;
}

bool DrawDumpBudget::has_room(u64 generation, u32 max) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return generation != m_generation || m_taken < max;
}

DrawDumpBudget& draw_dump_budget() noexcept {
    static DrawDumpBudget* const budget = new DrawDumpBudget();
    return *budget;
}

}  // namespace d3d11

namespace {

constexpr std::string_view kTag = "draw-dump";

/// The marker's parsed contents, per marker generation.
struct ParsedMarker {
    std::mutex mutex;
    u64 generation{~u64{0}};
    d3d11::DrawDumpConfig config;
};

ParsedMarker& parsed_marker() noexcept {
    static ParsedMarker* const state = new ParsedMarker();
    return *state;
}

std::string address_text(GuestAddr address) {
    return live_diag::strprintf("0x%llx",
                                static_cast<unsigned long long>(address));
}

}  // namespace

void D3D11Manager::log_draw_dump(const ContextState& state,
                                 const DrawDumpCall& call) {
    using live_diag::emit;
    using live_diag::strprintf;
    auto& marker = live_diag::draw_dump_marker();
    const u64 generation = marker.generation();
    // The budget of this marker is spent: one load per draw, no parsing.
    if (d3d11::draw_dump_budget().full(generation)) return;

    // The marker's contents select the draws; parse them once per generation.
    d3d11::DrawDumpConfig config;
    {
        ParsedMarker& parsed = parsed_marker();
        std::lock_guard<std::mutex> lock(parsed.mutex);
        if (parsed.generation != generation) {
            parsed.generation = generation;
            parsed.config = d3d11::parse_draw_dump_config(marker.contents());
            if (!parsed.config.ignored.empty()) {
                emit(kTag, strprintf("ignoring unrecognized marker text: %s "
                                     "(expected ps_semantic=<NAME> max=<N>)",
                                     parsed.config.ignored.c_str()));
            }
        }
        config = parsed.config;
    }
    auto& budget = d3d11::draw_dump_budget();
    if (!budget.has_room(generation, config.max_draws)) return;

    // The pixel shader's input signature decides: parse its bytecode only
    // for a draw that could still be taken.
    const auto ps = m_pixel_shaders.find(state.pixel_shader);
    graphics::DxbcContainer ps_container;
    if (ps != m_pixel_shaders.end()) {
        ps_container =
            graphics::parse_dxbc(ps->second.bytecode.data(),
                                 ps->second.bytecode.size());
    }
    if (!config.ps_semantic.empty()) {
        if (ps == m_pixel_shaders.end() ||
            !d3d11::signature_names_semantic(ps_container.input_signature,
                                             config.ps_semantic)) {
            return;
        }
    }
    if (!budget.take(generation, config.max_draws)) return;
    const u32 number = budget.taken();

    const std::string head = strprintf("draw=%u ", number);
    const auto line = [&](const std::string& text) {
        emit(kTag, head + text);
    };

    // ---- The call.
    {
        std::string text = strprintf(
            "call indexed=%u count=%u start=%u base=%d instances=%u "
            "first_instance=%u topology=%u",
            call.indexed ? 1u : 0u, call.count, call.start_location,
            call.base_vertex, call.instance_count, call.first_instance,
            state.primitive_topology);
        if (call.indirect_args != 0) {
            text += strprintf(" indirect_args=%s indirect_offset=%u",
                              address_text(call.indirect_args).c_str(),
                              call.indirect_offset);
        }
        text += strprintf(" ps_semantic=%s max=%u",
                          config.ps_semantic.empty()
                              ? "(any)" : config.ps_semantic.c_str(),
                          config.max_draws);
        line(text);
    }

    // ---- Shaders and targets. The bytecode's size and FNV-1a identify the
    // program in the log (the environment-variable dump writes the .dxbc).
    const auto dxbc_identity = [](const std::vector<u8>& bytes) {
        u64 fnv = 0xcbf29ce484222325ull;
        for (const u8 byte : bytes) {
            fnv = (fnv ^ byte) * 0x100000001b3ull;
        }
        return strprintf("%zu/fnv1a=0x%016llx", bytes.size(),
                         static_cast<unsigned long long>(fnv));
    };
    const auto vs = m_vertex_shaders.find(state.vertex_shader);
    line(strprintf(
        "shaders vs=%s ps=%s layout=%s vs_dxbc=%s ps_dxbc=%s",
        address_text(state.vertex_shader).c_str(),
        address_text(state.pixel_shader).c_str(),
        address_text(state.input_layout).c_str(),
        vs != m_vertex_shaders.end()
            ? dxbc_identity(vs->second.bytecode).c_str() : "none",
        ps != m_pixel_shaders.end()
            ? dxbc_identity(ps->second.bytecode).c_str() : "none"));
    for (usize slot = 0; slot < state.render_targets.size(); ++slot) {
        if (state.render_targets[slot] == 0) continue;
        const auto rtv = m_render_target_views.find(state.render_targets[slot]);
        if (rtv == m_render_target_views.end()) {
            line(strprintf("rtv%zu view=%s (unknown)", slot,
                           address_text(state.render_targets[slot]).c_str()));
            continue;
        }
        line(strprintf(
            "rtv%zu view=%s resource=%s format=%u dimension=%u mip=%u "
            "slices=%u+%u",
            slot, address_text(state.render_targets[slot]).c_str(),
            address_text(rtv->second.texture != 0 ? rtv->second.texture
                                                  : rtv->second.buffer).c_str(),
            rtv->second.format, rtv->second.view_dimension,
            rtv->second.mip_slice, rtv->second.first_array_slice,
            rtv->second.array_size));
    }
    if (state.depth_stencil_view != 0) {
        const auto dsv = m_depth_stencil_views.find(state.depth_stencil_view);
        if (dsv == m_depth_stencil_views.end()) {
            line(strprintf("dsv view=%s (unknown)",
                           address_text(state.depth_stencil_view).c_str()));
        } else {
            line(strprintf(
                "dsv view=%s resource=%s format=%u dimension=%u flags=0x%x "
                "mip=%u slices=%u+%u",
                address_text(state.depth_stencil_view).c_str(),
                address_text(dsv->second.texture).c_str(), dsv->second.format,
                dsv->second.view_dimension, dsv->second.flags,
                dsv->second.mip_slice, dsv->second.first_array_slice,
                dsv->second.array_size));
        }
    }

    // ---- The pixel shader's input signature (what the filter matched on).
    {
        std::string text = "ps_inputs";
        if (ps == m_pixel_shaders.end()) {
            text += " (no pixel shader bound)";
        } else {
            for (const auto& element : ps_container.input_signature) {
                text += strprintf(" %s%u@v%u/mask=0x%x/sv=%u",
                                  element.semantic_name.c_str(),
                                  element.semantic_index,
                                  element.register_num,
                                  static_cast<u32>(element.component_mask),
                                  element.system_value_type);
            }
        }
        line(text);
    }

    // ---- Viewport and scissor.
    {
        std::string text = strprintf("viewports count=%u", state.viewport_count);
        for (u32 i = 0; i < state.viewport_count && i < state.viewports.size();
             ++i) {
            const auto& v = state.viewports[i];
            text += strprintf(" v%u=%g,%g,%g,%g,%g,%g", i, v[0], v[1], v[2],
                              v[3], v[4], v[5]);
        }
        line(text);
        text = strprintf("scissors count=%u", state.scissor_rect_count);
        for (u32 i = 0;
             i < state.scissor_rect_count && i < state.scissor_rects.size();
             ++i) {
            const auto& r = state.scissor_rects[i];
            text += strprintf(" s%u=%d,%d,%d,%d", i, r[0], r[1], r[2], r[3]);
        }
        line(text);
    }

    // ---- Rasterizer, blend and depth-stencil state.
    {
        const auto rs = m_rasterizer_states.find(state.rasterizer_state);
        if (rs == m_rasterizer_states.end()) {
            line(strprintf("rasterizer state=%s (none: solid, cull back, "
                           "clockwise fronts)",
                           address_text(state.rasterizer_state).c_str()));
        } else {
            const auto& d = rs->second.desc;
            line(strprintf(
                "rasterizer state=%s fill=%u cull=%u front_ccw=%u "
                "depth_bias=%d depth_bias_clamp=%g slope_scaled_bias=%g "
                "depth_clip=%u scissor_enable=%u multisample=%u "
                "antialiased_line=%u forced_sample_count=%u conservative=%u",
                address_text(state.rasterizer_state).c_str(), d.fill_mode,
                d.cull_mode, d.front_counter_clockwise, d.depth_bias,
                static_cast<double>(d.depth_bias_clamp),
                static_cast<double>(d.slope_scaled_depth_bias),
                d.depth_clip_enable, d.scissor_enable, d.multisample_enable,
                d.antialiased_line_enable, d.forced_sample_count,
                d.conservative_raster));
        }
        const auto bs = m_blend_states.find(state.blend_state);
        std::string text = strprintf(
            "blend state=%s factor=%g,%g,%g,%g sample_mask=0x%08x",
            address_text(state.blend_state).c_str(),
            static_cast<double>(state.blend_factor[0]),
            static_cast<double>(state.blend_factor[1]),
            static_cast<double>(state.blend_factor[2]),
            static_cast<double>(state.blend_factor[3]), state.sample_mask);
        if (bs == m_blend_states.end()) {
            text += " (none: blending off, write mask 0xf)";
            line(text);
        } else {
            const auto& d = bs->second.desc;
            text += strprintf(" alpha_to_coverage=%u independent=%u",
                              d.alpha_to_coverage_enable,
                              d.independent_blend_enable);
            line(text);
            const usize targets = d.independent_blend_enable != 0 ? 8 : 1;
            for (usize i = 0; i < targets; ++i) {
                line(strprintf(
                    "blend_rt%zu enable=%u src=%u dest=%u op=%u src_alpha=%u "
                    "dest_alpha=%u op_alpha=%u write_mask=0x%x "
                    "logic_enable=%u logic_op=%u",
                    i, d.blend_enable[i], d.src_blend[i], d.dest_blend[i],
                    d.blend_op[i], d.src_blend_alpha[i], d.dest_blend_alpha[i],
                    d.blend_op_alpha[i], d.render_target_write_mask[i],
                    d.logic_op_enable[i], d.logic_op[i]));
            }
        }
        const auto dss = m_depth_stencil_states.find(state.depth_stencil_state);
        if (dss == m_depth_stencil_states.end()) {
            line(strprintf("depth_stencil state=%s (none: depth LESS, write "
                           "all, stencil off) stencil_ref=%u",
                           address_text(state.depth_stencil_state).c_str(),
                           state.stencil_ref));
        } else {
            const auto& d = dss->second.desc;
            line(strprintf(
                "depth_stencil state=%s depth_enable=%u depth_write=%u "
                "depth_func=%u stencil_enable=%u stencil_ref=%u "
                "stencil_read=0x%x stencil_write=0x%x "
                "front(fail,depth_fail,pass,func)=%u,%u,%u,%u "
                "back=%u,%u,%u,%u",
                address_text(state.depth_stencil_state).c_str(),
                d.depth_enable, d.depth_write_mask, d.depth_func,
                d.stencil_enable, state.stencil_ref, d.stencil_read_mask,
                d.stencil_write_mask, d.front_stencil_fail_op,
                d.front_stencil_depth_fail_op, d.front_stencil_pass_op,
                d.front_stencil_func, d.back_stencil_fail_op,
                d.back_stencil_depth_fail_op, d.back_stencil_pass_op,
                d.back_stencil_func));
        }
    }

    // ---- Input layout, vertex and index buffers.
    {
        const auto layout = m_input_layouts.find(state.input_layout);
        if (layout == m_input_layouts.end()) {
            line("input_layout none");
        } else {
            line(strprintf("input_layout elements=%zu",
                           layout->second.elements.size()));
            for (usize i = 0; i < layout->second.elements.size(); ++i) {
                const auto& e = layout->second.elements[i];
                line(strprintf(
                    "input_element %zu %s%u format=%u slot=%u offset=%u "
                    "class=%u step=%u",
                    i, e.semantic_name.c_str(), e.semantic_index, e.format,
                    e.input_slot, e.aligned_byte_offset, e.input_slot_class,
                    e.instance_data_step_rate));
            }
        }
        for (usize slot = 0; slot < state.vertex_buffers.size(); ++slot) {
            if (state.vertex_buffers[slot] == 0) continue;
            line(strprintf("vertex_buffer slot=%zu buffer=%s stride=%u offset=%u",
                           slot,
                           address_text(state.vertex_buffers[slot]).c_str(),
                           state.vertex_buffer_strides[slot],
                           state.vertex_buffer_offsets[slot]));
        }
        if (state.index_buffer != 0) {
            line(strprintf("index_buffer buffer=%s format=%u offset=%u",
                           address_text(state.index_buffer).c_str(),
                           state.index_buffer_format,
                           state.index_buffer_offset));
        }
    }

    // ---- Constant buffers: every bound slot, its FirstConstant /
    // NumConstants window and the window's bytes. The bytes are the D3D11
    // contract's CPU shadow of the buffer (what Map / UpdateSubresource
    // wrote); `shadow_bytes` shorter than the window means the GPU, not the
    // CPU, produced the rest.
    const auto constant_buffers = [&](const char* stage,
                                      const std::array<GuestAddr,
                                          d3d11::CONSTANT_BUFFER_SLOTS>& bound,
                                      const std::array<u32,
                                          d3d11::CONSTANT_BUFFER_SLOTS>& first,
                                      const std::array<u32,
                                          d3d11::CONSTANT_BUFFER_SLOTS>& count) {
        for (usize slot = 0; slot < bound.size(); ++slot) {
            if (bound[slot] == 0) continue;
            const auto buffer = m_buffers.find(bound[slot]);
            if (buffer == m_buffers.end()) {
                line(strprintf("%s slot=%zu buffer=%s (unknown)", stage, slot,
                               address_text(bound[slot]).c_str()));
                continue;
            }
            const u64 byte_width = buffer->second.desc.byte_width;
            const u64 begin = std::min<u64>(u64{first[slot]} * 16, byte_width);
            const u64 end =
                std::min<u64>(begin + u64{count[slot]} * 16, byte_width);
            const auto& shadow = buffer->second.shadow;
            const u64 have_end = std::min<u64>(end, shadow.size());
            const std::span<const u8> bytes(
                shadow.data() + std::min<u64>(begin, have_end),
                static_cast<usize>(have_end - std::min<u64>(begin, have_end)));
            line(strprintf(
                "%s slot=%zu buffer=%s first_constant=%u num_constants=%u "
                "buffer_bytes=%llu window=%llu..%llu shadow_bytes=%zu hex=%s",
                stage, slot, address_text(bound[slot]).c_str(), first[slot],
                count[slot], static_cast<unsigned long long>(byte_width),
                static_cast<unsigned long long>(begin),
                static_cast<unsigned long long>(end), bytes.size(),
                live_diag::hex_words(bytes).c_str()));
        }
    };
    constant_buffers("vs_cb", state.vertex_constant_buffers,
                     state.vertex_constant_first, state.vertex_constant_count);
    constant_buffers("ps_cb", state.pixel_constant_buffers,
                     state.pixel_constant_first, state.pixel_constant_count);

    // ---- Shader-resource views: resource, format and the viewed range.
    const auto resource_views = [&](const char* stage,
                                    const std::array<GuestAddr,
                                        d3d11::SHADER_RESOURCE_SLOTS>& bound,
                                    const std::array<GuestAddr,
                                        d3d11::SAMPLER_SLOTS>& samplers) {
        for (usize slot = 0; slot < bound.size(); ++slot) {
            if (bound[slot] == 0) continue;
            // The sampler of the same slot number (the environment-variable
            // dump printed it with the pixel stage's views).
            const std::string sampler =
                slot < samplers.size()
                    ? strprintf(" sampler=%s",
                                address_text(samplers[slot]).c_str())
                    : std::string();
            const auto srv = m_shader_resource_views.find(bound[slot]);
            if (srv == m_shader_resource_views.end()) {
                line(strprintf("%s slot=%zu view=%s (unknown)%s", stage, slot,
                               address_text(bound[slot]).c_str(),
                               sampler.c_str()));
                continue;
            }
            const auto& v = srv->second;
            if (v.buffer != 0) {
                line(strprintf(
                    "%s slot=%zu view=%s kind=buffer resource=%s format=%u "
                    "dimension=%u first_element=%u num_elements=%u "
                    "element_bytes=%u flags=0x%x%s",
                    stage, slot, address_text(bound[slot]).c_str(),
                    address_text(v.buffer).c_str(), v.format, v.view_dimension,
                    v.first_element, v.num_elements, v.element_bytes,
                    v.buffer_flags, sampler.c_str()));
                continue;
            }
            std::string size;
            if (const auto texture = m_textures.find(v.texture);
                texture != m_textures.end()) {
                const auto& d = texture->second.desc;
                size = strprintf(" resource_size=%ux%u mips=%u array=%u "
                                 "resource_format=%u",
                                 d.width, d.height, d.mip_levels,
                                 d.array_size, d.format);
            }
            line(strprintf(
                "%s slot=%zu view=%s kind=texture resource=%s format=%u "
                "dimension=%u mips=%u+%u slices=%u+%u%s%s",
                stage, slot, address_text(bound[slot]).c_str(),
                address_text(v.texture).c_str(), v.format, v.view_dimension,
                v.most_detailed_mip, v.mip_levels, v.first_array_slice,
                v.array_size, size.c_str(), sampler.c_str()));
        }
    };
    resource_views("vs_srv", state.vertex_shader_resources.values(),
                   state.vertex_samplers.values());
    resource_views("ps_srv", state.pixel_shader_resources.values(),
                   state.pixel_samplers.values());

    line("end");
}

}  // namespace vitreo::winapi
