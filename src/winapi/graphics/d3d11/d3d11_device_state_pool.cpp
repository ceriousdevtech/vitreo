// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_state_pool.cpp
/// @brief D3D11 state objects: description validation, the canonical
///        description and per-device deduplication.
///
/// Split out of d3d11_device_methods.cpp. The creators that use it are
/// in d3d11_device_states.cpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// State objects: validation, the canonical description, deduplication.

bool D3D11Manager::read_blend_desc(GuestAddr address, bool desc1,
                                   BlendStateDesc& desc) const {
    // D3D11_BLEND_DESC: AlphaToCoverageEnable, IndependentBlendEnable, then
    // eight D3D11_RENDER_TARGET_BLEND_DESC {BlendEnable, SrcBlend, DestBlend,
    // BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, UINT8
    // RenderTargetWriteMask} of 32 bytes. D3D11_BLEND_DESC1's targets insert
    // LogicOpEnable after BlendEnable and LogicOp before the mask: 40 bytes.
    const u32 target_bytes = desc1 ? 40u : 32u;
    std::array<Byte, 8 + 8 * 40> bytes{};
    if (m_memory.read_bytes(address, bytes.data(), 8 + 8 * target_bytes)
            .is_err()) {
        return false;
    }
    const auto word = [&bytes](u32 offset) {
        u32 value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    desc = BlendStateDesc{};
    desc.alpha_to_coverage_enable = word(0);
    desc.independent_blend_enable = word(4);
    for (u32 target = 0; target < 8; ++target) {
        const u32 base = 8 + target * target_bytes;
        const u32 shift = desc1 ? 4u : 0u;
        desc.blend_enable[target] = word(base);
        desc.logic_op_enable[target] = desc1 ? word(base + 4) : 0u;
        desc.src_blend[target] = word(base + 4 + shift);
        desc.dest_blend[target] = word(base + 8 + shift);
        desc.blend_op[target] = word(base + 12 + shift);
        desc.src_blend_alpha[target] = word(base + 16 + shift);
        desc.dest_blend_alpha[target] = word(base + 20 + shift);
        desc.blend_op_alpha[target] = word(base + 24 + shift);
        desc.logic_op[target] = desc1 ? word(base + 32) : 4u;  // NOOP
        desc.render_target_write_mask[target] =
            bytes[base + (desc1 ? 36u : 28u)];
    }
    return true;
}

bool D3D11Manager::blend_desc_valid(const BlendStateDesc& desc,
                                    bool logic_ops) {
    // Learn D3D11_BLEND: 12 and 13 are not values. Learn
    // D3D11_RENDER_TARGET_BLEND_DESC: "Blend options that end in _COLOR are
    // not allowed" in the alpha pair. D3D11_BLEND_OP is 1..5.
    const auto color_factor = [](u32 factor) {
        return (factor >= 1u && factor <= 11u) ||
               (factor >= 14u && factor <= 19u);
    };
    const auto alpha_factor = [&color_factor](u32 factor) {
        return color_factor(factor) && factor != 3u && factor != 4u &&
               factor != 9u && factor != 10u && factor != 16u &&
               factor != 17u;
    };
    const auto blend_op = [](u32 op) { return op >= 1u && op <= 5u; };
    const auto dual_source = [](u32 factor) {
        return factor >= 16u && factor <= 19u;
    };
    // Only RenderTarget[0] is judged unless IndependentBlendEnable is set
    // (Learn: "RenderTarget[1..7] are ignored"), and only the fields a
    // target's enables switch on -- except the write mask, a UINT8 of which
    // only the low four bits exist (measured, d3d11_a04_state_objects_probe
    // "blend rt0 mask=0x10", "blend indep=0 rt3 mask=0x30").
    const bool independent = desc.independent_blend_enable != 0;
    const u32 targets = independent ? 8u : 1u;
    bool dual_source_target = false;
    bool other_blending_target = false;
    for (u32 target = 0; target < targets; ++target) {
        if ((desc.render_target_write_mask[target] & ~0xFu) != 0) return false;
        if (desc.logic_op_enable[target] != 0) {
            // D3D11.3 functional spec §17.7: a logic op replaces blending on
            // its target, cannot be combined with independent blending, is
            // one of the 16 D3D11_LOGIC_OP values, and exists only where
            // the device reports OutputMergerLogicOp.
            if (desc.blend_enable[target] != 0 || independent ||
                desc.logic_op[target] > 15u || !logic_ops) {
                return false;
            }
        }
        if (desc.blend_enable[target] == 0) continue;
        if (!color_factor(desc.src_blend[target]) ||
            !color_factor(desc.dest_blend[target]) ||
            !alpha_factor(desc.src_blend_alpha[target]) ||
            !alpha_factor(desc.dest_blend_alpha[target]) ||
            !blend_op(desc.blend_op[target]) ||
            !blend_op(desc.blend_op_alpha[target])) {
            return false;
        }
        // §17.6 Dual Source Color Blending: "The only valid blend ops with
        // Dual Source Color Blending are: add, subtract and revsubtract",
        // judged per pair, and it works "with a single RenderTarget bound,
        // at slot 0" -- so no dual source on RT1..7 and no other target
        // blending beside it.
        const bool dual_color = dual_source(desc.src_blend[target]) ||
                                dual_source(desc.dest_blend[target]);
        const bool dual_alpha = dual_source(desc.src_blend_alpha[target]) ||
                                dual_source(desc.dest_blend_alpha[target]);
        if ((dual_color && desc.blend_op[target] > 3u) ||
            (dual_alpha && desc.blend_op_alpha[target] > 3u)) {
            return false;
        }
        if (dual_color || dual_alpha) {
            if (target != 0) return false;
            dual_source_target = true;
        } else if (target != 0) {
            other_blending_target = true;
        }
    }
    return !(dual_source_target && other_blending_target);
}

bool D3D11Manager::rasterizer_desc_valid(const RasterizerStateDesc& desc,
                                         u32 feature_level) {
    // D3D11_FILL_MODE is 2..3 and D3D11_CULL_MODE 1..3 (a zero-filled
    // description is refused). A NaN DepthBiasClamp or SlopeScaledDepthBias
    // is refused; infinities and negative clamps are not (measured,
    // d3d11_a04_state_objects_probe "rs clamp=NaN", "rs slope=NaN").
    if ((desc.fill_mode != 2u && desc.fill_mode != 3u) ||
        desc.cull_mode < 1u || desc.cull_mode > 3u ||
        std::isnan(desc.depth_bias_clamp) ||
        std::isnan(desc.slope_scaled_depth_bias)) {
        return false;
    }
    // D3D11.3 functional spec §3.5.6 (target-independent rasterization) as
    // measured at FL 11_0: ForcedSampleCount is 0, 1, 4 or 8 -- 16 and a
    // forced single sample with MultisampleEnable need FL 11_1, and 2 is not
    // a forced count at any level ("rs1 forced=*").
    const u32 forced = desc.forced_sample_count;
    const bool fl11_1 = feature_level >= 0xB100u;
    if ((forced != 0u && forced != 1u && forced != 4u && forced != 8u &&
         !(forced == 16u && fl11_1)) ||
        (forced == 1u && desc.multisample_enable != 0 && !fl11_1)) {
        return false;
    }
    // D3D11_CONSERVATIVE_RASTERIZATION_MODE is OFF or ON, and ON needs a
    // ConservativeRasterizationTier, which this device reports as 0
    // (D3D11_OPTIONS2): E_INVALIDARG as on a tier-0 Windows device ("rs2
    // conservative=1").
    return desc.conservative_raster == 0u;
}

D3D11Manager::BlendStateDesc D3D11Manager::canonical_blend_desc(
    const BlendStateDesc& desc) {
    // Measured (d3d11_a04_state_objects_probe "blend * getdesc", "blend1
    // getdesc1"): BOOLs are 1; without IndependentBlendEnable every target
    // reports RenderTarget[0]; a target that does not blend reports ONE /
    // ZERO / ADD for both pairs; a disabled logic op reports NOOP.
    BlendStateDesc canonical{};
    canonical.alpha_to_coverage_enable = desc.alpha_to_coverage_enable != 0;
    canonical.independent_blend_enable = desc.independent_blend_enable != 0;
    for (u32 target = 0; target < 8; ++target) {
        const u32 source = canonical.independent_blend_enable ? target : 0u;
        const bool blend = desc.blend_enable[source] != 0;
        const bool logic = desc.logic_op_enable[source] != 0;
        canonical.blend_enable[target] = blend ? 1u : 0u;
        canonical.logic_op_enable[target] = logic ? 1u : 0u;
        canonical.src_blend[target] = blend ? desc.src_blend[source] : 2u;
        canonical.dest_blend[target] = blend ? desc.dest_blend[source] : 1u;
        canonical.blend_op[target] = blend ? desc.blend_op[source] : 1u;
        canonical.src_blend_alpha[target] =
            blend ? desc.src_blend_alpha[source] : 2u;
        canonical.dest_blend_alpha[target] =
            blend ? desc.dest_blend_alpha[source] : 1u;
        canonical.blend_op_alpha[target] =
            blend ? desc.blend_op_alpha[source] : 1u;
        canonical.logic_op[target] = logic ? desc.logic_op[source] : 4u;
        canonical.render_target_write_mask[target] =
            desc.render_target_write_mask[source] & 0xFFu;
    }
    return canonical;
}

D3D11Manager::RasterizerStateDesc D3D11Manager::canonical_rasterizer_desc(
    const RasterizerStateDesc& desc) {
    // Only the BOOLs change ("rs roundtrip getdesc"; "rs dedup scissor=2 vs
    // scissor=1").
    RasterizerStateDesc canonical = desc;
    canonical.front_counter_clockwise = desc.front_counter_clockwise != 0;
    canonical.depth_clip_enable = desc.depth_clip_enable != 0;
    canonical.scissor_enable = desc.scissor_enable != 0;
    canonical.multisample_enable = desc.multisample_enable != 0;
    canonical.antialiased_line_enable = desc.antialiased_line_enable != 0;
    return canonical;
}

D3D11Manager::DepthStencilStateDesc
D3D11Manager::canonical_depth_stencil_desc(const DepthStencilStateDesc& desc) {
    // Measured ("ds * getdesc"): BOOLs are 1; with depth disabled the mask
    // reads ALL and the function LESS; with stencil disabled both masks read
    // 0xFF and both faces KEEP / KEEP / KEEP / ALWAYS.
    DepthStencilStateDesc canonical = desc;
    canonical.depth_enable = desc.depth_enable != 0;
    canonical.stencil_enable = desc.stencil_enable != 0;
    if (canonical.depth_enable == 0) {
        canonical.depth_write_mask = 1u;  // D3D11_DEPTH_WRITE_MASK_ALL
        canonical.depth_func = 2u;        // D3D11_COMPARISON_LESS
    }
    if (canonical.stencil_enable == 0) {
        canonical.stencil_read_mask = 0xFFu;
        canonical.stencil_write_mask = 0xFFu;
        canonical.front_stencil_fail_op = 1u;  // D3D11_STENCIL_OP_KEEP
        canonical.front_stencil_depth_fail_op = 1u;
        canonical.front_stencil_pass_op = 1u;
        canonical.front_stencil_func = 8u;     // D3D11_COMPARISON_ALWAYS
        canonical.back_stencil_fail_op = 1u;
        canonical.back_stencil_depth_fail_op = 1u;
        canonical.back_stencil_pass_op = 1u;
        canonical.back_stencil_func = 8u;
    }
    return canonical;
}

D3D11Manager::SamplerStateDesc D3D11Manager::canonical_sampler_desc(
    const SamplerStateDesc& desc) {
    // Measured ("smp * getdesc", "smp dedup * vs *"): MaxAnisotropy reads 0
    // unless the filter is anisotropic, ComparisonFunc NEVER unless it is a
    // COMPARISON filter, and BorderColor 0 unless an address mode is BORDER.
    // Floats are otherwise kept bit for bit (-0.0 stays -0.0).
    SamplerStateDesc canonical = desc;
    if ((desc.filter & 0x7Fu) != 0x55u) canonical.max_anisotropy = 0;
    if ((desc.filter & 0x180u) != 0x80u) canonical.comparison_func = 1u;
    if (desc.address_u != 4u && desc.address_v != 4u &&
        desc.address_w != 4u) {
        for (float& component : canonical.border_color) component = 0.0f;
    }
    return canonical;
}

namespace {

/// A deduplication key: the family, the owning device, then the canonical
/// description word by word (floats as their bits, so -0.0 and +0.0 differ
/// as they do on Windows, "smp dedup bias -0.0 vs +0.0: same=0").
class StateKey {
public:
    StateKey(u8 family, GuestAddr device_owner) {
        m_bytes.push_back(static_cast<char>(family));
        append(static_cast<u32>(device_owner));
        append(static_cast<u32>(device_owner >> 32));
    }
    void append(u32 value) {
        char raw[sizeof(value)];
        std::memcpy(raw, &value, sizeof(value));
        m_bytes.append(raw, sizeof(raw));
    }
    void append(float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        append(bits);
    }
    [[nodiscard]] std::string take() { return std::move(m_bytes); }

private:
    std::string m_bytes;
};

}  // namespace

u32 D3D11Manager::state_pool_lookup(u8 family, const std::string& key,
                                    const std::string& default_key,
                                    GuestAddr device_owner,
                                    GuestAddr& existing) const {
    existing = 0;
    if (const auto hit = m_state_pool.find(key); hit != m_state_pool.end()) {
        existing = hit->second;
        return d3d11::S_OK;
    }
    // Learn (CreateBlendState et al.): "You can create up to 4096 unique
    // [state] objects per device"; the next is
    // D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS. The runtime's own default
    // object (StatePoolEntry) holds one of them from device creation on,
    // which is why an application gets 4095 more (measured, "limit *").
    u32 limit = D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE;
    switch (family) {
    case STATE_FAMILY_BLEND:
        limit = D3D11_REQ_BLEND_OBJECT_COUNT_PER_DEVICE;
        break;
    case STATE_FAMILY_RASTERIZER:
        limit = D3D11_REQ_RASTERIZER_OBJECT_COUNT_PER_DEVICE;
        break;
    case STATE_FAMILY_DEPTH_STENCIL:
        limit = D3D11_REQ_DEPTH_STENCIL_OBJECT_COUNT_PER_DEVICE;
        break;
    default:
        break;
    }
    const auto count = m_state_pool_counts.find({device_owner, family});
    const u32 live = count == m_state_pool_counts.end() ? 0u : count->second;
    const u32 reserved =
        key == default_key || m_state_pool.contains(default_key) ? 0u : 1u;
    if (live + reserved >= limit) {
        d3d11_diag("state object family %u of device 0x%llx holds %u unique "
                   "objects -> D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS",
                   family, static_cast<unsigned long long>(device_owner),
                   live + reserved);
        return D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
    }
    return d3d11::S_OK;
}

void D3D11Manager::state_pool_insert(u8 family, GuestAddr device_owner,
                                     const std::string& key, GuestAddr state) {
    m_state_pool[key] = state;
    ++m_state_pool_counts[{device_owner, family}];
}

void D3D11Manager::state_pool_erase(u8 family, GuestAddr device_owner,
                                    const std::string& key) {
    if (m_state_pool.erase(key) == 0) return;
    const auto count = m_state_pool_counts.find({device_owner, family});
    if (count != m_state_pool_counts.end() && count->second != 0) {
        --count->second;
    }
}

u32 D3D11Manager::acquire_blend_state(GuestAddr device_owner,
                                      const BlendStateDesc& desc,
                                      GuestAddr& state) {
    state = 0;
    const auto key_of = [device_owner](const BlendStateDesc& d) {
        StateKey key(STATE_FAMILY_BLEND, device_owner);
        key.append(d.alpha_to_coverage_enable);
        key.append(d.independent_blend_enable);
        for (u32 t = 0; t < 8; ++t) {
            for (const u32 word :
                 {d.blend_enable[t], d.logic_op_enable[t], d.src_blend[t],
                  d.dest_blend[t], d.blend_op[t], d.src_blend_alpha[t],
                  d.dest_blend_alpha[t], d.blend_op_alpha[t], d.logic_op[t],
                  d.render_target_write_mask[t]}) {
                key.append(word);
            }
        }
        return key.take();
    };
    // Learn D3D11_BLEND_DESC defaults: no blending (ONE / ZERO / ADD), every
    // channel written, NOOP logic op.
    BlendStateDesc defaults{};
    defaults.render_target_write_mask.fill(0xFu);
    const BlendStateDesc canonical = canonical_blend_desc(desc);
    const std::string key = key_of(canonical);
    const std::string default_key = key_of(canonical_blend_desc(defaults));
    GuestAddr existing = 0;
    const u32 hr = state_pool_lookup(STATE_FAMILY_BLEND, key, default_key,
                                     device_owner, existing);
    if (hr != d3d11::S_OK) return hr;
    if (auto it = m_blend_states.find(existing); it != m_blend_states.end()) {
        // A deduplicated object: one more reference for the application
        // (measured: count 2, and no second device reference).
        (void)retain_object(existing);
        state = existing;
        return d3d11::S_OK;
    }
    const GuestAddr created = create_blend_state(device_owner, canonical);
    if (created == 0) return d3d11::E_FAIL;
    auto& object = m_blend_states.at(created);
    object.pool.pool_key = key;
    object.pool.runtime_default = key == default_key;
    state_pool_insert(STATE_FAMILY_BLEND, device_owner, key, created);
    state = created;
    return d3d11::S_OK;
}

u32 D3D11Manager::acquire_rasterizer_state(GuestAddr device_owner,
                                           const RasterizerStateDesc& desc,
                                           GuestAddr& state) {
    state = 0;
    const auto key_of = [device_owner](const RasterizerStateDesc& d) {
        StateKey key(STATE_FAMILY_RASTERIZER, device_owner);
        key.append(d.fill_mode);
        key.append(d.cull_mode);
        key.append(d.front_counter_clockwise);
        key.append(static_cast<u32>(d.depth_bias));
        key.append(d.depth_bias_clamp);
        key.append(d.slope_scaled_depth_bias);
        key.append(d.depth_clip_enable);
        key.append(d.scissor_enable);
        key.append(d.multisample_enable);
        key.append(d.antialiased_line_enable);
        key.append(d.forced_sample_count);
        key.append(d.conservative_raster);
        return key.take();
    };
    // Learn D3D11_RASTERIZER_DESC defaults: SOLID, BACK, clockwise front
    // faces, no bias, depth clip on, scissor / multisample / AA lines off.
    RasterizerStateDesc defaults{};
    defaults.fill_mode = 3u;
    defaults.cull_mode = 3u;
    defaults.depth_clip_enable = 1u;
    const RasterizerStateDesc canonical = canonical_rasterizer_desc(desc);
    const std::string key = key_of(canonical);
    const std::string default_key =
        key_of(canonical_rasterizer_desc(defaults));
    GuestAddr existing = 0;
    const u32 hr = state_pool_lookup(STATE_FAMILY_RASTERIZER, key,
                                     default_key, device_owner, existing);
    if (hr != d3d11::S_OK) return hr;
    if (auto it = m_rasterizer_states.find(existing);
        it != m_rasterizer_states.end()) {
        // A deduplicated object: one more reference for the application
        // (measured: count 2, and no second device reference).
        (void)retain_object(existing);
        state = existing;
        return d3d11::S_OK;
    }
    const GuestAddr created = create_rasterizer_state(device_owner, canonical);
    if (created == 0) return d3d11::E_FAIL;
    auto& object = m_rasterizer_states.at(created);
    object.pool.pool_key = key;
    object.pool.runtime_default = key == default_key;
    state_pool_insert(STATE_FAMILY_RASTERIZER, device_owner, key, created);
    state = created;
    return d3d11::S_OK;
}

u32 D3D11Manager::acquire_depth_stencil_state(
    GuestAddr device_owner, const DepthStencilStateDesc& desc,
    GuestAddr& state) {
    state = 0;
    const auto key_of = [device_owner](const DepthStencilStateDesc& d) {
        StateKey key(STATE_FAMILY_DEPTH_STENCIL, device_owner);
        for (const u32 word :
             {d.depth_enable, d.depth_write_mask, d.depth_func,
              d.stencil_enable, static_cast<u32>(d.stencil_read_mask),
              static_cast<u32>(d.stencil_write_mask), d.front_stencil_fail_op,
              d.front_stencil_depth_fail_op, d.front_stencil_pass_op,
              d.front_stencil_func, d.back_stencil_fail_op,
              d.back_stencil_depth_fail_op, d.back_stencil_pass_op,
              d.back_stencil_func}) {
            key.append(word);
        }
        return key.take();
    };
    // Learn D3D11_DEPTH_STENCIL_DESC defaults: depth test LESS writing ALL,
    // stencil off.
    DepthStencilStateDesc defaults{};
    defaults.depth_enable = 1u;
    defaults.depth_write_mask = 1u;
    defaults.depth_func = 2u;
    const DepthStencilStateDesc canonical = canonical_depth_stencil_desc(desc);
    const std::string key = key_of(canonical);
    const std::string default_key =
        key_of(canonical_depth_stencil_desc(defaults));
    GuestAddr existing = 0;
    const u32 hr = state_pool_lookup(STATE_FAMILY_DEPTH_STENCIL, key,
                                     default_key, device_owner, existing);
    if (hr != d3d11::S_OK) return hr;
    if (auto it = m_depth_stencil_states.find(existing);
        it != m_depth_stencil_states.end()) {
        // A deduplicated object: one more reference for the application
        // (measured: count 2, and no second device reference).
        (void)retain_object(existing);
        state = existing;
        return d3d11::S_OK;
    }
    const GuestAddr created =
        create_depth_stencil_state(device_owner, canonical);
    if (created == 0) return d3d11::E_FAIL;
    auto& object = m_depth_stencil_states.at(created);
    object.pool.pool_key = key;
    object.pool.runtime_default = key == default_key;
    state_pool_insert(STATE_FAMILY_DEPTH_STENCIL, device_owner, key, created);
    state = created;
    return d3d11::S_OK;
}

u32 D3D11Manager::acquire_sampler_state(GuestAddr device_owner,
                                        const SamplerStateDesc& desc,
                                        GuestAddr& state) {
    state = 0;
    const auto key_of = [device_owner](const SamplerStateDesc& d) {
        StateKey key(STATE_FAMILY_SAMPLER, device_owner);
        key.append(d.filter);
        key.append(d.address_u);
        key.append(d.address_v);
        key.append(d.address_w);
        key.append(d.mip_lod_bias);
        key.append(d.max_anisotropy);
        key.append(d.comparison_func);
        for (const float component : d.border_color) key.append(component);
        key.append(d.min_lod);
        key.append(d.max_lod);
        return key.take();
    };
    // Learn D3D11_SAMPLER_DESC defaults: MIN_MAG_MIP_LINEAR, CLAMP, no bias,
    // MaxAnisotropy 1, NEVER, border (1,1,1,1), LOD -FLT_MAX..FLT_MAX.
    SamplerStateDesc defaults{};
    defaults.filter = 0x15u;
    defaults.address_u = defaults.address_v = defaults.address_w = 3u;
    defaults.max_anisotropy = 1u;
    defaults.comparison_func = 1u;
    for (float& component : defaults.border_color) component = 1.0f;
    defaults.min_lod = -std::numeric_limits<float>::max();
    defaults.max_lod = std::numeric_limits<float>::max();
    const SamplerStateDesc canonical = canonical_sampler_desc(desc);
    const std::string key = key_of(canonical);
    const std::string default_key = key_of(canonical_sampler_desc(defaults));
    GuestAddr existing = 0;
    const u32 hr = state_pool_lookup(STATE_FAMILY_SAMPLER, key, default_key,
                                     device_owner, existing);
    if (hr != d3d11::S_OK) return hr;
    if (auto it = m_sampler_states.find(existing);
        it != m_sampler_states.end()) {
        // A deduplicated object: one more reference for the application
        // (measured: count 2, and no second device reference).
        (void)retain_object(existing);
        state = existing;
        return d3d11::S_OK;
    }
    const GuestAddr created = create_sampler_state(device_owner, canonical);
    if (created == 0) return d3d11::E_FAIL;
    auto& object = m_sampler_states.at(created);
    object.pool.pool_key = key;
    object.pool.runtime_default = key == default_key;
    state_pool_insert(STATE_FAMILY_SAMPLER, device_owner, key, created);
    state = created;
    return d3d11::S_OK;
}

} // namespace vitreo::winapi
