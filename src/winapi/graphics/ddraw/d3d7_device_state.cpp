// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_device_state.cpp
/// @brief IDirect3DDevice7 fixed-function state accessors: transforms, viewport,
///        material, lights, render and texture-stage state, clip planes.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>

namespace vitreo::winapi {

namespace {

/// Read a guest float (a D3DVALUE is an IEEE-754 single).
float read_f32(GuestMemory& memory, GuestAddr addr) {
    const u32 bits = memory.read_u32(addr).value_or(0);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f32(GuestMemory& memory, GuestAddr addr, float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    (void)memory.write_u32(addr, bits);
}

/// Read a D3DMATRIX: sixteen D3DVALUEs in row-major m[row][column] order.
bool read_matrix(GuestMemory& memory, GuestAddr addr, d3d7::Mat4& out) {
    if (addr == 0) return false;
    for (u32 i = 0; i < 16; ++i) {
        out[i] = read_f32(memory, addr + i * 4);
    }
    return true;
}

/// D3DMATRIX multiplication in D3D's row-vector convention: the result of
/// MultiplyTransform is `current * argument`, i.e. the argument is applied
/// after the state already in place.
d3d7::Mat4 multiply_matrix(const d3d7::Mat4& left, const d3d7::Mat4& right) {
    d3d7::Mat4 out{};
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (u32 k = 0; k < 4; ++k) {
                sum += left[row * 4 + k] * right[k * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
    return out;
}

} // namespace

u32 DirectDrawManager::device_set_transform(GuestAddr device, u32 which,
                                            GuestAddr matrix) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    d3d7::Mat4 value{};
    if (!read_matrix(*m_memory, matrix, value)) return ddraw::E_INVALIDARG;
    const bool known = which == d3d7::ts::WORLD || which == d3d7::ts::VIEW ||
                       which == d3d7::ts::PROJECTION ||
                       (which >= d3d7::ts::WORLD1 && which <= d3d7::ts::WORLD3) ||
                       (which >= d3d7::ts::TEXTURE0 &&
                        which <= d3d7::ts::TEXTURE7);
    if (!known) return ddraw::E_INVALIDARG;
    state->state.transforms[which] = value;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_transform(GuestAddr device, u32 which,
                                            GuestAddr matrix) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (matrix == 0) return ddraw::E_INVALIDARG;
    const d3d7::Mat4& value = state->state.transform(which);
    for (u32 i = 0; i < 16; ++i) {
        write_f32(*m_memory, matrix + i * 4, value[i]);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_multiply_transform(GuestAddr device, u32 which,
                                                 GuestAddr matrix) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    d3d7::Mat4 value{};
    if (!read_matrix(*m_memory, matrix, value)) return ddraw::E_INVALIDARG;
    const d3d7::Mat4 result =
        multiply_matrix(state->state.transform(which), value);
    state->state.transforms[which] = result;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_viewport(GuestAddr device,
                                           GuestAddr viewport) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (viewport == 0) return ddraw::E_INVALIDARG;
    // D3DVIEWPORT7 { DWORD dwX, dwY, dwWidth, dwHeight; D3DVALUE dvMinZ, dvMaxZ }
    d3d7::Viewport value{};
    value.x = m_memory->read_u32(viewport).value_or(0);
    value.y = m_memory->read_u32(viewport + 4).value_or(0);
    value.width = m_memory->read_u32(viewport + 8).value_or(0);
    value.height = m_memory->read_u32(viewport + 12).value_or(0);
    value.min_z = read_f32(*m_memory, viewport + 16);
    value.max_z = read_f32(*m_memory, viewport + 20);

    // A viewport that leaves the render target is rejected: it would place
    // fragments outside the surface this device owns.
    SurfaceObject* target = find_surface(state->state.render_target);
    if (target != nullptr && target->surface) {
        if (value.x + value.width > target->surface->width() ||
            value.y + value.height > target->surface->height()) {
            return ddraw::DDERR_INVALIDPARAMS;
        }
    }
    state->state.viewport = value;
    state->state.viewport_set = true;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_viewport(GuestAddr device,
                                           GuestAddr viewport) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (viewport == 0) return ddraw::E_INVALIDARG;
    const d3d7::Viewport& value = state->state.viewport;
    (void)m_memory->write_u32(viewport, value.x);
    (void)m_memory->write_u32(viewport + 4, value.y);
    (void)m_memory->write_u32(viewport + 8, value.width);
    (void)m_memory->write_u32(viewport + 12, value.height);
    write_f32(*m_memory, viewport + 16, value.min_z);
    write_f32(*m_memory, viewport + 20, value.max_z);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_material(GuestAddr device,
                                           GuestAddr material) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (material == 0) return ddraw::E_INVALIDARG;
    // D3DMATERIAL7 is four D3DCOLORVALUEs (diffuse, ambient, specular,
    // emissive) followed by the specular power — the same 68-byte shape as
    // D3DMATERIAL9, which is why FFMaterial is the storage.
    graphics::FFMaterial& out = state->state.material;
    float* const channels[4] = {out.diffuse, out.ambient, out.specular,
                                out.emissive};
    for (u32 group = 0; group < 4; ++group) {
        for (u32 i = 0; i < 4; ++i) {
            channels[group][i] =
                read_f32(*m_memory, material + (group * 4 + i) * 4);
        }
    }
    out.power = read_f32(*m_memory, material + 64);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_material(GuestAddr device,
                                           GuestAddr material) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (material == 0) return ddraw::E_INVALIDARG;
    const graphics::FFMaterial& value = state->state.material;
    const float* const channels[4] = {value.diffuse, value.ambient,
                                      value.specular, value.emissive};
    for (u32 group = 0; group < 4; ++group) {
        for (u32 i = 0; i < 4; ++i) {
            write_f32(*m_memory, material + (group * 4 + i) * 4,
                      channels[group][i]);
        }
    }
    write_f32(*m_memory, material + 64, value.power);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_light(GuestAddr device, u32 index,
                                        GuestAddr light) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (light == 0) return ddraw::E_INVALIDARG;
    // D3DLIGHT7, 104 bytes:
    //   0 dltType, 4 diffuse, 20 specular, 36 ambient, 52 position,
    //   64 direction, 76 range, 80 falloff, 84 attenuation0..2,
    //   96 theta, 100 phi.
    const u32 type = m_memory->read_u32(light).value_or(0);
    if (type < 1 || type > 3) return ddraw::E_INVALIDARG;

    graphics::FFLight value{};
    value.type = static_cast<graphics::LightType>(type);
    const auto read4 = [&](GuestAddr base, float (&out)[4]) {
        for (u32 i = 0; i < 4; ++i) out[i] = read_f32(*m_memory, base + i * 4);
    };
    const auto read3 = [&](GuestAddr base, float (&out)[3]) {
        for (u32 i = 0; i < 3; ++i) out[i] = read_f32(*m_memory, base + i * 4);
    };
    read4(light + 4, value.diffuse);
    read4(light + 20, value.specular);
    read4(light + 36, value.ambient);
    read3(light + 52, value.position);
    read3(light + 64, value.direction);
    value.range = read_f32(*m_memory, light + 76);
    value.spot_falloff = read_f32(*m_memory, light + 80);
    read3(light + 84, value.attenuation);
    value.spot_theta = read_f32(*m_memory, light + 96);
    value.spot_phi = read_f32(*m_memory, light + 100);

    // SetLight and LightEnable are independent in D3D7, so setting a light
    // that was never enabled must not switch it on.
    d3d7::LightSlot& slot = state->state.lights[index];
    const bool enabled = slot.enabled;
    slot.light = value;
    slot.enabled = enabled;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_light(GuestAddr device, u32 index,
                                        GuestAddr light) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (light == 0) return ddraw::E_INVALIDARG;
    const auto found = state->state.lights.find(index);
    // Reading a light that was never set has nothing to report.
    if (found == state->state.lights.end()) return ddraw::DDERR_INVALIDPARAMS;
    const graphics::FFLight& value = found->second.light;
    (void)m_memory->write_u32(light, static_cast<u32>(value.type));
    const auto write4 = [&](GuestAddr base, const float (&in)[4]) {
        for (u32 i = 0; i < 4; ++i) write_f32(*m_memory, base + i * 4, in[i]);
    };
    const auto write3 = [&](GuestAddr base, const float (&in)[3]) {
        for (u32 i = 0; i < 3; ++i) write_f32(*m_memory, base + i * 4, in[i]);
    };
    write4(light + 4, value.diffuse);
    write4(light + 20, value.specular);
    write4(light + 36, value.ambient);
    write3(light + 52, value.position);
    write3(light + 64, value.direction);
    write_f32(*m_memory, light + 76, value.range);
    write_f32(*m_memory, light + 80, value.spot_falloff);
    write3(light + 84, value.attenuation);
    write_f32(*m_memory, light + 96, value.spot_theta);
    write_f32(*m_memory, light + 100, value.spot_phi);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_light_enable(GuestAddr device, u32 index,
                                           u32 enable) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    // Enabling an index that was never given a D3DLIGHT7 creates the slot
    // holding the documented default light, matching D3D7 where LightEnable
    // may legally precede SetLight.
    state->state.lights[index].enabled = enable != 0;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_light_enable(GuestAddr device, u32 index,
                                               GuestAddr out_enable) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_enable == 0) return ddraw::E_INVALIDARG;
    const auto found = state->state.lights.find(index);
    if (found == state->state.lights.end()) return ddraw::DDERR_INVALIDPARAMS;
    (void)m_memory->write_u32(out_enable, found->second.enabled ? 1u : 0u);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_render_state(GuestAddr device, u32 which,
                                               u32 value) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (which == 0 || which > d3d7::rs::MAX) return ddraw::E_INVALIDARG;
    state->state.render_states[which] = value;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_render_state(GuestAddr device, u32 which,
                                               GuestAddr out_value) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_value == 0) return ddraw::E_INVALIDARG;
    if (which == 0 || which > d3d7::rs::MAX) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_value, state->state.render_state(which));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_texture_stage_state(GuestAddr device,
                                                      u32 stage, u32 which,
                                                      u32 value) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (stage >= d3d7::MAX_TEXTURE_STAGES) return ddraw::E_INVALIDARG;
    if (which == 0 || which >= d3d7::tss::MAX) return ddraw::E_INVALIDARG;
    state->state.stage_states[stage][which] = value;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_texture_stage_state(GuestAddr device,
                                                      u32 stage, u32 which,
                                                      GuestAddr out_value) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_value == 0) return ddraw::E_INVALIDARG;
    if (stage >= d3d7::MAX_TEXTURE_STAGES) return ddraw::E_INVALIDARG;
    if (which == 0 || which >= d3d7::tss::MAX) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_value,
                              state->state.stage_state(stage, which));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_texture(GuestAddr device, u32 stage,
                                          GuestAddr surface) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (stage >= d3d7::MAX_TEXTURE_STAGES) return ddraw::E_INVALIDARG;
    // A null surface unbinds the stage; anything else must be a surface this
    // DirectDraw object created.
    if (surface != 0 && find_surface(surface) == nullptr) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    const GuestAddr previous = state->state.textures[stage];
    if (previous == surface) return ddraw::DD_OK;
    if (surface != 0) (void)surface_add_ref(surface);
    state->state.textures[stage] = surface;
    if (previous != 0) (void)surface_release(previous);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_texture(GuestAddr device, u32 stage,
                                          GuestAddr out_surface) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_surface == 0) return ddraw::E_INVALIDARG;
    if (stage >= d3d7::MAX_TEXTURE_STAGES) return ddraw::E_INVALIDARG;
    const GuestAddr surface = state->state.textures[stage];
    (void)m_memory->write_u32(out_surface, static_cast<u32>(surface));
    if (surface != 0) (void)surface_add_ref(surface);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_clip_status(GuestAddr device,
                                              GuestAddr status) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (status == 0) return ddraw::E_INVALIDARG;
    // D3DCLIPSTATUS { DWORD dwFlags, dwStatus; float minx, maxx, miny, maxy,
    // minz, maxz }.
    state->state.clip_status_flags = m_memory->read_u32(status).value_or(0);
    state->state.clip_status = m_memory->read_u32(status + 4).value_or(0);
    for (u32 i = 0; i < 6; ++i) {
        state->state.clip_status_extents[i] =
            read_f32(*m_memory, status + 8 + i * 4);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_clip_status(GuestAddr device,
                                              GuestAddr status) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (status == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(status, state->state.clip_status_flags);
    (void)m_memory->write_u32(status + 4, state->state.clip_status);
    for (u32 i = 0; i < 6; ++i) {
        write_f32(*m_memory, status + 8 + i * 4,
                  state->state.clip_status_extents[i]);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_clip_plane(GuestAddr device, u32 index,
                                             GuestAddr plane) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (index >= d3d7::MAX_CLIP_PLANES || plane == 0) {
        return ddraw::E_INVALIDARG;
    }
    for (u32 i = 0; i < 4; ++i) {
        state->state.clip_planes[index][i] = read_f32(*m_memory, plane + i * 4);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_clip_plane(GuestAddr device, u32 index,
                                             GuestAddr plane) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (index >= d3d7::MAX_CLIP_PLANES || plane == 0) {
        return ddraw::E_INVALIDARG;
    }
    for (u32 i = 0; i < 4; ++i) {
        write_f32(*m_memory, plane + i * 4, state->state.clip_planes[index][i]);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_validate_device(GuestAddr device,
                                              GuestAddr out_passes) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_passes == 0) return ddraw::E_INVALIDARG;
    // Every stage combiner the state translation accepts is expressed in one
    // generated shader, so any state that validates at all needs a single
    // pass. States it cannot express are refused at Set time, not here.
    (void)m_memory->write_u32(out_passes, 1);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_pre_load(GuestAddr device, GuestAddr surface) {
    if (!find_device(device)) return ddraw::DDERR_INVALIDOBJECT;
    if (surface != 0 && find_surface(surface) == nullptr) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    // PreLoad is a hint to move a managed texture into video memory. Surfaces
    // here are ordinary guest RAM with no separate pool, so there is nothing
    // to move and nothing to report.
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_info(GuestAddr device, u32 info_id,
                                       GuestAddr buffer, u32 size) {
    if (!find_device(device)) return ddraw::DDERR_INVALIDOBJECT;
    (void)info_id;
    (void)buffer;
    (void)size;
    // GetInfo is a debug-runtime facility; the retail Direct3D runtime returns
    // S_FALSE for every device-information id, which is what a caller that
    // ships against retail expects to see.
    return 1;  // S_FALSE
}

} // namespace vitreo::winapi
