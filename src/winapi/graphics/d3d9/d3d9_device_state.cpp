// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_state.cpp
/// @brief IDirect3DDevice9 fixed-function and rasterizer state setters and
///        getters.
///
/// Vtable slots 44-58, 62, 63, 75 and 76 of IDirect3DDevice9, routed here
/// from `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <array>

namespace vitreo::winapi {

/// IDirect3DDevice9 vtable slots: transforms, viewport, material, lights,
/// clip planes, render state, clip status and the scissor rectangle.
void D3D9Manager::dispatch_device_state(Marshaler& m, usize slot,
                                        DeviceState& state) {
    switch (slot) {
    case 44: { // SetTransform
        const u32 transform_state = static_cast<u32>(m.arg(1));
        const GuestAddr matrix_in = m.arg_ptr(2);
        if (matrix_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<float, 16> matrix{};
        if (m.memory().read_bytes(
                matrix_in, reinterpret_cast<Byte*>(matrix.data()),
                sizeof(matrix)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.transforms[transform_state] = matrix;
        state.ff_uniform_dirty = true;
        state.invalidate_draw_pipeline_reuse();
        if (state.recording_state_block) {
            state.recording_block.has_transforms = true;
            state.recording_block.transforms[transform_state] = matrix;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 45: { // GetTransform
        const u32 transform_state = static_cast<u32>(m.arg(1));
        const GuestAddr matrix_out = m.arg_ptr(2);
        if (matrix_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Every transform starts as the identity matrix.
        std::array<float, 16> matrix{1.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 1.0f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 1.0f};
        const auto found = state.transforms.find(transform_state);
        if (found != state.transforms.end()) matrix = found->second;
        if (m.memory().write_bytes(
                matrix_out, reinterpret_cast<const Byte*>(matrix.data()),
                sizeof(matrix)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 46: { // MultiplyTransform
        // Learn: multiplication order is pMatrix times State (left-multiply).
        // https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-multiplytransform
        const u32 transform_state = static_cast<u32>(m.arg(1));
        const GuestAddr matrix_in = m.arg_ptr(2);
        if (matrix_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<float, 16> rhs{};
        if (m.memory().read_bytes(
                matrix_in, reinterpret_cast<Byte*>(rhs.data()),
                sizeof(rhs)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Unset transforms are identity, same as GetTransform.
        std::array<float, 16> current{1.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f};
        const auto found = state.transforms.find(transform_state);
        if (found != state.transforms.end()) current = found->second;
        // Row-major 4x4: result = pMatrix * current.
        std::array<float, 16> result{};
        for (usize row = 0; row < 4; ++row) {
            for (usize column = 0; column < 4; ++column) {
                float sum = 0.0f;
                for (usize k = 0; k < 4; ++k) {
                    sum += rhs[row * 4 + k] * current[k * 4 + column];
                }
                result[row * 4 + column] = sum;
            }
        }
        state.transforms[transform_state] = result;
        if (state.recording_state_block) {
            state.recording_block.has_transforms = true;
            state.recording_block.transforms[transform_state] = result;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 47: { // SetViewport
        const GuestAddr viewport_in = m.arg_ptr(1);
        if (viewport_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 6> viewport{};
        if (m.memory().read_bytes(
                viewport_in, reinterpret_cast<Byte*>(viewport.data()),
                D3DVIEWPORT9_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Windows rejects a viewport that leaves the *current* render target.
        // Do not clamp-and-store an invalid region; a later successful
        // SetRenderTarget call establishes the new target's full viewport.
        u32 rt_width = state.back_buffer_width();
        u32 rt_height = state.back_buffer_height();
        if (state.render_target_surface != 0 &&
            state.render_target_surface != state.back_buffer_surface) {
            ComObject* rt =
                find(state.render_target_surface, D3D9InterfaceId::Surface);
            if (rt != nullptr && rt->surface) {
                rt_width = rt->surface->width;
                rt_height = rt->surface->height;
            }
        }
        const u64 right = static_cast<u64>(viewport[0]) + viewport[2];
        const u64 bottom = static_cast<u64>(viewport[1]) + viewport[3];
        if (right > rt_width || bottom > rt_height) {
            static u32 vp_reject = 0;
            ++vp_reject;
            if (vp_reject <= 16 || (vp_reject & 0xFFu) == 0) {
                VITREO_TRACE("[d3d9] SetViewport INVALIDCALL #%u "
                             "req=%u,%u %ux%u rt=%ux%u\n",
                             vp_reject, viewport[0], viewport[1], viewport[2],
                             viewport[3], rt_width, rt_height);
            }
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.viewport = viewport;
        state.vs_const_dirty = true;
        state.ff_uniform_dirty = true;
        if (state.recording_state_block) {
            state.recording_block.has_viewport = true;
            state.recording_block.viewport = viewport;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 48: { // GetViewport
        const GuestAddr viewport_out = m.arg_ptr(1);
        if (viewport_out == 0 ||
            m.memory().write_bytes(
                viewport_out,
                reinterpret_cast<const Byte*>(state.viewport.data()),
                D3DVIEWPORT9_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 49: { // SetMaterial
        const GuestAddr material_in = m.arg_ptr(1);
        if (material_in == 0 ||
            m.memory().read_bytes(material_in, state.material.data(),
                                  state.material.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.ff_uniform_dirty = true;
        state.invalidate_draw_pipeline_reuse();
        m.set_return(D3D_OK);
        return;
    }

    case 50: { // GetMaterial
        const GuestAddr material_out = m.arg_ptr(1);
        if (material_out == 0 ||
            m.memory().write_bytes(material_out, state.material.data(),
                                   state.material.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 51: { // SetLight
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr light_in = m.arg_ptr(2);
        if (light_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u8, D3DLIGHT9_SIZE> light{};
        if (m.memory().read_bytes(light_in, light.data(),
                                  light.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.lights[index] = light;
        // A light that has never been enabled defaults to disabled.
        state.light_enabled.emplace(index, false);
        state.ff_uniform_dirty = true;
        state.invalidate_draw_pipeline_reuse();
        m.set_return(D3D_OK);
        return;
    }

    case 52: { // GetLight
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr light_out = m.arg_ptr(2);
        if (light_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Never-SetLight indices still expose Learn LightEnable defaults.
        const auto found = state.lights.find(index);
        const auto light = (found != state.lights.end())
                               ? found->second
                               : default_d3dlight9();
        if (m.memory().write_bytes(light_out, light.data(), light.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 53: { // LightEnable
        const u32 index = static_cast<u32>(m.arg(1));
        const u32 enable = static_cast<u32>(m.arg(2));
        // Learn: LightEnable on an unset index creates the default D3DLIGHT9.
        if (state.lights.find(index) == state.lights.end()) {
            state.lights[index] = default_d3dlight9();
        }
        state.light_enabled[index] = enable != 0;
        state.ff_uniform_dirty = true;
        state.invalidate_draw_pipeline_reuse();
        m.set_return(D3D_OK);
        return;
    }

    case 54: { // GetLightEnable
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const auto found = state.light_enabled.find(index);
        if (out == 0 || found == state.light_enabled.end()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.write_u32(out, found->second ? 1u : 0u).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 55: { // SetClipPlane
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr plane_in = m.arg_ptr(2);
        if (index >= D3D9_MAX_CLIP_PLANES || plane_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory().read_bytes(
                plane_in,
                reinterpret_cast<Byte*>(state.clip_planes[index].data()),
                sizeof(float) * 4).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 56: { // GetClipPlane
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr plane_out = m.arg_ptr(2);
        if (index >= D3D9_MAX_CLIP_PLANES || plane_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory().write_bytes(
                plane_out,
                reinterpret_cast<const Byte*>(state.clip_planes[index].data()),
                sizeof(float) * 4).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 57: { // SetRenderState
        const u32 render_state = static_cast<u32>(m.arg(1));
        const u32 value = static_cast<u32>(m.arg(2));
        record_d3d9_state_use(D3D9StateKind::RenderState, render_state,
                              value);
        const auto found = state.render_states.find(render_state);
        if (found == state.render_states.end() || found->second != value) {
            state.render_states[render_state] = value;
            state.invalidate_draw_pipeline_reuse();
            state.ff_uniform_dirty = true;
        }
        if (state.recording_state_block) {
            state.recording_block.has_render_states = true;
            state.recording_block.render_states[render_state] = value;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 58: { // GetRenderState
        const u32 render_state = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto found = state.render_states.find(render_state);
        const u32 value =
            found != state.render_states.end()
                ? found->second
                : d3d9_default_render_state(
                      render_state,
                      state.present_field(PP_ENABLE_AUTO_DEPTH_STENCIL) != 0);
        if (m.write_u32(out, value).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 62: { // SetClipStatus
        const GuestAddr status_in = m.arg_ptr(1);
        if (status_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 2> status{};
        if (m.memory()
                .read_bytes(status_in, reinterpret_cast<Byte*>(status.data()),
                            D3DCLIPSTATUS9_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.clip_status_union = status[0];
        state.clip_status_intersection = status[1];
        m.set_return(D3D_OK);
        return;
    }

    case 63: { // GetClipStatus
        const GuestAddr status_out = m.arg_ptr(1);
        if (status_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const std::array<u32, 2> status{state.clip_status_union,
                                        state.clip_status_intersection};
        if (m.memory()
                .write_bytes(status_out,
                             reinterpret_cast<const Byte*>(status.data()),
                             D3DCLIPSTATUS9_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 75: { // SetScissorRect
        const GuestAddr rect_in = m.arg_ptr(1);
        if (rect_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<i32, 4> rect{};
        if (m.memory().read_bytes(rect_in,
                                  reinterpret_cast<Byte*>(rect.data()),
                                  sizeof(rect)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (rect[0] > rect[2] || rect[1] > rect[3]) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.scissor = rect;
        if (state.recording_state_block) {
            state.recording_block.has_scissor = true;
            state.recording_block.scissor = rect;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 76: { // GetScissorRect
        const GuestAddr rect_out = m.arg_ptr(1);
        if (rect_out == 0 ||
            m.memory().write_bytes(
                rect_out, reinterpret_cast<const Byte*>(state.scissor.data()),
                sizeof(state.scissor)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
