// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_tessellation.cpp
/// @brief D3D11 hull and domain shaders on Metal (11.17;
///        .docs/design-tessellation-emulation.md).
///
/// Metal has neither stage. A draw with both bound becomes:
///
///   A  capture   the bound vertex shader writing its outputs -- the patches'
///                control points -- into a buffer keyed by vertex id
///                (execute_draw, DrawVariant::Capture);
///   B  hull      the hull shader lowered to a compute kernel, one thread per
///                patch, running every phase and writing output control
///                points, patch constants and tessellation factors
///                (compiled per vertex-shader record layout);
///
/// then one of two tessellators:
///
///   C  patches   (hardware) drawPatches through the domain shader, lowered to
///                a post-tessellation vertex function, with the guest's pixel
///                shader, targets and states (DrawVariant::Tessellated).
///                Only where Metal's tessellator produces exactly D3D's
///                points: tri/quad domains with triangle output and integer
///                partitioning (pow2 too: the hull kernel rounds its factors
///                up to a power of two, 11.7.6), no geometry shader, no
///                stream output, a direct draw.
///   C' software  everything else D3D11 allows (11.7.15): isolines, point and
///                line output, fractional partitioning, stream output from
///                the DS, a geometry shader after the DS, indirect draws. A
///                port of the reference tessellator (graphics/shaders/
///                tessellator.hpp) counts, scans and writes each patch's
///                points and connectivity on the GPU; the domain shader,
///                lowered to a compute kernel, evaluates every point into a
///                record; the records are rasterized through the index list
///                (an indexed indirect draw), streamed out, or handed to the
///                geometry program as its input primitives.
///
/// A draw it cannot serve (a patch size the hull shader does not declare)
/// draws nothing, with a diagnostic.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/tessellator.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

// vitreo_stats_tessellation reads the batch block by these words.
static_assert(graphics::STATS_TESS_BLOCK_INDICES ==
              graphics::tess::TESS_BLOCK_INDICES);
static_assert(graphics::STATS_TESS_BLOCK_PRIMITIVES ==
              graphics::tess::TESS_BLOCK_PRIMITIVES);
static_assert(graphics::STATS_TESS_BLOCK_OVERFLOW ==
              graphics::tess::TESS_BLOCK_OVERFLOW);

// d3d11.h (mingw-w64 SDK).
constexpr GuidBytes IID_ID3D11HULLSHADER{
    0x61, 0x60, 0x5C, 0x8E, 0x8A, 0x62, 0x8E, 0x4C,
    0x82, 0x64, 0xBB, 0xE4, 0x5C, 0xB3, 0xD5, 0xDD};
constexpr GuidBytes IID_ID3D11DOMAINSHADER{
    0x08, 0xC5, 0x82, 0xF5, 0x36, 0x0F, 0x0C, 0x49,
    0x99, 0x77, 0x31, 0xEE, 0xCE, 0x26, 0x8C, 0xFA};

/// Pass buffers larger than this are declined; all scale with guest counts.
constexpr u64 MAX_TESSELLATION_BYTES = 256ull << 20;

/// D3D11_PRIMITIVE_TOPOLOGY_n_CONTROL_POINT_PATCHLIST is 32 + n.
constexpr u32 PATCH_LIST_FIRST = 33;
constexpr u32 PATCH_LIST_LAST = 64;

/// Metal's partitioning for a DXBC one. pow2 is INTEGER: "From the hardware
/// point of view there is no distinction between pow2 and integer - the
/// hardware doesn't do the rounding of the TessFactors to pow2" (D3D11.3
/// 11.7.2; the reference tessellator's HWIntegerPartitioning). Metal's own
/// pow2 mode rounds to powers of two (measured: 15391 px where WARP covers
/// 15228, d3d11_a09b_shader_probe bent patch).
[[nodiscard]] graphics::BackendTessellationPartition partition_of(u32 value) {
    switch (value) {
    case 3: return graphics::BackendTessellationPartition::FractionalOdd;
    case 4: return graphics::BackendTessellationPartition::FractionalEven;
    default: return graphics::BackendTessellationPartition::Integer;
    }
}

}  // namespace

std::string d3d11_hull_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_HullShader_%03zu", slot);
    return name;
}

std::string d3d11_domain_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_DomainShader_%03zu", slot);
    return name;
}

// -----------------------------------------------------------------------------
// Object lifetime and the COM surface (IUnknown + ID3D11DeviceChild)
// -----------------------------------------------------------------------------

u32 D3D11Manager::release_hull_shader(GuestAddr address) {
    auto it = m_hull_shaders.find(address);
    if (it == m_hull_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_hull_shaders.erase(it);
        forget_msl_hashes();
        free_object(address);
        destroy_specialisations(LinkedStage::Hull, linkage);
    }
    return count;
}

u32 D3D11Manager::release_domain_shader(GuestAddr address) {
    auto it = m_domain_shaders.find(address);
    if (it == m_domain_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        if (it->second.vertex_key != 0) {
            m_vertex_shaders.erase(it->second.vertex_key);
            forget_msl_hashes();
        }
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_domain_shaders.erase(it);
        forget_msl_hashes();
        free_object(address);
        destroy_specialisations(LinkedStage::Domain, linkage);
    }
    return count;
}

namespace {

/// The seven methods every D3D11 shader object has, for either map.
template <typename Shader, typename ReadGuid, typename Retain,
          typename Release, typename AddRefDevice, typename PrivateGet,
          typename PrivateSet, typename PrivateSetIface>
void shader_object_method(Marshaler& m, usize slot, GuestAddr self,
                          std::unordered_map<GuestAddr, Shader>& shaders,
                          const GuidBytes& iid_self, ReadGuid read_guid,
                          Retain retain, Release release,
                          AddRefDevice device_of, PrivateGet get_private,
                          PrivateSet set_private,
                          PrivateSetIface set_private_interface) {
    auto it = shaders.find(self);
    if (it == shaders.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, iid_self))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain(self));
        return;
    case 2:
        m.set_return(release(self));
        return;
    case 3:
        device_of(it->second.device_owner, m.arg_ptr(1));
        m.set_return(0);
        return;
    case 4:
        m.set_return(get_private(it->second.private_data));
        return;
    case 5:
        m.set_return(set_private(it->second.private_data));
        return;
    case 6:
        m.set_return(set_private_interface(it->second.private_data));
        return;
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

}  // namespace

void D3D11Manager::impl_HullShaderMethod(Marshaler& m, usize slot) {
    shader_object_method(
        m, slot, m.arg_ptr(0), m_hull_shaders, IID_ID3D11HULLSHADER,
        [this](GuestAddr a, GuidBytes& g) { return read_guid(a, g); },
        [this](GuestAddr a) { return retain_object(a); },
        [this](GuestAddr a) { return release_object(a); },
        [this](GuestAddr owner, GuestAddr out) {
            auto device = m_devices.find(owner);
            if (out == 0 || device == m_devices.end()) return;
            (void)add_ref(device->second.device_interface);
            if (write_pointer(out, device->second.device_interface) == false) {
                (void)release(device->second.device_interface);
            }
        },
        [&](PrivateDataStore& s) { return get_private_data(m, s); },
        [&](PrivateDataStore& s) { return set_private_data(m, s); },
        [&](PrivateDataStore& s) { return set_private_data_interface(m, s); });
}

void D3D11Manager::impl_DomainShaderMethod(Marshaler& m, usize slot) {
    shader_object_method(
        m, slot, m.arg_ptr(0), m_domain_shaders, IID_ID3D11DOMAINSHADER,
        [this](GuestAddr a, GuidBytes& g) { return read_guid(a, g); },
        [this](GuestAddr a) { return retain_object(a); },
        [this](GuestAddr a) { return release_object(a); },
        [this](GuestAddr owner, GuestAddr out) {
            auto device = m_devices.find(owner);
            if (out == 0 || device == m_devices.end()) return;
            (void)add_ref(device->second.device_interface);
            if (write_pointer(out, device->second.device_interface) == false) {
                (void)release(device->second.device_interface);
            }
        },
        [&](PrivateDataStore& s) { return get_private_data(m, s); },
        [&](PrivateDataStore& s) { return set_private_data(m, s); },
        [&](PrivateDataStore& s) { return set_private_data_interface(m, s); });
}

// -----------------------------------------------------------------------------
// CreateHullShader / CreateDomainShader
// -----------------------------------------------------------------------------

u32 D3D11Manager::build_tessellation_program(
    const std::vector<u8>& bytes, const graphics::DxbcContainer& container,
    bool hull, HullShader& hull_shader, DomainShader& domain_shader,
    VertexShader& domain_vertex, bool& hardware_domain, std::string& why) {
    // Lower once on the software tessellator's form, which serves every
    // declaration D3D11 allows (11.7.15: tri, quad and isoline domains;
    // point, line and triangle output): that decides whether the shader can
    // run at all, and reports its declarations.
    graphics::DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    options.domain_to_compute = true;
    const auto lowered = graphics::translate_dxbc_to_spirv(container, options);
    if (!lowered.valid() ||
        !(hull ? lowered.hs_lowered_to_compute
               : lowered.ds_lowered_to_compute)) {
        why = "the tessellation emulation cannot express this shader: " +
              lowered.failure;
        return d3d11::E_NOTIMPL;
    }

    hardware_domain = false;
    if (hull) {
        std::string kernel_msl;
        if (!compile_geometry_kernel(bytes, {}, kernel_msl,
                                     &hull_shader.binding_layout,
                                     /*tess_software=*/true)) {
            why = "the hull kernel does not compile";
            return d3d11::E_NOTIMPL;
        }
        hull_shader.bytecode = bytes;
        hull_shader.domain = lowered.tess_domain;
        hull_shader.partitioning = lowered.tess_partitioning;
        hull_shader.output_primitive = lowered.tess_output_primitive;
        hull_shader.input_control_points = lowered.hs_input_control_points;
        hull_shader.output_control_points = lowered.hs_output_control_points;
        hull_shader.max_tess_factor = lowered.hs_max_tess_factor;
        hull_shader.cp_record_vec4s = lowered.hs_cp_record_vec4s;
        hull_shader.pc_record_vec4s = lowered.hs_pc_record_vec4s;
    } else {
        if (!compile_domain_kernel(bytes, domain_shader.compute_msl,
                                   &domain_shader.compute_layout,
                                   domain_shader.record_registers, why)) {
            return d3d11::E_NOTIMPL;
        }
        domain_shader.bytecode = bytes;
        // The pass-through that rasterizes its records (dense: every record
        // is a point the tessellator made), from the output signature.
        std::vector<graphics::GsPassthroughOutput> outputs;
        for (const auto& element : container.output_signature) {
            const u32 sv = element.system_value_type;
            if (sv == 3u) {  // D3D_NAME_CULL_DISTANCE
                why = "SV_CullDistance output: Metal has no cull distances";
                return d3d11::E_NOTIMPL;
            }
            domain_shader.writes_viewport_index |= sv == 5u;
            auto same = std::find_if(outputs.begin(), outputs.end(),
                                     [&](const auto& o) {
                                         return o.register_num ==
                                                element.register_num;
                                     });
            if (same == outputs.end()) {
                outputs.push_back({element.register_num,
                                   element.component_type, false});
                same = std::prev(outputs.end());
            }
            if (sv == 1u) same->is_position = true;
            if (sv == 2u || sv == 4u || sv == 5u) {
                for (u32 c = 0; c < 4; ++c) {
                    if ((element.component_mask & (1u << c)) != 0u) {
                        same->component_system_values[c] =
                            static_cast<u8>(sv);
                    }
                }
            }
        }
        domain_shader.passthrough_msl =
            graphics::generate_gs_passthrough_vertex_msl(
                outputs, 0, /*fixed_function_push=*/true,
                /*dense_records=*/true);
        domain_shader.stats_clip = stats_clip_layout(
            container.output_signature, domain_shader.record_registers, 0);
        domain_shader.domain = lowered.tess_domain;
        domain_shader.input_control_points = lowered.hs_output_control_points;
        // Metal's tessellator runs tri and quad domains (the fast path).
        if (lowered.tess_domain == 2u || lowered.tess_domain == 3u) {
            std::string hw_why;
            hardware_domain = compile_domain_shader(bytes, domain_vertex.msl_source,
                                                    &domain_vertex.binding_layout,
                                                    hw_why);
            if (!hardware_domain) {
                d3d11_diag("CreateDomainShader: Metal's tessellator cannot run "
                           "it (%s); the software tessellator will",
                           hw_why.c_str());
            }
        }
    }

    return d3d11::S_OK;
}

void D3D11Manager::create_tessellation_shader(Marshaler& m, GuestAddr self,
                                              bool hull) {
    // (pShaderBytecode, BytecodeLength, pClassLinkage, ppShader)
    const GuestAddr bytecode = m.arg_ptr(1);
    const u64 length = m.arg(2);
    const GuestAddr linkage = m.arg_ptr(3);
    const GuestAddr shader_out = m.arg_ptr(4);
    const char* const name = hull ? "CreateHullShader" : "CreateDomainShader";
    if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
    const auto refuse = [&](u32 hr, const std::string& why) {
        d3d11_diag("ID3D11Device::%s -> 0x%08x (%s)", name, hr, why.c_str());
        m.set_return(hr);
    };
    if (bytecode == 0 || length == 0 || length > (1u << 24) ||
        !valid_class_linkage(m_interfaces.find(self)->second.owner,
                             linkage)) {
        refuse(d3d11::E_INVALIDARG, "bad bytecode or class linkage");
        return;
    }
    std::vector<u8> bytes(static_cast<usize>(length));
    if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
        refuse(d3d11::E_INVALIDARG, "bytecode unreadable");
        return;
    }
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    const graphics::ShaderStage wanted =
        hull ? graphics::ShaderStage::Hull : graphics::ShaderStage::Domain;
    if (!container.valid() || container.stage != wanted) {
        refuse(d3d11::E_INVALIDARG, hull ? "not hull-shader bytecode"
                                         : "not domain-shader bytecode");
        return;
    }
    if (const char* why = shader_bytecode_refused(self, bytes.data(),
                                                  bytes.size(), {})) {
        refuse(d3d11::E_INVALIDARG, why);
        return;
    }
    if (uav_requirements_unavailable(self, bytes.data(), bytes.size())) {
        refuse(d3d11::E_INVALIDARG, "declares a UAV below feature level 11_1");
        return;
    }
    // WARP: a shader that declares interfaces needs a class linkage
    // (E_INVALIDARG without one, d3d11_classlink_probe [create]).
    if (graphics::dxbc_uses_class_linkage(container) &&
        refuse_interface_shader(linkage, name) != d3d11::S_OK) {
        refuse(d3d11::E_INVALIDARG, "shader declares interfaces");
        return;
    }
    HullShader hull_shader{};
    DomainShader domain_shader{};
    VertexShader domain_vertex{};
    bool hardware_domain = false;
    {
        std::string why;
        u32 built = d3d11::S_OK;
        {
            const PureWorkUnlock unlocked(*this);
            built = build_tessellation_program(
                bytes, container, hull, hull_shader, domain_shader,
                domain_vertex, hardware_domain, why);
        }
        if (const u32 hr = built; hr != d3d11::S_OK) {
            refuse(hr, why);
            return;
        }
    }

    if (shader_out == 0) {
        m.set_return(1u);  // S_FALSE: validation only
        return;
    }
    if (!ensure_storage()) {
        refuse(d3d11::E_FAIL, "no object storage");
        return;
    }
    const u32 tess_domain = hull ? hull_shader.domain : domain_shader.domain;
    attach_shader_linkage(hull ? hull_shader.linkage : domain_shader.linkage,
                          bytes, container);
    register_class_linkage(linkage, container);
    const GuestAddr address = allocate_object(hull ? m_hull_shader_vtable
                                                   : m_domain_shader_vtable);
    if (address == 0) {
        refuse(d3d11::E_FAIL, "object allocation failed");
        return;
    }
    const auto owner = m_interfaces.find(self);
    const GuestAddr device_owner =
        owner != m_interfaces.end() ? owner->second.owner : 0;
    if (hull) {
        hull_shader.interface_address = address;
        hull_shader.device_owner = device_owner;
        m_hull_shaders.emplace(address, std::move(hull_shader));
    } else {
        // The patch draw runs the domain shader as its vertex function; it
        // sits in the vertex-shader map under a key no guest object can have
        // (objects are aligned), so the draw path needs no new lookup.
        domain_shader.interface_address = address;
        domain_shader.device_owner = device_owner;
        if (hardware_domain) {
            domain_shader.vertex_key = address | 1u;
            domain_vertex.interface_address = domain_shader.vertex_key;
            domain_vertex.device_owner = device_owner;
            m_vertex_shaders.emplace(domain_shader.vertex_key,
                                     std::move(domain_vertex));
        }
        m_domain_shaders.emplace(address, std::move(domain_shader));
    }
    acquire_parent(device_owner);
    if (m.write_ptr(shader_out, address).is_err()) {
        (void)release_object(address);
        refuse(d3d11::E_INVALIDARG, "out-parameter unwritable");
        return;
    }
    d3d11_diag("ID3D11Device::%s -> 0x%llx domain=%u", name,
               static_cast<unsigned long long>(address), tess_domain);
    m.set_return(d3d11::S_OK);
}

// -----------------------------------------------------------------------------
// HSSetShader / DSSetShader (context 60 / 64); the resource, sampler and
// constant-buffer slots share the other stages' code in
// d3d11_context_shader_stages.cpp and d3d11_context_constant_buffers.cpp.
// -----------------------------------------------------------------------------

void D3D11Manager::context_tessellation_method(Marshaler& m, GuestAddr self,
                                               usize slot) {
    const bool hull = slot == 60;
    switch (slot) {
    case 60:
    case 64:
        // (pShader, ppClassInstances, NumClassInstances).
        set_stage_shader(self, hull ? LinkedStage::Hull : LinkedStage::Domain,
                         m.arg_ptr(1), m.arg_ptr(2),
                         static_cast<u32>(m.arg(3)));
        break;
    default:
        break;
    }
    m.set_return(0);
}

// -----------------------------------------------------------------------------
// The three-pass draw
// -----------------------------------------------------------------------------

void D3D11Manager::execute_tessellation(GuestAddr context, bool indexed,
                                        u32 count, u32 start_location,
                                        i32 base_vertex, u32 instance_count,
                                        u32 first_instance,
                                        const IndirectDraw& indirect) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_skip_stat("Draw skipped: tessellation");
        d3d11_diag("tessellated draw: nothing drawn: %s", why);
    };
    const auto hs_it = m_hull_shaders.find(state.hull_shader);
    const auto ds_it = m_domain_shaders.find(state.domain_shader);
    if (hs_it == m_hull_shaders.end() || ds_it == m_domain_shaders.end()) {
        // A hull shader without a domain shader (or the reverse) is an
        // invalid pipeline; Windows draws nothing either.
        return decline("the hull and domain stages must both be bound");
    }
    // Metal's tessellator draws triangle output of tri and quad domains
    // straight to the rasterizer, and for integer and pow2 partitioning it
    // is exact: the hull kernel hands it the D3D-rounded integer factors
    // (11.7.11). Fractional partitioning is not -- Metal takes the factors
    // as half floats where D3D converts float32 to 16.16 fixed point
    // (11.7.14), which moved the bent-patch coverage by a pixel -- and
    // neither is anything else: point and line output, isolines, stream
    // output of the domain shader, a geometry shader after it, indirect
    // counts. Those run on the software tessellator.
    // A PIPELINE_STATISTICS query building here needs the domain points and
    // primitives the tessellator made, which Metal's never reports: the
    // software tessellator counts them (vitreo_stats_tessellation).
    // A MINIMUM_ / MAXIMUM_ sampler on the domain stage reduces in a
    // variant of the domain kernel, which only the software path runs.
    const bool hardware =
        !indirect.active() && state.geometry_shader == 0 &&
        !pipeline_statistics_building(context) &&
        minmax_sampler_mask(state.domain_samplers) == 0 &&
        ds_it->second.vertex_key != 0 &&
        (hs_it->second.output_primitive == 3u ||
         hs_it->second.output_primitive == 4u) &&
        (hs_it->second.domain == 2u || hs_it->second.domain == 3u) &&
        (hs_it->second.partitioning == 1u || hs_it->second.partitioning == 2u);
    if (!hardware) {
        return execute_tessellation_software(context, indexed, count,
                                             start_location, base_vertex,
                                             instance_count, first_instance,
                                             indirect);
    }
    const u32 topology = state.primitive_topology;
    if (topology < PATCH_LIST_FIRST || topology > PATCH_LIST_LAST) {
        return decline("a tessellated draw needs a patch-list topology");
    }
    const u32 control_points = topology - (PATCH_LIST_FIRST - 1);
    const HullShader hs = hs_it->second;  // copied: execute_draw re-enters
    if (control_points != hs.input_control_points) {
        return decline("the patch size differs from the hull shader's input "
                       "control point count");
    }
    if (ds_it->second.domain != hs.domain ||
        ds_it->second.input_control_points != hs.output_control_points) {
        return decline("the domain shader does not match the hull shader's "
                       "domain or output control points");
    }
    const GuestAddr domain_vertex_key = ds_it->second.vertex_key;

    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    const std::vector<u32> record_layout = vs->second.capture_registers;

    auto& kernels = hs_it->second.kernels;
    const u32 hull_minmax = minmax_sampler_mask(state.hull_samplers);
    const std::vector<u32> kernel_key =
        kernel_variant_key(record_layout, hull_minmax);
    if (kernels.find(kernel_key) == kernels.end()) {
        std::string msl;
        std::shared_ptr<graphics::BackendComputePipeline> built;
        if (compile_geometry_kernel(hs.bytecode, record_layout, msl, nullptr,
                                    false, hull_minmax)) {
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = msl;
            desc.compute_entry_point = "main0";
            desc.cache_key = std::hash<std::string>{}(msl);
            auto created = m_backend.create_compute_pipeline(desc);
            if (created && created->valid()) built = std::move(created);
        }
        kernels.emplace(kernel_key, std::move(built));
    }
    const std::shared_ptr<graphics::BackendComputePipeline> kernel =
        kernels.at(kernel_key);
    if (!kernel) return decline("the hull kernel failed for this vertex shader");

    // Patch assembly: a patch list is a list of N-vertex primitives.
    CaptureExtent extent{};
    if (const char* why = capture_extent(state, indexed, count, start_location,
                                         base_vertex, extent)) {
        return decline(why);
    }
    std::vector<u32> indices;
    if (!draw_indices(state, indexed, count, extent, indices)) {
        return decline("the index data is not readable on the CPU");
    }
    const u32 cut = state.index_buffer_format == 42 ? 0xFFFFFFFFu : 0xFFFFu;
    const std::vector<u32> ids = graphics::assemble_primitive_vertices(
        indices, indexed, cut, indexed ? base_vertex : 0, control_points,
        graphics::PrimitiveAssembly::List);
    const u32 patches = static_cast<u32>(ids.size() / control_points);
    if (patches == 0) return;
    // Sized by every vertex the capture runs, not only the assembled
    // patches' (a trailing partial patch still captures): see the geometry
    // emulation.
    extent.max_vertex = std::max<u64>(
        extent.max_vertex, *std::max_element(ids.begin(), ids.end()));
    std::vector<u32> capture_indices;
    if (indexed) {
        for (const u32 index : indices) {
            if (index != cut) capture_indices.push_back(index);
        }
    }

    const u64 record_bytes =
        (extent.max_vertex + 1) * record_layout.size() * 16u;
    const u32 cp_stride = std::max(hs.cp_record_vec4s, 1u);
    const u32 pc_stride = std::max(hs.pc_record_vec4s, 1u);
    const u64 cp_bytes =
        static_cast<u64>(patches) * hs.output_control_points * cp_stride * 16u;
    const u64 pc_bytes = static_cast<u64>(patches) * pc_stride * 16u;
    const u64 factor_bytes =
        static_cast<u64>(patches) * (hs.domain == 3u ? 12u : 8u);
    if (record_bytes * instance_count > MAX_TESSELLATION_BYTES ||
        cp_bytes > MAX_TESSELLATION_BYTES || pc_bytes > MAX_TESSELLATION_BYTES) {
        return decline("a pass would exceed the size limit");
    }

    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target) return decline("capture target unavailable");

    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    const auto make = [&](u64 bytes, const void* data) {
        storage.byte_width = static_cast<usize>(std::max<u64>(bytes, 16u));
        return m_backend.create_buffer(storage, data);
    };
    auto capture = make(record_bytes * instance_count, nullptr);
    auto id_buffer = make(ids.size() * sizeof(u32), ids.data());
    // [1] index format (32-bit ids), [2] first, [3] base, [4] primitive
    // stride, [5] control-point record stride, [6] patch-constant stride;
    // [8]-[10] (indirect draws only) stay 0.
    std::array<u32, graphics::kHullControlWords> control{};
    control[1] = 2u;
    control[4] = control_points;
    control[5] = cp_stride;
    control[6] = pc_stride;
    auto control_buffer = make(sizeof(control), control.data());
    std::unique_ptr<graphics::BackendBuffer> capture_index_buffer;
    if (!capture_indices.empty()) {
        graphics::BackendBufferDesc ib{};
        ib.index_buffer = true;
        ib.shader_resource = true;
        ib.byte_width = capture_indices.size() * sizeof(u32);
        capture_index_buffer =
            m_backend.create_buffer(ib, capture_indices.data());
    }
    if (!capture || !id_buffer || !control_buffer ||
        (indexed && !capture_index_buffer)) {
        return decline("pass buffers could not be allocated");
    }

    for (u32 instance = 0; instance < instance_count; ++instance) {
        // A: capture this instance's control points.
        DrawVariant capture_draw{};
        capture_draw.kind = DrawVariant::Kind::Capture;
        capture_draw.buffer = capture.get();
        capture_draw.offset = static_cast<unsigned>(record_bytes * instance);
        capture_draw.target = m_stream_out_target.get();
        capture_draw.capture_indices = capture_index_buffer.get();
        capture_draw.capture_index_count =
            static_cast<u32>(capture_indices.size());
        // SV_InstanceID = instance; execute_draw places the per-instance
        // data (StartInstanceLocation + instance / step) from the bias.
        capture_draw.instance_id_bias = instance;
        execute_draw(context, indexed, count, start_location, base_vertex, 1,
                     first_instance, 0, 0, &capture_draw);
        if (!capture_draw.submitted) {
            return decline("the capture draw was not submitted");
        }

        // B: the hull kernel, one thread per patch.
        auto cp_out = make(cp_bytes, nullptr);
        auto pc_out = make(pc_bytes, nullptr);
        auto factors = make(factor_bytes, nullptr);
        if (!cp_out || !pc_out || !factors) {
            return decline("hull output buffers could not be allocated");
        }
        graphics::BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.group_count_x = patches;
        call.group_count_y = call.group_count_z = 1;
        bind_compute_stage(state.hull_constant_buffers,
                           state.hull_constant_first,
                           state.hull_shader_resources, state.hull_samplers, {},
                           hs.binding_layout, call);
        // The hull stage's s# MipLODBias for explicit-LOD sampling.
        auto hull_push = sampler_push_block(state.hull_samplers);
        fill_constant_buffer_limits(hull_push, state.hull_constant_buffers,
                                    state.hull_constant_first,
                                    state.hull_constant_count);
        fill_typed_buffer_biases(hull_push, state.hull_shader_resources);
        call.constants = hull_push.data();
        call.constants_size = hull_push.size() * sizeof(u32);
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        call.buffers[base + 0] = capture.get();
        call.buffer_offsets[base + 0] = capture_draw.offset;
        call.buffers[base + 1] = cp_out.get();
        call.buffers[base + 2] = control_buffer.get();
        call.buffers[base + 3] = id_buffer.get();
        call.buffers[base + 4] = pc_out.get();
        call.buffers[base + 5] = factors.get();
        if (!m_backend.dispatch(call)) {
            return decline("hull dispatch failed");
        }

        // C: the patch draw. The domain shader is the vertex function, so
        // for this draw the vertex stage's bindings are the domain stage's.
        DrawVariant patch_draw{};
        patch_draw.kind = DrawVariant::Kind::Tessellated;
        patch_draw.tess_factors = factors.get();
        patch_draw.tess_control_points = cp_out.get();
        patch_draw.tess_control = control_buffer.get();
        patch_draw.tess_patch_constants = pc_out.get();
        patch_draw.patch_count = patches;
        patch_draw.patch_control_points = hs.output_control_points;
        patch_draw.tess_partition = partition_of(hs.partitioning);
        patch_draw.tess_clockwise = hs.output_primitive == 3u;
        patch_draw.tess_max_factor = static_cast<u32>(
            std::clamp(hs.max_tess_factor, 2.0f, 64.0f));
        const GuestAddr saved_vertex_shader = state.vertex_shader;
        state.vertex_shader = domain_vertex_key;
        std::swap(state.vertex_constant_buffers, state.domain_constant_buffers);
        std::swap(state.vertex_constant_first, state.domain_constant_first);
        std::swap(state.vertex_constant_count, state.domain_constant_count);
        std::swap(state.vertex_shader_resources,
                  state.domain_shader_resources);
        std::swap(state.vertex_samplers, state.domain_samplers);
        execute_draw(context, false, 0, 0, 0, 1, 0, 0, 0, &patch_draw);
        std::swap(state.vertex_constant_buffers, state.domain_constant_buffers);
        std::swap(state.vertex_constant_first, state.domain_constant_first);
        std::swap(state.vertex_constant_count, state.domain_constant_count);
        std::swap(state.vertex_shader_resources,
                  state.domain_shader_resources);
        std::swap(state.vertex_samplers, state.domain_samplers);
        state.vertex_shader = saved_vertex_shader;
        if (!patch_draw.submitted) {
            return decline("the patch draw was not submitted");
        }
    }
}

// -----------------------------------------------------------------------------
// The software tessellator (the exact path)
// -----------------------------------------------------------------------------

void D3D11Manager::execute_tessellation_software(
    GuestAddr context, bool indexed, u32 count, u32 start_location,
    i32 base_vertex, u32 instance_count, u32 first_instance,
    const IndirectDraw& indirect) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_skip_stat("Draw skipped: tessellation");
        d3d11_diag("tessellated draw: nothing drawn: %s", why);
    };
    const auto hs_it = m_hull_shaders.find(state.hull_shader);
    const auto ds_it = m_domain_shaders.find(state.domain_shader);
    if (hs_it == m_hull_shaders.end() || ds_it == m_domain_shaders.end()) {
        return decline("the hull and domain stages must both be bound");
    }
    const u32 topology = state.primitive_topology;
    if (topology < PATCH_LIST_FIRST || topology > PATCH_LIST_LAST) {
        return decline("a tessellated draw needs a patch-list topology");
    }
    const u32 control_points = topology - (PATCH_LIST_FIRST - 1);
    const HullShader hs = hs_it->second;  // copied: execute_draw re-enters
    if (control_points != hs.input_control_points) {
        return decline("the patch size differs from the hull shader's input "
                       "control point count");
    }
    DomainShader& ds = ds_it->second;
    if (ds.domain != hs.domain ||
        ds.input_control_points != hs.output_control_points) {
        return decline("the domain shader does not match the hull shader's "
                       "domain or output control points");
    }
    // What follows the domain shader: a geometry program, a stream-output
    // object made from domain-shader bytecode (the "NULL GS" of D3D11.3
    // 14.3), or the rasterizer alone.
    const GuestAddr geometry_shader = state.geometry_shader;
    const auto gs_it = m_geometry_shaders.find(geometry_shader);
    const bool geometry_program =
        gs_it != m_geometry_shaders.end() && gs_it->second.program;
    const bool stream_out_only =
        gs_it != m_geometry_shaders.end() && !gs_it->second.program;
    // Tessellator output: 1 point, 2 line (isolines), 3/4 triangles.
    const u32 vpp = hs.output_primitive == 1u   ? 1u
                    : hs.output_primitive == 2u ? 2u
                                                : 3u;
    if (geometry_program && gs_it->second.input_primitive != vpp) {
        // D3D11.3 11.8: the geometry shader's input primitive must be what
        // the tessellator outputs; Windows' debug layer refuses the draw.
        return decline("the geometry shader's input primitive does not match "
                       "the tessellator's output");
    }
    const bool rasterize =
        !geometry_program &&
        (!stream_out_only || gs_it->second.rasterized_stream == 0) &&
        !ds.passthrough_msl.empty();

    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    const std::vector<u32> record_layout = vs->second.capture_registers;
    // The hull kernel writing raw factors, per vertex-shader record layout.
    auto& hs_kernels = hs_it->second.software_kernels;
    const u32 hull_minmax = minmax_sampler_mask(state.hull_samplers);
    const std::vector<u32> hull_key =
        kernel_variant_key(record_layout, hull_minmax);
    if (hs_kernels.find(hull_key) == hs_kernels.end()) {
        std::string msl;
        std::shared_ptr<graphics::BackendComputePipeline> built;
        if (compile_geometry_kernel(hs.bytecode, record_layout, msl, nullptr,
                                    /*tess_software=*/true, hull_minmax)) {
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = msl;
            desc.compute_entry_point = "main0";
            desc.cache_key = std::hash<std::string>{}(msl);
            auto created = m_backend.create_compute_pipeline(desc);
            if (created && created->valid()) built = std::move(created);
        }
        hs_kernels.emplace(hull_key, std::move(built));
    }
    const std::shared_ptr<graphics::BackendComputePipeline> hull_kernel =
        hs_kernels.at(hull_key);
    if (!hull_kernel) {
        return decline("the hull kernel failed for this vertex shader");
    }
    if (!ds.compute_kernel_built) {
        ds.compute_kernel_built = true;
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = ds.compute_msl;
        desc.compute_entry_point = "main0";
        desc.cache_key = std::hash<std::string>{}(ds.compute_msl);
        auto created = m_backend.create_compute_pipeline(desc);
        if (created && created->valid()) ds.compute_kernel = std::move(created);
    }
    std::shared_ptr<graphics::BackendComputePipeline> domain_kernel =
        ds.compute_kernel;
    // MINIMUM_ / MAXIMUM_ samplers on the domain stage: the kernel variant
    // reducing through them, built once per mask.
    if (const u32 domain_minmax = minmax_sampler_mask(state.domain_samplers);
        domain_minmax != 0) {
        auto found = ds.minmax_kernels.find(domain_minmax);
        if (found == ds.minmax_kernels.end()) {
            std::string msl;
            std::vector<u32> records;
            std::string why;
            std::shared_ptr<graphics::BackendComputePipeline> built;
            if (compile_domain_kernel(ds.bytecode, msl, nullptr, records, why,
                                      domain_minmax) &&
                records == ds.record_registers) {
                graphics::BackendComputePipelineDesc desc{};
                desc.compute_shader_msl = msl;
                desc.compute_entry_point = "main0";
                desc.cache_key = std::hash<std::string>{}(msl);
                auto created = m_backend.create_compute_pipeline(desc);
                if (created && created->valid()) built = std::move(created);
            }
            found = ds.minmax_kernels.emplace(domain_minmax, std::move(built))
                        .first;
        }
        domain_kernel = found->second;
    }
    const std::vector<u32> ds_records = ds.record_registers;
    const std::string ds_passthrough = ds.passthrough_msl;
    const bool ds_viewport_array = ds.writes_viewport_index;
    const d3d11::ShaderBindingLayout ds_layout = ds.compute_layout;
    if (!domain_kernel) return decline("the domain kernel does not compile");
    std::shared_ptr<graphics::BackendComputePipeline> gs_kernel;
    if (geometry_program) {
        gs_kernel = geometry_kernel_for(
            geometry_shader, ds_records,
            minmax_sampler_mask(state.geometry_samplers));
        if (!gs_kernel) {
            return decline("the geometry kernel failed for this domain shader");
        }
    }
    if (!m_tess_patch || !m_tess_scan) {
        const std::string msl = graphics::tess::tessellator_msl();
        graphics::BackendComputePipelineDesc pd{};
        pd.compute_shader_msl = msl;
        pd.compute_entry_point = graphics::tess::kTessPatchEntryPoint;
        pd.cache_key = 0x7E550A7Cull;
        m_tess_patch = m_backend.create_compute_pipeline(pd);
        pd.compute_entry_point = graphics::tess::kTessScanEntryPoint;
        pd.cache_key = 0x7E550A7Dull;
        m_tess_scan = m_backend.create_compute_pipeline(pd);
    }
    if (!m_tess_patch || !m_tess_patch->valid() || !m_tess_scan ||
        !m_tess_scan->valid()) {
        return decline("the tessellator kernels are unavailable");
    }
    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target) return decline("capture target unavailable");

    const u32 cp_stride = std::max(hs.cp_record_vec4s, 1u);
    const u32 pc_stride = std::max(hs.pc_record_vec4s, 1u);
    const u32 ds_record_vec4s = static_cast<u32>(ds_records.size());
    // The most one patch can make (every factor at the declared
    // maxtessfactor, which the hull kernel clamps to) sizes the batches.
    const graphics::tess::TessPatchBound bound = graphics::tess::tess_patch_bound(
        hs.domain, hs.partitioning, hs.output_primitive, hs.max_tess_factor);
    u64 per_patch_bytes =
        static_cast<u64>(bound.points) * (16u + ds_record_vec4s * 16u) +
        static_cast<u64>(bound.indices) * 4u * 2u + 8u;
    if (geometry_program) {
        const GeometryShader& gs = gs_it->second;
        u32 streams = 0;
        for (u32 st = 0; st < 4; ++st) {
            if ((gs.stream_mask & (1u << st)) != 0) streams = st + 1;
        }
        per_patch_bytes +=
            static_cast<u64>(bound.indices / vpp + 1) * gs.instance_count *
            graphics::gs_vertices_per_invocation(gs.output_topology,
                                                 gs.max_output_vertices) *
            std::max(gs.output_record_vec4s, 1u) * 16u * std::max(streams, 1u);
    }
    // The most patches one pass sees: the draw's (one instance's), or for an
    // indirect draw what the capture holds.
    const u64 draw_patches =
        indirect.active()
            ? MAX_TESSELLATION_BYTES /
                  std::max<u64>(static_cast<u64>(hs.output_control_points) *
                                    std::max(hs.cp_record_vec4s, 1u) * 16u,
                                1)
            : std::max<u64>(count / std::max(control_points, 1u), 1);
    constexpr u64 kBatchBytes = 64ull << 20;
    const u32 batch_patches = static_cast<u32>(std::clamp<u64>(
        std::min<u64>(kBatchBytes / std::max<u64>(per_patch_bytes, 1),
                      draw_patches),
        1, 1u << 16));
    const u32 point_capacity = batch_patches * std::max(bound.points, 1u);
    const u32 index_capacity = batch_patches * std::max(bound.indices, 1u);

    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    const auto make = [&](u64 bytes, const void* data)
        -> std::shared_ptr<graphics::BackendBuffer> {
        storage.byte_width = static_cast<usize>(std::max<u64>(bytes, 16u));
        return m_backend.create_buffer(storage, data);
    };
    // Scratch the GPU alone writes, kept between draws and grown on demand
    // (the GPU orders each use's writes after the previous one's reads); a
    // batch reuses it too.
    const auto scratch = [&](std::shared_ptr<graphics::BackendBuffer>& slot,
                             u64 bytes) {
        bytes = std::max<u64>(bytes, 16u);
        if (!slot || slot->desc().byte_width < bytes) slot = make(bytes, nullptr);
        return slot;
    };
    const auto counts =
        scratch(m_tess_scratch.counts, static_cast<u64>(batch_patches) * 8u);
    const auto points =
        scratch(m_tess_scratch.points, static_cast<u64>(point_capacity) * 16u);
    // Indices, then one SV_PrimitiveID per primitive (a geometry shader's
    // table, kGsControlPrimitiveIdTable).
    const auto indices = scratch(
        m_tess_scratch.indices,
        (static_cast<u64>(index_capacity) + index_capacity / vpp + 1) * 4u);
    const auto records = scratch(
        m_tess_scratch.records,
        static_cast<u64>(point_capacity) * ds_record_vec4s * 16u);
    std::array<u32, graphics::tess::TESS_BLOCK_WORDS> block_init{};
    block_init[graphics::kDomainControlCpStride] = cp_stride;
    block_init[graphics::kDomainControlPcStride] = pc_stride;
    const auto block = make(sizeof(block_init), block_init.data());
    const auto args =
        make(graphics::tess::TESS_ARGS_WORDS * sizeof(u32), nullptr);
    if (!counts || !points || !indices || !records || !block || !args) {
        return decline("tessellation buffers could not be allocated");
    }
    const auto stats = stream_out_stats_record(context);
    if (!stats) return decline("no stats record");

    // One instance's (or, indirect, every instance's) patches after the hull
    // kernel: tessellate them in batches and hand each batch's domain
    // records on.
    const auto run_patches = [&](graphics::BackendBuffer* cp_out,
                                 graphics::BackendBuffer* pc_out,
                                 graphics::BackendBuffer* factors,
                                 u32 max_patches, u32 patches_per_instance,
                                 const graphics::BackendBuffer* patch_count,
                                 u32 patch_count_offset) -> bool {
        for (u32 first = 0; first < max_patches; first += batch_patches) {
            const u32 in_batch = std::min(batch_patches, max_patches - first);
            graphics::tess::TessPatchParams params{};
            params.mode = 0;
            params.domain = hs.domain;
            params.partitioning = hs.partitioning;
            params.output_primitive = hs.output_primitive;
            params.patches = in_batch;
            params.count_mode = patch_count != nullptr ? 1u : 0u;
            params.first_patch = first;
            params.patches_per_instance = patches_per_instance;
            params.write_primitive_ids = geometry_program ? 1u : 0u;
            params.factor_words = graphics::kTessFactorWords;
            params.point_capacity = point_capacity;
            params.index_capacity = index_capacity;
            graphics::BackendDispatchCall call{};
            call.pipeline = m_tess_patch.get();
            call.threads_per_group_x = 32;
            call.group_count_x = (in_batch + 31) / 32;
            call.group_count_y = call.group_count_z = 1;
            call.constants = &params;
            call.constants_size = sizeof(params);
            call.buffers[graphics::tess::TESS_FACTORS_INDEX] = factors;
            call.buffers[graphics::tess::TESS_COUNTS_INDEX] = counts.get();
            call.buffers[graphics::tess::TESS_POINTS_INDEX] = points.get();
            call.buffers[graphics::tess::TESS_INDICES_INDEX] = indices.get();
            call.buffers[graphics::tess::TESS_PRIMITIVE_IDS_INDEX] =
                indices.get();
            call.buffer_offsets[graphics::tess::TESS_PRIMITIVE_IDS_INDEX] =
                index_capacity * 4u;
            call.buffers[graphics::tess::TESS_PATCH_COUNT_INDEX] =
                patch_count != nullptr
                    ? const_cast<graphics::BackendBuffer*>(patch_count)
                    : counts.get();
            call.buffer_offsets[graphics::tess::TESS_PATCH_COUNT_INDEX] =
                patch_count != nullptr ? patch_count_offset : 0u;
            if (!m_backend.dispatch(call)) return false;

            graphics::tess::TessScanParams scan{};
            scan.patches = in_batch;
            scan.vertices_per_primitive = vpp;
            scan.point_capacity = point_capacity;
            scan.index_capacity = index_capacity;
            graphics::BackendDispatchCall scan_call{};
            scan_call.pipeline = m_tess_scan.get();
            scan_call.threads_per_group_x = graphics::tess::TESS_SCAN_THREADS;
            scan_call.group_count_x = scan_call.group_count_y =
                scan_call.group_count_z = 1;
            scan_call.constants = &scan;
            scan_call.constants_size = sizeof(scan);
            scan_call.buffers[graphics::tess::TESS_SCAN_COUNTS_INDEX] =
                counts.get();
            scan_call.buffers[graphics::tess::TESS_SCAN_BLOCK_INDEX] =
                block.get();
            scan_call.buffers[graphics::tess::TESS_SCAN_ARGS_INDEX] =
                args.get();
            if (!m_backend.dispatch(scan_call)) return false;

            params.mode = 1;
            call.constants = &params;
            if (!m_backend.dispatch(call)) return false;

            // D: the domain shader, one thread per point (GPU-counted).
            graphics::BackendDispatchCall ds_call{};
            ds_call.pipeline = domain_kernel.get();
            ds_call.threads_per_group_x = graphics::kDomainComputeThreads;
            // Sized for the batch's capacity: threads past the point count
            // the scan wrote return (kDomainControlPointCount). An indirect
            // dispatch from the scan's own output was measured to run with
            // stale (zero) arguments -- Metal orders a compute pass's writes
            // before a later draw's indirect read, not a later dispatch's.
            ds_call.group_count_x =
                (point_capacity + graphics::kDomainComputeThreads - 1) /
                graphics::kDomainComputeThreads;
            ds_call.group_count_y = ds_call.group_count_z = 1;
            bind_compute_stage(state.domain_constant_buffers,
                               state.domain_constant_first,
                               state.domain_shader_resources,
                               state.domain_samplers, {}, ds_layout, ds_call);
            auto domain_push = sampler_push_block(state.domain_samplers);
            fill_constant_buffer_limits(domain_push,
                                        state.domain_constant_buffers,
                                        state.domain_constant_first,
                                        state.domain_constant_count);
            fill_typed_buffer_biases(domain_push,
                                     state.domain_shader_resources);
            ds_call.constants = domain_push.data();
            ds_call.constants_size = domain_push.size() * sizeof(u32);
            constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
            ds_call.buffers[base + 0] = points.get();
            ds_call.buffers[base + 1] = cp_out;
            ds_call.buffers[base + 2] = block.get();
            ds_call.buffers[base + 3] = records.get();
            ds_call.buffers[base + 4] = pc_out;
            if (!m_backend.dispatch(ds_call)) return false;
            if (pipeline_statistics_building(context)) {
                count_tessellation_statistics(
                    context, ds, vpp, index_capacity, block.get(),
                    indices.get(), records.get(),
                    rasterize && !rasterization_disabled(state));
            }

            // What reads the records.
            if (geometry_program) {
                GeometryInput in{};
                in.records = records.get();
                in.ids = indices.get();
                in.vertices_per_primitive = vpp;
                in.max_primitives = index_capacity / vpp;
                in.primitive_count = block.get();
                in.primitive_count_offset =
                    graphics::tess::TESS_BLOCK_PRIMITIVES * sizeof(u32);
                in.primitive_id_table = true;
                in.primitive_id_table_offset = index_capacity;
                if (!geometry_expand(context, geometry_shader, gs_kernel, in,
                                     stats)) {
                    return false;
                }
                continue;
            }
            if (stream_out_only) {
                const auto so_it = m_geometry_shaders.find(geometry_shader);
                if (so_it == m_geometry_shaders.end()) return false;
                StreamOutPass pass{};
                pass.stream = 0;
                pass.strides = so_it->second.stream_out_strides;
                pass.slot_mask = so_it->second.stream_out_slots[0];
                pass.vertices_per_primitive = vpp;
                for (const auto& e : so_it->second.stream_out) {
                    graphics::StreamOutRepackEntry out{};
                    out.slot = e.slot;
                    out.dst_dword = e.dst_dword;
                    out.start_component = e.start_component;
                    out.component_count = e.component_count;
                    out.src_record = graphics::STREAM_OUT_GAP;
                    if (!e.gap) {
                        const auto found = std::find(ds_records.begin(),
                                                     ds_records.end(),
                                                     e.output_register);
                        if (found == ds_records.end()) continue;
                        out.src_record =
                            static_cast<u32>(found - ds_records.begin());
                    }
                    pass.entries.push_back(out);
                }
                pass.records = records.get();
                pass.record_vec4s = ds_record_vec4s;
                pass.index_mode = graphics::StreamOutIndexMode::Uint32;
                pass.indices = indices.get();
                pass.count_mode = 1;
                pass.count = block.get();
                pass.count_offset =
                    graphics::tess::TESS_BLOCK_PRIMITIVES * sizeof(u32);
                pass.max_vertices = index_capacity;
                if (!stream_out_pass(context, pass, stats)) return false;
            }
            if (rasterize) {
                DrawVariant draw{};
                draw.kind = DrawVariant::Kind::GeometryOutput;
                draw.buffer = records.get();
                draw.vertex_msl = &ds_passthrough;
                draw.index_buffer = indices.get();
                draw.vertex_count = index_capacity;
                draw.indirect = IndirectDraw{
                    args.get(),
                    graphics::tess::TESS_ARGS_DRAW *
                        static_cast<u32>(sizeof(u32))};
                draw.viewport_array = ds_viewport_array;
                draw.topology =
                    vpp == 1 ? graphics::BackendPrimitiveTopology::PointList
                    : vpp == 2 ? graphics::BackendPrimitiveTopology::LineList
                               : graphics::BackendPrimitiveTopology::TriangleList;
                execute_draw(context, false, 0, 0, 0, 1, 0, 0, 0, &draw);
                if (!draw.submitted) return false;
            }
        }
        return true;
    };

    const u64 cp_per_patch =
        static_cast<u64>(hs.output_control_points) * cp_stride * 16u;
    const u64 pc_per_patch = static_cast<u64>(pc_stride) * 16u;
    const u64 factor_per_patch = graphics::kTessFactorWords * 4u;
    const auto run_hull = [&](graphics::BackendBuffer* capture,
                              u32 capture_offset,
                              graphics::BackendBuffer* ids, u32 patches,
                              const std::array<u32, graphics::kHullControlWords>&
                                  control,
                              const graphics::BackendBuffer* patch_limit,
                              u32 patch_limit_offset,
                              const graphics::BackendBuffer* per_instance,
                              u32 per_instance_offset,
                              std::shared_ptr<graphics::BackendBuffer>& cp_out,
                              std::shared_ptr<graphics::BackendBuffer>& pc_out,
                              std::shared_ptr<graphics::BackendBuffer>& factors)
        -> bool {
        if (cp_per_patch * patches > MAX_TESSELLATION_BYTES) return false;
        cp_out = make(cp_per_patch * patches, nullptr);
        pc_out = make(pc_per_patch * patches, nullptr);
        factors = make(factor_per_patch * patches, nullptr);
        const auto control_buffer = make(sizeof(control), control.data());
        if (!cp_out || !pc_out || !factors || !control_buffer) return false;
        if (patch_limit != nullptr &&
            (!m_backend.copy_buffer_region(
                 control_buffer.get(), graphics::kHullControlPatchLimit * 4u,
                 patch_limit, patch_limit_offset, 4u) ||
             !m_backend.copy_buffer_region(
                 control_buffer.get(),
                 graphics::kHullControlPatchesPerInstance * 4u, per_instance,
                 per_instance_offset, 4u))) {
            return false;
        }
        graphics::BackendDispatchCall call{};
        call.pipeline = hull_kernel.get();
        call.group_count_x = patches;
        call.group_count_y = call.group_count_z = 1;
        bind_compute_stage(state.hull_constant_buffers,
                           state.hull_constant_first,
                           state.hull_shader_resources, state.hull_samplers, {},
                           hs.binding_layout, call);
        auto hull_push = sampler_push_block(state.hull_samplers);
        fill_constant_buffer_limits(hull_push, state.hull_constant_buffers,
                                    state.hull_constant_first,
                                    state.hull_constant_count);
        fill_typed_buffer_biases(hull_push, state.hull_shader_resources);
        call.constants = hull_push.data();
        call.constants_size = hull_push.size() * sizeof(u32);
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        call.buffers[base + 0] = capture;
        call.buffer_offsets[base + 0] = capture_offset;
        call.buffers[base + 1] = cp_out.get();
        call.buffers[base + 2] = control_buffer.get();
        call.buffers[base + 3] = ids;
        call.buffers[base + 4] = pc_out.get();
        call.buffers[base + 5] = factors.get();
        return m_backend.dispatch(call);
    };
    std::array<u32, graphics::kHullControlWords> control{};
    control[1] = 2u;  // 32-bit record keys
    control[4] = control_points;
    control[5] = cp_stride;
    control[6] = pc_stride;

    if (indirect.active()) {
        // The patches (and instances) are GPU-counted: resolve and capture
        // every instance at once, run the hull kernel over the most there
        // can be, and let each pass stop at the GPU's count.
        IndirectCapture captured{};
        if (!capture_indirect(context, indexed, indirect, control_points, 0u,
                              static_cast<u32>(record_layout.size()),
                              MAX_TESSELLATION_BYTES /
                                  std::max<u64>(cp_per_patch, 1),
                              captured)) {
            return;
        }
        std::array<u32, graphics::kHullControlWords> indirect_control = control;
        indirect_control[graphics::kHullControlFlags] = 3u;
        std::shared_ptr<graphics::BackendBuffer> cp_out, pc_out, factors;
        if (!run_hull(captured.capture.get(), graphics::CAPTURE_HEADER_BYTES,
                      captured.ids.get(), captured.primitive_capacity,
                      indirect_control, captured.resolved.get(),
                      graphics::IA_RESOLVED_PRIMITIVES * 4u,
                      captured.resolved.get(),
                      graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE * 4u, cp_out,
                      pc_out, factors) ||
            !run_patches(cp_out.get(), pc_out.get(), factors.get(),
                         captured.primitive_capacity, 0u,
                         captured.resolved.get(),
                         graphics::IA_RESOLVED_PRIMITIVES * 4u)) {
            return decline("the tessellation passes could not be recorded");
        }
        note_stream_out_stats(context, stats);
        return;
    }

    // Patch assembly: a patch list is a list of N-vertex primitives.
    CaptureExtent extent{};
    if (const char* why = capture_extent(state, indexed, count, start_location,
                                         base_vertex, extent)) {
        return decline(why);
    }
    std::vector<u32> draw_ids;
    if (!draw_indices(state, indexed, count, extent, draw_ids)) {
        return decline("the index data is not readable on the CPU");
    }
    const u32 cut = state.index_buffer_format == 42 ? 0xFFFFFFFFu : 0xFFFFu;
    const std::vector<u32> ids = graphics::assemble_primitive_vertices(
        draw_ids, indexed, cut, indexed ? base_vertex : 0, control_points,
        graphics::PrimitiveAssembly::List);
    const u32 patches = static_cast<u32>(ids.size() / control_points);
    if (patches == 0) return;
    extent.max_vertex = std::max<u64>(
        extent.max_vertex, *std::max_element(ids.begin(), ids.end()));
    std::vector<u32> capture_indices;
    if (indexed) {
        for (const u32 index : draw_ids) {
            if (index != cut) capture_indices.push_back(index);
        }
    }
    const u64 record_bytes =
        (extent.max_vertex + 1) * record_layout.size() * 16u;
    if (record_bytes * instance_count > MAX_TESSELLATION_BYTES) {
        return decline("a pass would exceed the size limit");
    }
    const auto capture = make(record_bytes * instance_count, nullptr);
    const auto id_buffer = make(ids.size() * sizeof(u32), ids.data());
    std::unique_ptr<graphics::BackendBuffer> capture_index_buffer;
    if (!capture_indices.empty()) {
        graphics::BackendBufferDesc ib{};
        ib.index_buffer = true;
        ib.shader_resource = true;
        ib.byte_width = capture_indices.size() * sizeof(u32);
        capture_index_buffer =
            m_backend.create_buffer(ib, capture_indices.data());
    }
    if (!capture || !id_buffer || (indexed && !capture_index_buffer)) {
        return decline("pass buffers could not be allocated");
    }
    for (u32 instance = 0; instance < instance_count; ++instance) {
        // A: capture this instance's control points.
        DrawVariant capture_draw{};
        capture_draw.kind = DrawVariant::Kind::Capture;
        capture_draw.buffer = capture.get();
        capture_draw.offset = static_cast<unsigned>(record_bytes * instance);
        capture_draw.target = m_stream_out_target.get();
        capture_draw.capture_indices = capture_index_buffer.get();
        capture_draw.capture_index_count =
            static_cast<u32>(capture_indices.size());
        capture_draw.instance_id_bias = instance;
        execute_draw(context, indexed, count, start_location, base_vertex, 1,
                     first_instance, 0, 0, &capture_draw);
        if (!capture_draw.submitted) {
            return decline("the capture draw was not submitted");
        }
        // B: the hull kernel; then the tessellator, the domain kernel and
        // what follows, per batch. SV_PrimitiveID is the patch's index in
        // this instance (D3D11.3 8.17, 11.7.9.1).
        std::shared_ptr<graphics::BackendBuffer> cp_out, pc_out, factors;
        if (!run_hull(capture.get(), capture_draw.offset, id_buffer.get(),
                      patches, control, nullptr, 0, nullptr, 0, cp_out, pc_out,
                      factors) ||
            !run_patches(cp_out.get(), pc_out.get(), factors.get(), patches,
                         patches, nullptr, 0)) {
            return decline("the tessellation passes could not be recorded");
        }
    }
    note_stream_out_stats(context, stats);
}

}  // namespace vitreo::winapi
