// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_sampler_binding.cpp
/// @brief Sampler resolution, the per-stage binding summaries and their R4
///        memos, and R3c native-state settlement.
///        Split out of d3d11_pipeline.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

u32 D3D11Manager::border_twin_mask(std::span<const GuestAddr> samplers) const {
    u32 mask = 0;
    for (u32 slot = 0; slot < kD3D11SamplerSlotCount && slot < samplers.size();
         ++slot) {
        const SamplerState* samp = find_sampler_state(samplers[slot]);
        if (samp == nullptr) continue;
        const auto& desc = samp->desc;
        const bool uses_border = desc.address_u == 4u ||
                                 desc.address_v == 4u || desc.address_w == 4u;
        const bool comparison = (desc.filter & 0x180u) == 0x80u;
        // Only transparent black and opaque white are Metal colours that
        // hold for every format. Metal's opaque black gives an A8_UNORM view
        // alpha 0 where D3D11.3 7.18.9.1 wants the border's alpha, 1
        // (measured, d3d11_a07_sampler_probe `M5 A8_UNORM border(0,0,0,1)`);
        // the twin blend is exact for it as for any other colour.
        const float* c = desc.border_color;
        const bool metal_colour =
            (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) ||
            (c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f);
        // A comparison filter compares the border's red alone (D3D11.3
        // 7.18.9.1): 0 and 1 are Metal borders, any other value is
        // compared in the shader through the twin.
        const bool metal_compare = c[0] == 0.0f || c[0] == 1.0f;
        if (uses_border && (comparison ? !metal_compare : !metal_colour)) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

std::array<float, 4> D3D11Manager::border_twin_push_colour(
    GuestAddr sampler_address) const {
    std::array<float, 4> colour{};
    const SamplerState* samp = find_sampler_state(sampler_address);
    if (samp == nullptr) return colour;
    const auto& desc = samp->desc;
    if ((desc.filter & 0x180u) == 0x80u) {
        colour[0] = desc.border_color[0];
        colour[1] = static_cast<float>(desc.comparison_func);
        return colour;
    }
    std::copy_n(desc.border_color, 4, colour.begin());
    return colour;
}

// D3D11 sampler state -> backend sampler. An s# register is independent of
// every t#: a shader may sample t2 through s1, so a sampler is never decoded
// from, or bound only beside, the texture of the same number. How many mips
// a sample can reach is the texture VIEW's business -- its MostDetailedMip /
// MipLevels window -- so the sampler always filters across whatever the view
// exposes. Returns false, leaving @p backend_sampler as it was, for an empty
// or unknown slot.
namespace {

// D3D11.3 7.18.10: a point mip filter takes mip floor(LOD + 0.5), so an exact
// .5 rounds up. Apple GPUs move to the next mip only from a fraction of
// 0.515625 = 0.5 + 1/64 (measured on an M4 Pro: LOD 0.5146 -> mip 0,
// 0.515625 -> mip 1, at every level), so for an implicit LOD the whole LOD --
// shader bias and both sampler clamps -- is shifted by 1/64: clamp(L + b, lo,
// hi) + 1/64 is D3D's LOD with Metal's threshold at D3D's. The min/mag choice
// (LOD <= 0 magnifies) would shift with it, so a sampler whose min and mag
// filters differ keeps Metal's rounding. Explicit LODs are rounded exactly in
// the shader, which subtracts the nudge again (kPointMipNudgeMaskOffset).
[[nodiscard]] bool point_mip_tie_nudged(u32 filter) {
    return d3d_point_mip_tie_nudged(filter);
}

} // namespace

float D3D11Manager::sampler_mip_lod_bias(GuestAddr sampler_address) const {
    const SamplerState* samp = find_sampler_state(sampler_address);
    if (samp == nullptr) return 0.0f;
    return samp->desc.mip_lod_bias +
           (point_mip_tie_nudged(samp->desc.filter)
                ? graphics::kPointMipTieNudge
                : 0.0f);
}

u32 D3D11Manager::point_mip_nudge_mask(
    std::span<const GuestAddr> samplers) const {
    u32 mask = 0;
    for (usize slot = 0; slot < samplers.size() && slot < 32u; ++slot) {
        const SamplerState* samp = find_sampler_state(samplers[slot]);
        if (samp == nullptr) continue;
        if (point_mip_tie_nudged(samp->desc.filter)) mask |= 1u << slot;
    }
    return mask;
}

u32 D3D11Manager::minmax_sampler_mask(
    std::span<const GuestAddr> samplers) const {
    u32 mask = 0;
    for (usize slot = 0; slot < samplers.size() && slot < kD3D11SamplerSlotCount;
         ++slot) {
        const SamplerState* samp = find_sampler_state(samplers[slot]);
        if (samp == nullptr) continue;
        if (((samp->desc.filter >> 7u) & 0x3u) >= 2u) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

void D3D11Manager::fill_minmax_samplers(
    std::span<u32> words, std::span<const GuestAddr> samplers) const {
    constexpr usize kBase = graphics::kMinMaxSamplerOffset / 4;
    for (usize slot = 0; slot < samplers.size() &&
                         slot < graphics::kMinMaxSamplerSlots &&
                         kBase + slot * 4 + 3 < words.size();
         ++slot) {
        const SamplerState* samp = find_sampler_state(samplers[slot]);
        if (samp == nullptr || ((samp->desc.filter >> 7u) & 0x3u) < 2u) {
            continue;
        }
        const auto& desc = samp->desc;
        u32* entry = words.data() + kBase + slot * 4;
        entry[0] = desc.filter;
        std::memcpy(entry + 1, &desc.min_lod, sizeof(float));
        std::memcpy(entry + 2, &desc.max_lod, sizeof(float));
        // WARP filters with the power of two at or below MaxAnisotropy
        // (measured, d3d11_minmax_probe [aniso]: 3 filters as 2, 6 as 4);
        // 0 -- legal, D3D11_MIN_MAXANISOTROPY -- as 1.
        u32 aniso = 1;
        while (aniso * 2u <= desc.max_anisotropy) aniso *= 2u;
        entry[3] = aniso;
    }
}

// ---- R4 (d3d11_draw_memo.hpp)
namespace {
[[noreturn]] void draw_memo_verify_failed(const char* what, u32 stage) {
    std::fprintf(stderr,
                 "[r4] VERIFY FAILED: the memoised %s of stage %u differs "
                 "from the recomputed one\n",
                 what, stage);
    std::abort();
}
void draw_memo_tick(const char* what, u64 hits, u64 misses) {
    if (!d3d11::draw_memo::count_tick(hits + misses)) return;
    std::fprintf(stderr, "[r4] %s hits=%llu misses=%llu\n", what,
                 static_cast<unsigned long long>(hits),
                 static_cast<unsigned long long>(misses));
}
}  // namespace

const D3D11Manager::SrvStageSummary& D3D11Manager::srv_stage_summary(
    u32 stage, const d3d11::SrvSlots& bound) const {
    const std::span<const GuestAddr> slots = bound;
    if (!memo_on(d3d11::draw_memo::Part::StageMemo)) {
        return srv_stage_summary(stage, slots);
    }
    SrvStageSummary& s = m_srv_summary[stage];
    if (s.stamped &&
        (s.stamp == bound.stamp() || d3d11::draw_memo::ignore_keys())) {
        ++m_r4_stage_hits;
        if (d3d11::draw_memo::verify()) {
            const SrvStageSummary kept = s;
            s.generation = 0;  // force the recompute
            const SrvStageSummary& fresh = srv_stage_summary(stage, slots);
            // (`bound` is not kept by the stamped rebuild below.)
            if (fresh.rgb32 != kept.rgb32 ||
                fresh.ms1 != kept.ms1 ||
                fresh.rgb32_unservable != kept.rgb32_unservable ||
                fresh.ms1_unservable != kept.ms1_unservable ||
                fresh.buffer_size_words != kept.buffer_size_words) {
                draw_memo_verify_failed("SRV summary", stage);
            }
            s.stamped = true;
            s.stamp = bound.stamp();
        }
        draw_memo_tick("stage-memo", m_r4_stage_hits, m_r4_stage_misses);
        return s;
    }
    ++m_r4_stage_misses;
    // R4c: rebuilt over the used slots only (every slot past used() is
    // empty), without the content-compared memo's 1 KB key. That memo is
    // invalidated (generation 0) since `bound` is not kept.
    const usize used = bound.used();
    const std::span<const GuestAddr> live = slots.first(used);
    s.generation = 0;
    s.rgb32_unservable = false;
    s.ms1_unservable = false;
    s.rgb32 = rgb32_buffer_srv_mask(live, s.rgb32_unservable);
    s.ms1 = single_sample_ms_srv_mask(live, s.ms1_unservable);
    std::fill_n(s.buffer_size_words.begin(), std::max(s.size_used, used), 0u);
    for (usize slot = 0; slot < used; ++slot) {
        if (bound[slot] != 0) {
            s.buffer_size_words[slot] = srv_buffer_size_words(bound[slot]);
        }
    }
    s.size_used = used;
    s.stamped = true;
    s.stamp = bound.stamp();
    return s;
}

const D3D11Manager::SamplerStageSummary& D3D11Manager::sampler_stage_summary(
    u32 stage, const d3d11::SamplerSlots& bound) const {
    const std::span<const GuestAddr> slots = bound;
    if (!memo_on(d3d11::draw_memo::Part::StageMemo)) {
        return sampler_stage_summary(stage, slots);
    }
    SamplerStageSummary& s = m_sampler_summary[stage];
    if (s.stamped &&
        (s.stamp == bound.stamp() || d3d11::draw_memo::ignore_keys())) {
        ++m_r4_stage_hits;
        if (d3d11::draw_memo::verify()) {
            const SamplerStageSummary kept = s;
            s.generation = 0;
            const SamplerStageSummary& fresh = sampler_stage_summary(stage, slots);
            if (fresh.bound != kept.bound ||
                fresh.border_twins != kept.border_twins ||
                fresh.minmax != kept.minmax || fresh.point_mip != kept.point_mip ||
                fresh.point_mip_nudge != kept.point_mip_nudge ||
                std::memcmp(fresh.lod_bias.data(), kept.lod_bias.data(),
                            sizeof(kept.lod_bias)) != 0 ||
                std::memcmp(fresh.border_colour.data(),
                            kept.border_colour.data(),
                            sizeof(kept.border_colour)) != 0 ||
                fresh.minmax_entry != kept.minmax_entry) {
                draw_memo_verify_failed("sampler summary", stage);
            }
            s.stamped = true;
            s.stamp = bound.stamp();
        }
        return s;
    }
    ++m_r4_stage_misses;
    const SamplerStageSummary& made = sampler_stage_summary(stage, slots);
    s.stamped = true;
    s.stamp = bound.stamp();
    return made;
}

const D3D11Manager::ResolvedSamplers& D3D11Manager::resolved_samplers(
    u32 stage, const d3d11::SamplerSlots& bound, bool with_native) {
    ResolvedSamplers& r = m_resolved_samplers[stage];
    const auto resolve_into = [&](ResolvedSamplers& out) {
        out.descs.fill(graphics::BackendSamplerDesc{});
        out.natives.fill(nullptr);
        for (u32 i = 0; i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
            (void)resolve_sampler(bound[i], out.descs[i], nullptr,
                                  with_native ? &out.natives[i] : nullptr);
        }
    };
    if (r.valid && r.with_native == with_native &&
        (r.stamp == bound.stamp() || d3d11::draw_memo::ignore_keys())) {
        ++m_r4_sampler_hits;
        if (d3d11::draw_memo::verify()) {
            ResolvedSamplers fresh{};
            resolve_into(fresh);
            for (u32 i = 0; i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
                if (!graphics::same_sampler_desc(fresh.descs[i], r.descs[i]) ||
                    fresh.descs[i].mipmaps_enabled != r.descs[i].mipmaps_enabled ||
                    fresh.natives[i] != r.natives[i]) {
                    draw_memo_verify_failed("resolved samplers", stage);
                }
            }
        }
        draw_memo_tick("sampler-memo", m_r4_sampler_hits, m_r4_sampler_misses);
        return r;
    }
    ++m_r4_sampler_misses;
    resolve_into(r);
    r.valid = true;
    r.with_native = with_native;
    r.stamp = bound.stamp();
    return r;
}
const D3D11Manager::ResolvedSrvs& D3D11Manager::resolved_srvs(
    u32 stage, const d3d11::SrvSlots& bound) {
    ResolvedSrvs& r = m_resolved_srvs[stage];
    const auto resolve_into = [&](ResolvedSrvs& out) {
        out = ResolvedSrvs{};
        for (u32 i = 0; i < ResolvedSrvs::N; ++i) {
            if (bound[i] == 0) continue;
            resolve_shader_resource(bound[i], out.textures[i], out.views[i],
                                    out.textures_3d[i]);
            resolve_buffer_resource(bound[i], out.buffers[i], out.offsets[i],
                                    out.texture_buffers[i]);
        }
    };
    if (r.valid && ((r.stamp == bound.stamp() &&
                     r.backend_gen == m_texture_backend_gen) ||
                    d3d11::draw_memo::ignore_keys())) {
        ++m_r4_srv_hits;
        if (d3d11::draw_memo::verify()) {
            ResolvedSrvs fresh{};
            resolve_into(fresh);
            for (u32 i = 0; i < ResolvedSrvs::N; ++i) {
                if (fresh.textures[i] != r.textures[i] ||
                    std::memcmp(&fresh.views[i], &r.views[i],
                                sizeof(r.views[i])) != 0 ||
                    fresh.textures_3d[i] != r.textures_3d[i] ||
                    fresh.buffers[i] != r.buffers[i] ||
                    fresh.offsets[i] != r.offsets[i] ||
                    fresh.texture_buffers[i] != r.texture_buffers[i]) {
                    draw_memo_verify_failed("resolved SRVs", stage);
                }
            }
        }
        draw_memo_tick("srv-memo", m_r4_srv_hits, m_r4_srv_misses);
        return r;
    }
    ++m_r4_srv_misses;
    resolve_into(r);
    r.valid = true;
    r.stamp = bound.stamp();
    r.backend_gen = m_texture_backend_gen;
    return r;
}
const D3D11Manager::ResolvedVbs& D3D11Manager::resolved_vertex_buffers(
    const d3d11::StampedSlots<GuestAddr, d3d11::VERTEX_BUFFER_SLOTS>& bound) {
    ResolvedVbs& r = m_resolved_vbs;
    const auto resolve_into = [&](ResolvedVbs& out) {
        std::fill_n(out.buffers.begin(), std::max(out.used, bound.used()),
                    nullptr);
        out.used = bound.used();
        for (usize slot = 0; slot < out.used; ++slot) {
            if (bound[slot] == 0) continue;
            const auto found = m_buffers.find(bound[slot]);
            if (found != m_buffers.end()) {
                out.buffers[slot] = found->second.backend.get();
            }
        }
    };
    if (r.valid &&
        (r.stamp == bound.stamp() || d3d11::draw_memo::ignore_keys())) {
        ++m_r4_vb_hits;
        if (d3d11::draw_memo::verify()) {
            ResolvedVbs fresh{};
            fresh.used = d3d11::VERTEX_BUFFER_SLOTS;
            resolve_into(fresh);
            for (usize slot = 0; slot < d3d11::VERTEX_BUFFER_SLOTS; ++slot) {
                if (fresh.buffers[slot] != r.buffers[slot]) {
                    draw_memo_verify_failed("vertex buffers", 0);
                }
            }
        }
        draw_memo_tick("vb-memo", m_r4_vb_hits, m_r4_vb_misses);
        return r;
    }
    ++m_r4_vb_misses;
    resolve_into(r);
    r.valid = true;
    r.stamp = bound.stamp();
    return r;
}
// ---- end R4

const D3D11Manager::SrvStageSummary& D3D11Manager::srv_stage_summary(
    u32 stage, std::span<const GuestAddr> bound) const {
    SrvStageSummary& s = m_srv_summary[stage];
    const usize n = std::min<usize>(bound.size(), d3d11::SHADER_RESOURCE_SLOTS);
    if (s.generation == m_srv_set_generation &&
        std::equal(bound.begin(), bound.begin() + static_cast<std::ptrdiff_t>(n),
                   s.bound.begin()) &&
        std::all_of(s.bound.begin() + static_cast<std::ptrdiff_t>(n),
                    s.bound.end(), [](GuestAddr a) { return a == 0; })) {
        return s;
    }
    s = SrvStageSummary{};
    std::copy_n(bound.begin(), n, s.bound.begin());
    s.generation = m_srv_set_generation;
    s.rgb32 = rgb32_buffer_srv_mask(bound, s.rgb32_unservable);
    s.ms1 = single_sample_ms_srv_mask(bound, s.ms1_unservable);
    static_assert(graphics::kGuardSizeSrvSlots == d3d11::SHADER_RESOURCE_SLOTS);
    for (usize slot = 0; slot < n; ++slot) {
        if (bound[slot] != 0) {
            s.buffer_size_words[slot] = srv_buffer_size_words(bound[slot]);
        }
    }
    return s;
}

const D3D11Manager::SamplerStageSummary& D3D11Manager::sampler_stage_summary(
    u32 stage, std::span<const GuestAddr> bound) const {
    SamplerStageSummary& s = m_sampler_summary[stage];
    const usize n = std::min<usize>(bound.size(), d3d11::SAMPLER_SLOTS);
    if (s.generation == m_sampler_set_generation &&
        std::equal(bound.begin(), bound.begin() + static_cast<std::ptrdiff_t>(n),
                   s.bound.begin()) &&
        std::all_of(s.bound.begin() + static_cast<std::ptrdiff_t>(n),
                    s.bound.end(), [](GuestAddr a) { return a == 0; })) {
        return s;
    }
    s = SamplerStageSummary{};
    std::copy_n(bound.begin(), n, s.bound.begin());
    s.generation = m_sampler_set_generation;
    s.border_twins = border_twin_mask(bound);
    s.minmax = minmax_sampler_mask(bound);
    s.point_mip = point_mip_sampler_mask(bound);
    s.point_mip_nudge = point_mip_nudge_mask(bound);
    for (usize slot = 0; slot < n; ++slot) {
        s.lod_bias[slot] = sampler_mip_lod_bias(bound[slot]);
        if ((s.border_twins & (1u << slot)) != 0) {
            s.border_colour[slot] = border_twin_push_colour(bound[slot]);
        }
    }
    static_assert(graphics::kMinMaxSamplerSlots == d3d11::SAMPLER_SLOTS);
    std::array<u32, graphics::kPushBlockBytes / 4> words{};
    fill_minmax_samplers(words, bound);
    for (usize slot = 0; slot < d3d11::SAMPLER_SLOTS; ++slot) {
        std::copy_n(words.begin() + graphics::kMinMaxSamplerOffset / 4 +
                        static_cast<std::ptrdiff_t>(slot * 4),
                    4, s.minmax_entry[slot].begin());
    }
    return s;
}

std::vector<u32> D3D11Manager::sampler_push_block(
    std::span<const GuestAddr> samplers) const {
    std::vector<u32> words(graphics::kPushBlockBytes / 4, 0u);
    fill_minmax_samplers(words, samplers);
    for (usize slot = 0; slot < samplers.size() && slot < kD3D11SamplerSlotCount;
         ++slot) {
        const float bias = sampler_mip_lod_bias(samplers[slot]);
        std::memcpy(words.data() + slot, &bias, sizeof(bias));
    }
    words[graphics::kPointMipMaskOffset / 4] = point_mip_sampler_mask(samplers);
    words[graphics::kPointMipNudgeMaskOffset / 4] =
        point_mip_nudge_mask(samplers);
    return words;
}

u32 D3D11Manager::point_mip_sampler_mask(
    std::span<const GuestAddr> samplers) const {
    // D3D11_FILTER bit 0 is the mip filter; ANISOTROPIC (low seven bits
    // 0x55) always selects mips linearly (7.18.10).
    u32 mask = 0;
    for (usize slot = 0; slot < samplers.size() && slot < 32u; ++slot) {
        const SamplerState* samp = find_sampler_state(samplers[slot]);
        if (samp == nullptr) continue;
        const u32 filter_base = samp->desc.filter & 0x7fu;
        if (filter_base != 0x55u && (filter_base & 0x1u) == 0u) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

bool D3D11Manager::resolve_sampler(GuestAddr sampler_address,
                                   graphics::BackendSamplerDesc& backend_sampler,
                                   float* shader_lod_bias,
                                   const graphics::BackendSamplerState** native) {
    const SamplerState* samp = find_sampler_state(sampler_address);
    if (samp == nullptr) {
        // ---- R3c native-state: an empty slot binds the default sampler.
        if (native != nullptr) {
            if (!m_default_sampler_state_tried) {
                m_default_sampler_state_tried = true;
                m_default_sampler_state = m_backend.create_sampler_state(
                    graphics::BackendSamplerDesc{});
            }
            *native = m_default_sampler_state.get();
        }
        // ---- end R3c
        return false;
    }
    const auto& desc = samp->desc;
    D3DSamplerFields fields{};
    fields.filter = desc.filter;
    fields.address_u = desc.address_u;
    fields.address_v = desc.address_v;
    fields.address_w = desc.address_w;
    fields.mip_lod_bias = desc.mip_lod_bias;
    fields.max_anisotropy = desc.max_anisotropy;
    fields.comparison_func = desc.comparison_func;
    for (usize c = 0; c < 4; ++c) fields.border[c] = desc.border_color[c];
    fields.min_lod = desc.min_lod;
    fields.max_lod = desc.max_lod;
    d3d_backend_sampler(fields, backend_sampler, shader_lod_bias);
    // ---- R3c native-state: the state built from this desc, on the object's
    // first draw. settle_native_states drops it if the slot's final desc is
    // not the one it was built from.
    if (native != nullptr) {
        if (!samp->backend_state_tried) {
            samp->backend_state_tried = true;
            samp->backend_state = m_backend.create_sampler_state(backend_sampler);
        }
        *native = samp->backend_state.get();
    }
    // ---- end R3c
    return true;
}

// ---- R3c native-state (graphics/contracts/delta_record.hpp)
void D3D11Manager::settle_native_states(graphics::BackendDrawCall& call,
                                        graphics::BackendNativeStates& states,
                                        const DepthStencilState* dss,
                                        bool samplers_settled) {
    call.native_states = &states;
    // R4 sampler-memo: states taken from the memo were resolved with the
    // very descs the call carries, and nothing overrides an s# desc after
    // resolution, so their check is skipped (verify mode keeps it).
    for (u32 i = 0;
         !samplers_settled && i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
        if (states.samplers[i] != nullptr &&
            !graphics::same_sampler_desc(states.samplers[i]->desc(),
                                         call.samplers[i])) {
            states.samplers[i] = nullptr;
        }
        if (states.vertex_samplers[i] != nullptr &&
            !graphics::same_sampler_desc(states.vertex_samplers[i]->desc(),
                                         call.vertex_samplers[i])) {
            states.vertex_samplers[i] = nullptr;
        }
    }
    // The fields as they are drawn (read-only view masking, a capture draw's
    // overrides): the variant built from exactly those, or a new one.
    const graphics::BackendDepthStencilDesc fields =
        graphics::depth_stencil_desc_of(call);
    auto& variants = dss != nullptr ? dss->backend_states
                                    : m_default_depth_stencil_states;
    states.depth_stencil = nullptr;
    if (m_native_depth_stencil_unsupported) return;
    for (auto& variant : variants) {
        if (variant == nullptr) {
            variant = m_backend.create_depth_stencil_state(fields);
            // A backend without native states resolves from the fields.
            if (variant == nullptr) m_native_depth_stencil_unsupported = true;
            states.depth_stencil = variant.get();
            return;
        }
        if (graphics::same_depth_stencil_desc(variant->desc(), fields)) {
            states.depth_stencil = variant.get();
            return;
        }
    }
    // More distinct field sets than variants: resolved from the fields.
}
// ---- end R3c

} // namespace vitreo::winapi
