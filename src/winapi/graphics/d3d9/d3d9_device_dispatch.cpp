// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_dispatch.cpp
/// @brief IDirect3DDevice9 vtable router.
///
/// Looks the device up once and sends each vtable slot to the translation
/// unit that owns it: the `dispatch_device_*` members in the
/// `d3d9_device_*.cpp` files, `d3d9_state_block.cpp`, `d3d9_vertex_input.cpp`
/// and `d3d9_query.cpp`. Every case label is explicit so the compiler still
/// emits a single jump table.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <array>

namespace vitreo::winapi {

// =============================================================================
// IDirect3DDevice9 dispatch
// =============================================================================

void D3D9Manager::dispatch_device(Marshaler& m, usize slot, u32 self) {
    usize method_count = 0;
    const D3D9MethodDesc* methods =
        d3d9_interface_methods(D3D9InterfaceId::Device, method_count);

    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Device)) return;

    ComObject* entry = find(self, D3D9InterfaceId::Device);
    if (entry == nullptr || !entry->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *entry->device_state;

    const auto not_implemented = [&](const char* contract) {
        VITREO_TRACE("[d3d9] IDirect3DDevice9::%s -> E_NOTIMPL (%s)\n",
                     methods[slot].name, contract);
        m.set_return(E_NOTIMPL_D3D);
    };

    switch (slot) {
    case 3: // TestCooperativeLevel
        m.set_return(state.lost ? D3DERR_DEVICENOTRESET : D3D_OK);
        return;

    case 4: // GetAvailableTextureMem
        // UINT estimate of available texture memory, rounded to the nearest
        // MiB (Microsoft Learn). Populated at CreateDevice from a fixed
        // host-backed pool; not a live free-list walk.
        VITREO_TRACE("[d3d9] GetAvailableTextureMem -> %u bytes (%u MiB)\n",
                     state.available_texture_mem,
                     state.available_texture_mem / (1024u * 1024u));
        m.set_return(state.available_texture_mem);
        return;

    case 5: // EvictManagedResources
        // Nothing is evicted: every managed resource Vitreo holds is already
        // backed by shared-core storage that the backend manages.
        m.set_return(D3D_OK);
        return;

    case 6: { // GetDirect3D
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(state.parent_d3d9);
        if (m.write_u32(out, state.parent_d3d9).is_err()) {
            (void)release(state.parent_d3d9);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 7: { // GetDeviceCaps
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u8, D3DCAPS9_SIZE> caps{};
        fill_d3d9_device_caps(caps, state.device_type, state.adapter);
        if (m.memory().write_bytes(out, caps.data(), caps.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 8: { // GetDisplayMode
        const u32 swap_chain = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        std::array<u32, 4> mode{};
        if (swap_chain != 0 || out == 0 || !current_display_mode(mode) ||
            m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(mode.data()),
                sizeof(mode)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 9: { // GetCreationParameters
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const std::array<u32, 4> creation{state.adapter, state.device_type,
                                          state.focus_window,
                                          state.behavior_flags};
        if (m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(creation.data()),
                D3DDEVICE_CREATION_PARAMETERS_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 10:
    case 11:
    case 12:
    case 19:
    case 20:
    case 21:
    case 22:
        dispatch_device_display(m, slot, state);
        return;

    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
        dispatch_device_swap_chain(m, slot, self, state);
        return;

    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
        dispatch_device_create(m, slot, self);
        return;

    case 37:
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 43:
        dispatch_device_scene(m, slot, state);
        return;

    case 44:
    case 45:
    case 46:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
    case 62:
    case 63:
    case 75:
    case 76:
        dispatch_device_state(m, slot, state);
        return;

    case 59:
    case 60:
    case 61:
        dispatch_device_state_blocks(m, slot, self, state);
        return;

    case 64:
    case 65:
    case 66:
    case 67:
    case 68:
    case 69:
    case 71:
    case 72:
    case 73:
    case 74:
        dispatch_device_texture_state(m, slot, state);
        return;

    case 77:
    case 78:
    case 79:
    case 80:
    case 81:
    case 82:
    case 83:
    case 84:
    case 85:
    case 86:
    case 87:
    case 88:
    case 89:
    case 90:
    case 100:
    case 101:
    case 102:
    case 103:
    case 104:
    case 105:
        dispatch_device_vertex_input(m, slot, self, state);
        return;

    case 91:
    case 92:
    case 93:
    case 94:
    case 95:
    case 96:
    case 97:
    case 98:
    case 99:
    case 106:
    case 107:
    case 108:
    case 109:
    case 110:
    case 111:
    case 112:
    case 113:
    case 114:
        dispatch_device_shaders(m, slot, self, state);
        return;

    case 118:
        dispatch_device_query(m, slot, self);
        return;

    case 70: { // ValidateDevice
        // Learn: IDirect3DDevice9::ValidateDevice — validates that the current
        // device state can be rendered; on success writes the number of
        // passes needed (multipass fallbacks). Vitreo has no multipass path,
        // so accepted state reports 1 pass. Refuse only combinations the draw
        // path already cannot express.
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.lost) {
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        // SetNPatchMode(0) disables; any other segment count enables N-patches,
        // which have no DrawPrimitiveTessellated* implementation.
        if (state.npatch_mode != f32_bits(0.0f)) {
            not_implemented("N-patch / higher-order tessellation has no draw "
                            "path");
            return;
        }
        const auto bound_shader_ready = [&](u32 handle,
                                            D3D9InterfaceId id) -> u32 {
            if (handle == 0) return D3D_OK;
            ComObject* shader_entry = find(handle, id);
            if (shader_entry == nullptr || !shader_entry->shader) {
                return D3DERR_INVALIDCALL;
            }
            // Create*Shader refuses untranslated bytecode; mirror Draw's
            // requirement that a bound programmable stage has usable MSL.
            if (!shader_entry->shader->translated.ok ||
                shader_entry->shader->translated.msl.empty()) {
                return E_NOTIMPL_D3D;
            }
            return D3D_OK;
        };
        const u32 vs_status = bound_shader_ready(
            state.vertex_shader, D3D9InterfaceId::VertexShader);
        if (vs_status != D3D_OK) {
            m.set_return(vs_status);
            return;
        }
        const u32 ps_status = bound_shader_ready(
            state.pixel_shader, D3D9InterfaceId::PixelShader);
        if (ps_status != D3D_OK) {
            m.set_return(ps_status);
            return;
        }
        (void)m.write_u32(out, 1);
        m.set_return(D3D_OK);
        return;
    }

    case 115: not_implemented("no higher-order patch tessellation"); return;
    case 116: not_implemented("no higher-order patch tessellation"); return;
    case 117: not_implemented("no higher-order patch tessellation"); return;

    default:
        not_implemented("unhandled vtable slot");
        return;
    }
}

} // namespace vitreo::winapi
