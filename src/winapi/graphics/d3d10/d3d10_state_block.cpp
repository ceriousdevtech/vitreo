// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_state_block.cpp
/// @brief ID3D10StateBlock: D3D10CreateStateBlock, Capture and Apply
///        through the immediate context. Split out of d3d10_device.cpp
///        as verbatim code motion.

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
// ID3D10StateBlock
// -----------------------------------------------------------------------------

void D3D11Manager::impl_D3D10CreateStateBlock(Marshaler& m) {
    // D3D10CreateStateBlock(pDevice, pStateBlockMask, ppStateBlock).
    const GuestAddr device_facet = m.arg_ptr(0);
    const GuestAddr mask = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    InterfaceType type{};
    Device* device = find_device(device_facet, &type);
    if (out != 0) (void)m.write_ptr(out, 0);
    D3D10StateBlock block{};
    if (device == nullptr || type != InterfaceType::D3D10Device || out == 0 ||
        mask == 0 ||
        m_memory.read_bytes(mask, block.mask.data(), block.mask.size())
            .is_err()) {
        m.set_return(kEInvalidArg);
        return;
    }
    const GuestAddr address = allocate_object(m_d3d10_state_block_vtable);
    if (address == 0) {
        m.set_return(kEOutOfMemory);
        return;
    }
    block.device_owner = m_interfaces.at(device_facet).owner;
    // The block holds its device.
    (void)add_ref(device_facet);
    m_d3d10_state_blocks.emplace(address, std::move(block));
    (void)m.write_ptr(out, address);
    m.set_return(kSOk);
}

void D3D11Manager::d3d10_state_block_release_objects(D3D10StateBlock& block) {
    if (!block.state) return;
    const std::shared_ptr<ContextBindings> state = std::move(block.state);
    block.captured = false;
    release_bound_objects(*state);
}

void D3D11Manager::d3d10_state_block_capture(D3D10StateBlock& block,
                                             GuestAddr device_owner) {
    // Copy what the mask names out of the immediate context's bindings,
    // with a reference on each object (measured: Capture takes the VS count
    // 1 -> 2), and nothing else.
    d3d10_state_block_release_objects(block);
    const auto device = m_devices.find(device_owner);
    if (device == m_devices.end()) return;
    const ContextBindings& live =
        m_context_states[device->second.context_interface];
    auto state = std::make_shared<ContextBindings>();
    const u8* mask = block.mask.data();
    const auto bit = [mask](u32 type, u32 index) {
        return d3d10::state_mask_bit(mask, type, index);
    };
    if (bit(1, 0)) {  // SO buffers
        state->stream_out_targets = live.stream_out_targets;
        state->d3d10_stream_out_count = live.d3d10_stream_out_count;
    }
    if (bit(2, 0)) {  // render targets and the depth-stencil view
        state->render_targets = live.render_targets;
        state->depth_stencil_view = live.depth_stencil_view;
    }
    if (bit(3, 0)) {
        state->depth_stencil_state = live.depth_stencil_state;
        state->stencil_ref = live.stencil_ref;
    }
    if (bit(4, 0)) {
        state->blend_state = live.blend_state;
        state->blend_factor = live.blend_factor;
        state->sample_mask = live.sample_mask;
    }
    struct Stage {
        u32 shader_type;
        GuestAddr ContextBindings::*shader;
        d3d11::SamplerSlots ContextBindings::*samplers;
        d3d11::SrvSlots ContextBindings::*resources;
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            ContextBindings::*constants;
    };
    static constexpr std::array<Stage, 3> stages{{
        {5, &ContextBindings::vertex_shader, &ContextBindings::vertex_samplers,
         &ContextBindings::vertex_shader_resources,
         &ContextBindings::vertex_constant_buffers},
        {9, &ContextBindings::geometry_shader,
         &ContextBindings::geometry_samplers,
         &ContextBindings::geometry_shader_resources,
         &ContextBindings::geometry_constant_buffers},
        {13, &ContextBindings::pixel_shader, &ContextBindings::pixel_samplers,
         &ContextBindings::pixel_shader_resources,
         &ContextBindings::pixel_constant_buffers},
    }};
    for (const Stage& stage : stages) {
        if (bit(stage.shader_type, 0)) {
            state.get()->*stage.shader = live.*stage.shader;
        }
        for (u32 i = 0; i < 16; ++i) {
            if (bit(stage.shader_type + 1, i)) {
                (state.get()->*stage.samplers).set(i, (live.*stage.samplers)[i]);
            }
        }
        for (u32 i = 0; i < 128; ++i) {
            if (bit(stage.shader_type + 2, i)) {
                (state.get()->*stage.resources)
                    .set(i, (live.*stage.resources)[i]);
            }
        }
        for (u32 i = 0; i < 14; ++i) {
            if (bit(stage.shader_type + 3, i)) {
                (state.get()->*stage.constants)[i] =
                    (live.*stage.constants)[i];
            }
        }
    }
    for (u32 i = 0; i < 16; ++i) {
        if (bit(17, i)) {
            state->vertex_buffers.set(i, live.vertex_buffers[i]);
            state->vertex_buffer_strides[i] = live.vertex_buffer_strides[i];
            state->vertex_buffer_offsets[i] = live.vertex_buffer_offsets[i];
        }
    }
    if (bit(18, 0)) {
        state->index_buffer = live.index_buffer;
        state->index_buffer_format = live.index_buffer_format;
        state->index_buffer_offset = live.index_buffer_offset;
    }
    if (bit(19, 0)) state->input_layout = live.input_layout;
    if (bit(20, 0)) state->primitive_topology = live.primitive_topology;
    if (bit(21, 0)) {
        state->viewport_count = live.viewport_count;
        state->viewports = live.viewports;
    }
    if (bit(22, 0)) {
        state->scissor_rect_count = live.scissor_rect_count;
        state->scissor_rects = live.scissor_rects;
    }
    if (bit(23, 0)) state->rasterizer_state = live.rasterizer_state;
    if (bit(24, 0)) {
        state->predicate = live.predicate;
        state->predicate_value = live.predicate_value;
    }
    retain_bound_objects(*state);
    block.state = std::move(state);
    block.captured = true;
}

void D3D11Manager::d3d10_state_block_apply(const D3D10StateBlock& block,
                                           GuestAddr device_owner) {
    // Each masked entry goes back through the setter the D3D10 device would
    // use (the immediate context's, with the objects themselves).
    const auto device = m_devices.find(device_owner);
    if (device == m_devices.end() || !block.state) return;
    const GuestAddr context = device->second.context_interface;
    const ContextBindings& state = *block.state;
    const u8* mask = block.mask.data();
    const auto bit = [mask](u32 type, u32 index) {
        return d3d10::state_mask_bit(mask, type, index);
    };
    const usize pointer = m_is_64bit ? 8u : 4u;
    const usize mark = d3d10_scratch_mark();
    const auto ctx = [&](usize slot, std::initializer_list<u64> args) {
        (void)d3d10_forward(&D3D11Manager::impl_ContextMethod, slot, context,
                            args);
    };
    const auto one = [&](GuestAddr value) -> u64 {
        const GuestAddr cell = d3d10_scratch(8);
        if (cell != 0) (void)write_pointer(cell, value);
        return cell;
    };
    const auto words = [&](std::span<const u32> values) -> u64 {
        const GuestAddr cells = d3d10_scratch(std::max<usize>(
            values.size() * 4u, 4u));
        if (cells != 0) {
            (void)m_memory.write_bytes(
                cells, reinterpret_cast<const u8*>(values.data()),
                values.size() * 4u);
        }
        return cells;
    };
    const auto pointers = [&](std::span<const GuestAddr> values) -> u64 {
        const GuestAddr cells =
            d3d10_scratch(std::max<usize>(values.size(), 1) * pointer);
        for (usize i = 0; cells != 0 && i < values.size(); ++i) {
            (void)write_pointer(cells + i * pointer, values[i]);
        }
        return cells;
    };
    // Shaders, samplers, resources and constant buffers per stage: the
    // D3D11 setter slots (shader, samplers, SRVs, CBs).
    struct StageSlots {
        u32 type;
        usize shader, samplers, resources, constants;
        GuestAddr ContextBindings::*bound_shader;
        d3d11::SamplerSlots ContextBindings::*bound_samplers;
        d3d11::SrvSlots ContextBindings::*bound_resources;
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            ContextBindings::*bound_constants;
    };
    static constexpr std::array<StageSlots, 3> stages{{
        {5, 11, 26, 25, 7, &ContextBindings::vertex_shader,
         &ContextBindings::vertex_samplers,
         &ContextBindings::vertex_shader_resources,
         &ContextBindings::vertex_constant_buffers},
        {9, 23, 32, 31, 22, &ContextBindings::geometry_shader,
         &ContextBindings::geometry_samplers,
         &ContextBindings::geometry_shader_resources,
         &ContextBindings::geometry_constant_buffers},
        {13, 9, 10, 8, 16, &ContextBindings::pixel_shader,
         &ContextBindings::pixel_samplers,
         &ContextBindings::pixel_shader_resources,
         &ContextBindings::pixel_constant_buffers},
    }};
    for (const StageSlots& stage : stages) {
        if (bit(stage.type, 0)) ctx(stage.shader, {state.*stage.bound_shader, 0, 0});
        for (u32 i = 0; i < 16; ++i) {
            if (bit(stage.type + 1, i)) {
                ctx(stage.samplers, {i, 1, one((state.*stage.bound_samplers)[i])});
            }
        }
        for (u32 i = 0; i < 128; ++i) {
            if (bit(stage.type + 2, i)) {
                ctx(stage.resources,
                    {i, 1, one((state.*stage.bound_resources)[i])});
            }
        }
        for (u32 i = 0; i < 14; ++i) {
            if (bit(stage.type + 3, i)) {
                ctx(stage.constants,
                    {i, 1, one((state.*stage.bound_constants)[i])});
            }
        }
    }
    for (u32 i = 0; i < 16; ++i) {
        if (bit(17, i)) {
            const u32 stride = state.vertex_buffer_strides[i];
            const u32 offset = state.vertex_buffer_offsets[i];
            ctx(18, {i, 1, one(state.vertex_buffers[i]),
                     words(std::span<const u32>(&stride, 1)),
                     words(std::span<const u32>(&offset, 1))});
        }
    }
    if (bit(18, 0)) {
        ctx(19, {state.index_buffer, state.index_buffer_format,
                 state.index_buffer_offset});
    }
    if (bit(19, 0)) ctx(17, {state.input_layout});
    if (bit(20, 0)) ctx(24, {state.primitive_topology});
    if (bit(2, 0)) {
        ctx(33, {8, pointers(state.render_targets), state.depth_stencil_view});
    }
    if (bit(3, 0)) {
        ctx(36, {state.depth_stencil_state, state.stencil_ref});
    }
    if (bit(4, 0)) {
        std::array<u32, 4> factor{};
        std::memcpy(factor.data(), state.blend_factor.data(), 16);
        ctx(35, {state.blend_state, words(factor), state.sample_mask});
    }
    if (bit(21, 0)) {
        std::array<u32, 16 * 6> floats{};
        std::memcpy(floats.data(), state.viewports.data(), sizeof(floats));
        ctx(44, {state.viewport_count, words(floats)});
    }
    if (bit(22, 0)) {
        std::array<u32, 16 * 4> rects{};
        std::memcpy(rects.data(), state.scissor_rects.data(), sizeof(rects));
        ctx(45, {state.scissor_rect_count, words(rects)});
    }
    if (bit(23, 0)) ctx(43, {state.rasterizer_state});
    if (bit(1, 0)) {
        const std::array<u32, 4> append{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                        0xFFFFFFFFu};
        ctx(37, {4, pointers(state.stream_out_targets), words(append)});
        m_context_states[context].d3d10_stream_out_count =
            state.d3d10_stream_out_count;
    }
    if (bit(24, 0)) ctx(30, {state.predicate, state.predicate_value});
    d3d10_scratch_release(mark);
}

void D3D11Manager::impl_D3D10StateBlockMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto block = m_d3d10_state_blocks.find(self);
    if (block == m_d3d10_state_blocks.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    switch (slot) {
    case 0: {
        // Measured: Windows' state block answers no IID at all -- not even
        // IUnknown or ID3D10StateBlock -- with *ppv NULLed.
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(kEPointer);
            return;
        }
        (void)m.write_ptr(out, 0);
        m.set_return(kENoInterface);
        return;
    }
    case 1:
        m.set_return(++block->second.ref_count);
        return;
    case 2: {
        if (block->second.ref_count == 0) {
            m.set_return(0);
            return;
        }
        const u32 count = --block->second.ref_count;
        if (count == 0) {
            D3D10StateBlock dead = std::move(block->second);
            m_d3d10_state_blocks.erase(block);
            d3d10_state_block_release_objects(dead);
            free_object(self);
            if (const GuestAddr facet = d3d10_device_facet(dead.device_owner)) {
                (void)release(facet);
            }
        }
        m.set_return(count);
        return;
    }
    case 3:  // Capture
        d3d10_state_block_capture(block->second, block->second.device_owner);
        m.set_return(kSOk);
        return;
    case 4:  // Apply: E_FAIL before a Capture and after
             // ReleaseAllDeviceObjects (measured).
        if (!block->second.captured) {
            m.set_return(kEFail);
            return;
        }
        d3d10_state_block_apply(block->second, block->second.device_owner);
        m.set_return(kSOk);
        return;
    case 5:  // ReleaseAllDeviceObjects
        d3d10_state_block_release_objects(block->second);
        m.set_return(kSOk);
        return;
    case 6: {  // GetDevice(ID3D10Device**)
        const GuestAddr out = m.arg_ptr(1);
        const GuestAddr facet = d3d10_device_facet(block->second.device_owner);
        if (out == 0 || facet == 0) {
            m.set_return(kEInvalidArg);
            return;
        }
        (void)add_ref(facet);
        (void)m.write_ptr(out, facet);
        m.set_return(kSOk);
        return;
    }
    default:
        m.set_return(0);
        return;
    }
}

} // namespace vitreo::winapi
