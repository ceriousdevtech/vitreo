// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_shaders.cpp
/// @brief IDirect3DDevice9 shader binding and the shader-constant banks.
///
/// Vtable slots 91-99 and 106-114 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>

namespace vitreo::winapi {

/// IDirect3DDevice9 vtable slots: shader creation and binding, and the
/// vertex- and pixel-shader constant banks.
void D3D9Manager::dispatch_device_shaders(Marshaler& m, usize slot,
                                          u32 self, DeviceState& state) {
    switch (slot) {
    case 91: // CreateVertexShader
        create_shader(m, self, false);
        return;
    case 106: // CreatePixelShader
        create_shader(m, self, true);
        return;
    case 92:   // SetVertexShader
    case 107: { // SetPixelShader
        const bool pixel = slot == 107;
        const u32 shader = static_cast<u32>(m.arg(1));
        if (shader != 0 &&
            find(shader, pixel ? D3D9InterfaceId::PixelShader
                               : D3D9InterfaceId::VertexShader) == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        u32& bound = pixel ? state.pixel_shader : state.vertex_shader;
        if (bound == shader) {
            if (state.recording_state_block) {
                if (pixel) {
                    state.recording_block.has_pixel_shader = true;
                    state.recording_block.pixel_shader = shader;
                } else {
                    state.recording_block.has_vertex_shader = true;
                    state.recording_block.vertex_shader = shader;
                }
            }
            m.set_return(D3D_OK);
            return;
        }
        if (shader != 0) (void)add_ref(shader);
        if (bound != 0) (void)release(bound);
        bound = shader;
        if (state.recording_state_block) {
            if (pixel) {
                state.recording_block.has_pixel_shader = true;
                state.recording_block.pixel_shader = shader;
            } else {
                state.recording_block.has_vertex_shader = true;
                state.recording_block.vertex_shader = shader;
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    case 93:   // GetVertexShader
    case 108: { // GetPixelShader
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 shader =
            slot == 93 ? state.vertex_shader : state.pixel_shader;
        if (m.write_u32(out, shader).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 94: // SetVertexShaderConstantF
        set_shader_constant_f(m, state.vs_const_f);
        return;
    case 95: // GetVertexShaderConstantF
        get_shader_constant_f(m, state.vs_const_f);
        return;
    case 109: // SetPixelShaderConstantF
        set_shader_constant_f(m, state.ps_const_f);
        return;
    case 110: // GetPixelShaderConstantF
        get_shader_constant_f(m, state.ps_const_f);
        return;

    case 96: // SetVertexShaderConstantI
        set_shader_constant_i(m, state.vs_const_i);
        return;
    case 97: // GetVertexShaderConstantI
        get_shader_constant_i(m, state.vs_const_i);
        return;
    case 98: // SetVertexShaderConstantB
        set_shader_constant_b(m, state.vs_const_b);
        return;
    case 99: // GetVertexShaderConstantB
        get_shader_constant_b(m, state.vs_const_b);
        return;
    case 111: // SetPixelShaderConstantI
        set_shader_constant_i(m, state.ps_const_i);
        return;
    case 112: // GetPixelShaderConstantI
        get_shader_constant_i(m, state.ps_const_i);
        return;
    case 113: // SetPixelShaderConstantB
        set_shader_constant_b(m, state.ps_const_b);
        return;
    case 114: // GetPixelShaderConstantB
        get_shader_constant_b(m, state.ps_const_b);
        return;
    }
}

void D3D9Manager::set_shader_constant_f(Marshaler& m,
                                        std::array<float, 1024>& store) {
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 vector4_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + vector4_count > D3D9_MAX_SHADER_CONST_F) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (vector4_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().read_bytes(
            data, reinterpret_cast<Byte*>(store.data() + start * 4),
            sizeof(float) * 4 * vector4_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    ComObject* device_entry =
        find(static_cast<u32>(m.arg(0)), D3D9InterfaceId::Device);
    if (device_entry != nullptr && device_entry->device_state) {
        DeviceState& state = *device_entry->device_state;
        const bool vertex = &store == &state.vs_const_f;
        if (vertex) {
            state.vs_const_dirty = true;
        } else {
            state.ps_const_dirty = true;
        }
        if (!state.recording_state_block) {
            m.set_return(D3D_OK);
            return;
        }
        auto& captured = vertex ? state.recording_block.vs_const_f
                                : state.recording_block.ps_const_f;
        auto& included = vertex
                             ? state.recording_block.vs_const_f_included
                             : state.recording_block.ps_const_f_included;
        if (vertex) {
            state.recording_block.has_vs_const_f = true;
        } else {
            state.recording_block.has_ps_const_f = true;
        }
        for (u32 reg = start; reg < start + vector4_count; ++reg) {
            included[reg] = true;
            std::copy_n(store.data() + reg * 4, 4,
                        captured.data() + reg * 4);
        }
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::get_shader_constant_f(
    Marshaler& m, const std::array<float, 1024>& store) {
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 vector4_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + vector4_count > D3D9_MAX_SHADER_CONST_F) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (vector4_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().write_bytes(
            data, reinterpret_cast<const Byte*>(store.data() + start * 4),
            sizeof(float) * 4 * vector4_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::set_shader_constant_i(Marshaler& m,
                                        std::array<i32, 64>& store) {
    // Learn: SetVertexShaderConstantI / SetPixelShaderConstantI — each
    // register is four ints; StartRegister + Vector4iCount must fit the
    // device integer bank (16 on SM 2.0/3.0).
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 vector4_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + vector4_count > D3D9_MAX_SHADER_CONST_I) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (vector4_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().read_bytes(
            data, reinterpret_cast<Byte*>(store.data() + start * 4),
            sizeof(i32) * 4 * vector4_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    ComObject* device_entry =
        find(static_cast<u32>(m.arg(0)), D3D9InterfaceId::Device);
    if (device_entry != nullptr && device_entry->device_state) {
        DeviceState& state = *device_entry->device_state;
        if (&store == &state.vs_const_i) {
            state.vs_const_dirty = true;
        } else {
            state.ps_const_dirty = true;
        }
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::get_shader_constant_i(Marshaler& m,
                                        const std::array<i32, 64>& store) {
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 vector4_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + vector4_count > D3D9_MAX_SHADER_CONST_I) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (vector4_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().write_bytes(
            data, reinterpret_cast<const Byte*>(store.data() + start * 4),
            sizeof(i32) * 4 * vector4_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::set_shader_constant_b(Marshaler& m,
                                        std::array<u32, 16>& store) {
    // Learn: SetVertexShaderConstantB / SetPixelShaderConstantB — BOOL is a
    // 32-bit Windows value; BoolCount is the number of individual registers.
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 bool_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + bool_count > D3D9_MAX_SHADER_CONST_B) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (bool_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().read_bytes(
            data, reinterpret_cast<Byte*>(store.data() + start),
            sizeof(u32) * bool_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    ComObject* device_entry =
        find(static_cast<u32>(m.arg(0)), D3D9InterfaceId::Device);
    if (device_entry != nullptr && device_entry->device_state) {
        DeviceState& state = *device_entry->device_state;
        if (&store == &state.vs_const_b) {
            state.vs_const_dirty = true;
        } else {
            state.ps_const_dirty = true;
        }
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::get_shader_constant_b(Marshaler& m,
                                        const std::array<u32, 16>& store) {
    const u32 start = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 bool_count = static_cast<u32>(m.arg(3));
    if (data == 0 ||
        static_cast<u64>(start) + bool_count > D3D9_MAX_SHADER_CONST_B) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (bool_count == 0) {
        m.set_return(D3D_OK);
        return;
    }
    if (m.memory().write_bytes(
            data, reinterpret_cast<const Byte*>(store.data() + start),
            sizeof(u32) * bool_count).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

} // namespace vitreo::winapi
