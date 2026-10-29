// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_class_instance_binding.cpp
/// @brief *SetShader / *GetShader with class instances (D3D11.3 7.19.6).
///
/// set_stage_shader and get_stage_shader for the six stages, the
/// per-stage shader records they retain and release, and
/// specialise_shader, which builds a hidden shader for a shader's bound
/// class instances.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// *SetShader / *GetShader with class instances (D3D11.3 7.19.6)
// -----------------------------------------------------------------------------

GuestAddr& D3D11Manager::stage_shader_field(ContextBindings& state,
                                            LinkedStage stage) {
    switch (stage) {
    case LinkedStage::Vertex: return state.vertex_shader;
    case LinkedStage::Pixel: return state.pixel_shader;
    case LinkedStage::Compute: return state.compute_shader;
    case LinkedStage::Geometry: return state.geometry_shader;
    case LinkedStage::Hull: return state.hull_shader;
    case LinkedStage::Domain: break;
    }
    return state.domain_shader;
}

template <typename F>
bool D3D11Manager::visit_stage_record(LinkedStage stage, GuestAddr shader,
                                      F&& f) {
    const auto visit = [&](auto& records) {
        auto it = records.find(shader);
        if (it == records.end()) return false;
        f(it->second);
        return true;
    };
    switch (stage) {
    case LinkedStage::Vertex: return visit(m_vertex_shaders);
    case LinkedStage::Pixel: return visit(m_pixel_shaders);
    case LinkedStage::Compute: return visit(m_compute_shaders);
    case LinkedStage::Geometry: return visit(m_geometry_shaders);
    case LinkedStage::Hull: return visit(m_hull_shaders);
    case LinkedStage::Domain: return visit(m_domain_shaders);
    }
    return false;
}

D3D11Manager::ShaderLinkage* D3D11Manager::stage_shader_linkage(
    LinkedStage stage, GuestAddr shader) {
    ShaderLinkage* linkage = nullptr;
    (void)visit_stage_record(stage, shader,
                             [&](auto& record) { linkage = &record.linkage; });
    return linkage;
}

bool D3D11Manager::retain_stage_shader(LinkedStage stage, GuestAddr shader) {
    return visit_stage_record(stage, shader,
                              [](auto& record) { ++record.ref_count; });
}

void D3D11Manager::release_stage_shader(LinkedStage stage, GuestAddr shader) {
    switch (stage) {
    case LinkedStage::Vertex: (void)release_vertex_shader(shader); return;
    case LinkedStage::Pixel: (void)release_pixel_shader(shader); return;
    case LinkedStage::Compute: (void)release_compute_shader(shader); return;
    case LinkedStage::Geometry: (void)release_geometry_shader(shader); return;
    case LinkedStage::Hull: (void)release_hull_shader(shader); return;
    case LinkedStage::Domain: (void)release_domain_shader(shader); return;
    }
}

void D3D11Manager::unbind_stage_shader(ContextBindings& state,
                                       LinkedStage stage) {
    auto& linked = state.linked[static_cast<usize>(stage)];
    GuestAddr& bound = stage_shader_field(state, stage);
    const GuestAddr shader = linked.shader != 0 ? linked.shader : bound;
    const std::vector<GuestAddr> instances = std::move(linked.instances);
    linked = {};
    bound = 0;
    for (const GuestAddr instance : instances) unhold_object(instance);
    if (shader != 0) release_stage_shader(stage, shader);
}

void D3D11Manager::set_stage_shader(GuestAddr context, LinkedStage stage,
                                    GuestAddr shader, GuestAddr instances,
                                    u32 count) {
    static constexpr const char* kNames[] = {"VS", "PS", "CS",
                                             "GS", "HS", "DS"};
    const char* name = kNames[static_cast<usize>(stage)];
    // Learn *SetShader: "Each interface used by a shader must have a
    // corresponding class instance or the shader will get disabled";
    // ppClassInstances "must match the number of interfaces used in the
    // shader". Measured (d3d11_classlink_probe [get], [bad]): too few, too
    // many, a NULL entry, a class of the wrong interface, an unknown class,
    // an instance name the shader does not declare, and instances with a
    // shader that declares no interfaces all bind no shader at all
    // (*GetShader returns NULL and 0 instances).
    GuestAddr run = shader;
    std::vector<GuestAddr> bound_instances;
    ShaderLinkage* linkage = nullptr;
    if (shader != 0) {
        linkage = stage_shader_linkage(stage, shader);
        if (linkage == nullptr) {
            d3d11_diag("ID3D11DeviceContext::%sSetShader 0x%llx is not a "
                       "shader of that stage this device created; unbinding",
                       name, static_cast<unsigned long long>(shader));
            run = 0;
        }
    }
    const u32 given = instances != 0 ? count : 0u;
    if (run != 0 && given > 256u) {
        // Learn: "The maximum number of instances a shader can have is 256."
        run = 0;
    }
    if (run != 0) {
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < given; ++i) {
            GuestAddr instance = 0;
            (void)read_pointer(instances + i * ptr_size, instance);
            bound_instances.push_back(instance);
        }
    }
    std::vector<u32> key;
    if (run != 0 && linkage->classes == nullptr) {
        if (!bound_instances.empty()) run = 0;
    } else if (run != 0) {
        const auto& classes = *linkage->classes;
        if (bound_instances.size() != classes.slot_count) run = 0;
        u32 first_slot = 0;
        for (const auto& slots : classes.slot_runs) {
            for (u32 slot = first_slot;
                 run != 0 && slot < first_slot + slots.count &&
                 slot < bound_instances.size();
                 ++slot) {
                const auto instance =
                    m_class_instances.find(bound_instances[slot]);
                if (instance == m_class_instances.end()) {
                    run = 0;
                    break;
                }
                // Its class in this program, and its this[] data: what
                // CreateClassInstance was given, or -- for GetClassInstance --
                // where the program placed the named instance (element
                // InstanceIndex of an array: one class stride, texture count
                // and sampler count further on).
                u32 type = graphics::kDxbcClassNone;
                std::array<u32, 4> data{};
                const auto& desc = instance->second.desc;
                if (instance->second.created) {
                    for (u32 t = 0; t < classes.types.size(); ++t) {
                        if (classes.types[t].name == instance->second.type_name) {
                            type = t;
                        }
                    }
                    data = {desc[3], desc[4], desc[5], desc[6]};
                } else {
                    for (const auto& declared : classes.instances) {
                        if (declared.name != instance->second.instance_name ||
                            desc[1] >= declared.count ||
                            declared.type >= classes.types.size()) {
                            continue;
                        }
                        const auto& klass = classes.types[declared.type];
                        const auto at = [](u32 value, u32 step, u32 index) {
                            return value == graphics::kDxbcClassNone
                                ? 0u : value + step * index;
                        };
                        type = declared.type;
                        data = {at(declared.constant_buffer, 0u, 0u),
                                at(declared.cb_offset, klass.cb_stride, desc[1]),
                                at(declared.texture, klass.texture_count,
                                   desc[1]),
                                at(declared.sampler, klass.sampler_count,
                                   desc[1])};
                    }
                }
                const auto entry = std::find(slots.type_ids.begin(),
                                             slots.type_ids.end(), type);
                if (type == graphics::kDxbcClassNone ||
                    entry == slots.type_ids.end()) {
                    run = 0;
                    break;
                }
                key.push_back(slots.table_ids[static_cast<usize>(
                    entry - slots.type_ids.begin())]);
                key.insert(key.end(), data.begin(), data.end());
            }
            first_slot += slots.count;
        }
        if (run != 0) run = specialise_shader(stage, shader, key);
    }

    auto& state = m_context_states[context];
    ContextBindings::LinkedBinding previous_linked =
        std::move(state.linked[static_cast<usize>(stage)]);
    const GuestAddr previous = stage_shader_field(state, stage);
    state.linked[static_cast<usize>(stage)] = {};
    if (run != 0 && run != shader) {
        // The binding holds the application's shader and the instances.
        (void)retain_stage_shader(stage, shader);
        for (const GuestAddr instance : bound_instances) {
            (void)hold_object(instance);
        }
        state.linked[static_cast<usize>(stage)] = {shader, bound_instances};
    } else if (run != 0) {
        (void)retain_stage_shader(stage, shader);
    }
    stage_shader_field(state, stage) = run;
    for (const GuestAddr instance : previous_linked.instances) {
        unhold_object(instance);
    }
    const GuestAddr released =
        previous_linked.shader != 0 ? previous_linked.shader : previous;
    if (released != 0) release_stage_shader(stage, released);
}

void D3D11Manager::get_stage_shader(Marshaler& m, const ContextBindings& state,
                                    LinkedStage stage) {
    // (ppShader, ppClassInstances, pNumClassInstances). Measured
    // (d3d11_classlink_probe [get]): the shader and every instance returned
    // carry a new reference; *pNumClassInstances goes in as the array's
    // capacity and comes back as the number written (min(capacity, bound)),
    // or the number bound when ppClassInstances is NULL; entries past it
    // are untouched.
    const auto& linked = state.linked[static_cast<usize>(stage)];
    const GuestAddr shader = linked.shader != 0
        ? linked.shader
        : stage_shader_field(const_cast<ContextBindings&>(state), stage);
    const GuestAddr shader_out = m.arg_ptr(1);
    const GuestAddr instances_out = m.arg_ptr(2);
    const GuestAddr count_address = m.arg_ptr(3);
    if (shader_out != 0) {
        if (shader != 0) (void)retain_object(shader);
        if (m.write_ptr(shader_out, shader).is_err() && shader != 0) {
            (void)release_object(shader);
        }
    }
    if (count_address == 0) return;
    const u32 bound = static_cast<u32>(linked.instances.size());
    if (instances_out == 0) {
        (void)m_memory.write_u32(count_address, bound);
        return;
    }
    const auto capacity = m_memory.read_u32(count_address);
    const u32 written =
        std::min(bound, capacity.is_ok() ? capacity.value() : 0u);
    const usize ptr_size = m_is_64bit ? 8u : 4u;
    for (u32 i = 0; i < written; ++i) {
        const GuestAddr instance = linked.instances[i];
        if (instance != 0) (void)retain_object(instance);
        (void)write_pointer(instances_out + i * ptr_size, instance);
    }
    (void)m_memory.write_u32(count_address, written);
}

GuestAddr D3D11Manager::specialise_shader(LinkedStage stage, GuestAddr shader,
                                          const std::vector<u32>& key) {
    ShaderLinkage* linkage = stage_shader_linkage(stage, shader);
    if (linkage == nullptr) return 0;
    if (const auto known = linkage->specialisations.find(key);
        known != linkage->specialisations.end()) {
        return known->second;
    }
    std::vector<graphics::DxbcSlotBinding> bindings(key.size() / 5u);
    for (usize slot = 0; slot < bindings.size(); ++slot) {
        bindings[slot].table = key[slot * 5u];
        for (u32 c = 0; c < 4; ++c) {
            bindings[slot].this_data[c] = key[slot * 5u + 1u + c];
        }
    }
    const std::vector<u8> bytes = graphics::dxbc_with_slot_bindings(
        linkage->bytecode.data(), linkage->bytecode.size(), bindings);
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    GuestAddr device_owner = 0;
    (void)visit_stage_record(stage, shader, [&](auto& record) {
        device_owner = record.device_owner;
    });
    GuestAddr hidden = 0;
    std::string why = "translation failed";
    switch (stage) {
    case LinkedStage::Vertex:
        hidden = create_vertex_shader(device_owner, bytes.data(), bytes.size(),
                                      /*hidden=*/true);
        break;
    case LinkedStage::Pixel:
        hidden = create_pixel_shader(device_owner, bytes.data(), bytes.size(),
                                     /*hidden=*/true);
        break;
    case LinkedStage::Compute:
        hidden = create_compute_shader(device_owner, bytes.data(),
                                       bytes.size(), /*hidden=*/true);
        break;
    case LinkedStage::Geometry: {
        GeometryShader copy = m_geometry_shaders.at(shader);
        copy.linkage = {};
        copy.kernels.clear();
        if (copy.program &&
            build_geometry_program(bytes, container, copy.rasterized_stream,
                                   copy, why) != d3d11::S_OK) {
            break;
        }
        if ((hidden = allocate_object(m_geometry_shader_vtable)) == 0) break;
        copy.interface_address = hidden;
        copy.ref_count = 1;
        copy.public_refs = 0;
        m_geometry_shaders.emplace(hidden, std::move(copy));
        break;
    }
    case LinkedStage::Hull:
    case LinkedStage::Domain: {
        const bool hull = stage == LinkedStage::Hull;
        HullShader hull_shader{};
        DomainShader domain_shader{};
        VertexShader domain_vertex{};
        bool hardware_domain = false;
        if (build_tessellation_program(bytes, container, hull, hull_shader,
                                       domain_shader, domain_vertex,
                                       hardware_domain, why) != d3d11::S_OK) {
            break;
        }
        hidden = allocate_object(hull ? m_hull_shader_vtable
                                      : m_domain_shader_vtable);
        if (hidden == 0) break;
        if (hull) {
            hull_shader.interface_address = hidden;
            hull_shader.device_owner = device_owner;
            hull_shader.public_refs = 0;
            m_hull_shaders.emplace(hidden, std::move(hull_shader));
        } else {
            domain_shader.interface_address = hidden;
            domain_shader.device_owner = device_owner;
            domain_shader.public_refs = 0;
            if (hardware_domain) {
                domain_shader.vertex_key = hidden | 1u;
                domain_vertex.interface_address = domain_shader.vertex_key;
                domain_vertex.device_owner = device_owner;
                m_vertex_shaders.emplace(domain_shader.vertex_key,
                                         std::move(domain_vertex));
            }
            m_domain_shaders.emplace(hidden, std::move(domain_shader));
        }
        break;
    }
    }
    if (hidden == 0) {
        // The binding is valid, but this program cannot run with it: the
        // stage stays unbound, loudly.
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED d3d11!*SetShader: a class-"
                     "instance specialisation of shader 0x%llx cannot be "
                     "built (%s)\n",
                     static_cast<unsigned long long>(shader), why.c_str());
        return 0;
    }
    // Look the owner up again: creating the specialisation may have grown
    // the map that holds it.
    linkage = stage_shader_linkage(stage, shader);
    linkage->specialisations.emplace(key, hidden);
    return hidden;
}

void D3D11Manager::destroy_specialisations(LinkedStage stage,
                                           ShaderLinkage& linkage) {
    for (const auto& [_, hidden] : linkage.specialisations) {
        release_stage_shader(stage, hidden);
    }
    linkage.specialisations.clear();
}

}  // namespace vitreo::winapi
