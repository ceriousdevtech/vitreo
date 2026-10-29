// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_state_census.cpp
/// @brief VITREO_D3D11_STATE_CENSUS=1 (see d3d11_state_census.hpp).
///
/// DIAGNOSTIC (Track R round 3, P0). Nothing here changes what a guest call
/// does: every hook only reads the state the call already produced and counts
/// it. All of it runs only when the census is on.

#include "winapi/graphics/d3d11/d3d11_state_census.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"

#include <vitreo/ab_cycle.hpp>
#include <vitreo/common.hpp>
#include <vitreo/present_cpu.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <time.h>

namespace vitreo::winapi {
namespace d3d11_census {

namespace {

bool read_enabled() noexcept {
    const char* v = std::getenv("VITREO_D3D11_STATE_CENSUS");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

}  // namespace

const bool enabled = read_enabled();

u64 now_ns() noexcept { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

namespace {

/// The draw-state groups of design §3.2 (R4's group generations), plus the
/// setters that are not draw state (compute, whole-state resets), which only
/// the "called" mask counts.
enum Group : u32 {
    kIaLayout,
    kIaVb,
    kIaIb,
    kIaTopology,
    kVsShader,
    kVsCb,
    kVsSrv,
    kVsSampler,
    kPsShader,
    kPsCb,
    kPsSrv,
    kPsSampler,
    kOtherStages,  ///< GS/HS/DS shaders and registers
    kOmTargets,    ///< RTVs, DSV, OM UAVs
    kOmBlend,      ///< blend state, blend factor, sample mask
    kOmDss,        ///< depth-stencil state, stencil ref
    kRsState,
    kRsViewports,
    kRsScissors,
    kPredicate,
    kStreamOut,
    kDrawGroups,          ///< groups above compare by value at a draw
    kCompute = kDrawGroups,
    kWholeState,          ///< ClearState, SwapDeviceContextState, ExecuteCommandList
    kGroupCount
};

constexpr std::array<const char*, kGroupCount> kGroupNames{{
    "ia_layout", "ia_vb", "ia_ib", "ia_topo", "vs_shader", "vs_cb", "vs_srv",
    "vs_samp", "ps_shader", "ps_cb", "ps_srv", "ps_samp", "gs_hs_ds",
    "om_targets", "om_blend", "om_dss", "rs_state", "rs_vp", "rs_scissor",
    "predicate", "so", "cs", "whole_state",
}};

enum class Kind : u8 { Context, Device, Child, Dxgi, Count };
constexpr std::array<const char*, 4> kKindNames{{"context", "device", "child",
                                                 "dxgi"}};

constexpr std::array<const char*, static_cast<usize>(Iface::Count)>
    kIfaceNames{{
        "Context", "Annotation", "VideoContext", "Device", "Multithread",
        "DxgiDevice", "DxgiResource", "DxgiSurface", "SwapChain",
        "SwapChainMedia", "KeyedMutex", "Buffer", "Texture", "SRV", "UAV",
        "RTV", "DSV", "Query", "Fence", "CommandList", "Shader",
        "InputLayout", "StateObject", "ClassLinkage", "DeviceContextState",
        "Other",
    }};

constexpr Kind kind_of(Iface iface) noexcept {
    switch (iface) {
    case Iface::Context:
    case Iface::Annotation:
    case Iface::VideoContext:
        return Kind::Context;
    case Iface::Device:
    case Iface::Multithread:
        return Kind::Device;
    case Iface::DxgiDevice:
    case Iface::DxgiResource:
    case Iface::DxgiSurface:
    case Iface::SwapChain:
    case Iface::SwapChainMedia:
    case Iface::KeyedMutex:
        return Kind::Dxgi;
    default:
        return Kind::Child;
    }
}

/// Context / device / swap-chain slots the census keys on, found BY NAME in
/// the SDK-generated method tables (d3d11_stdcall.hpp, d3d11_internal.hpp),
/// never by a typed slot number.
struct SlotTables {
    std::array<i8, d3d11::CONTEXT_METHOD_COUNT> context_group{};
    std::array<bool, d3d11::CONTEXT_METHOD_COUNT> context_draw{};
    usize create_buffer{~usize{0}};
    usize present{~usize{0}};
    usize present1{~usize{0}};
};

const SlotTables& slot_tables() {
    static const SlotTables tables = [] {
        SlotTables t{};
        t.context_group.fill(-1);
        const auto starts = [](std::string_view name, std::string_view p) {
            return name.substr(0, p.size()) == p;
        };
        for (usize i = 0; i < d3d11::CONTEXT_METHOD_COUNT; ++i) {
            const std::string_view n = d3d11::CONTEXT_METHOD_NAMES[i];
            int g = -1;
            if (n == "IASetInputLayout") g = kIaLayout;
            else if (n == "IASetVertexBuffers") g = kIaVb;
            else if (n == "IASetIndexBuffer") g = kIaIb;
            else if (n == "IASetPrimitiveTopology") g = kIaTopology;
            else if (n == "VSSetShader") g = kVsShader;
            else if (starts(n, "VSSetConstantBuffers")) g = kVsCb;
            else if (n == "VSSetShaderResources") g = kVsSrv;
            else if (n == "VSSetSamplers") g = kVsSampler;
            else if (n == "PSSetShader") g = kPsShader;
            else if (starts(n, "PSSetConstantBuffers")) g = kPsCb;
            else if (n == "PSSetShaderResources") g = kPsSrv;
            else if (n == "PSSetSamplers") g = kPsSampler;
            else if (starts(n, "GSSet") || starts(n, "HSSet") ||
                     starts(n, "DSSet")) g = kOtherStages;
            else if (starts(n, "OMSetRenderTargets")) g = kOmTargets;
            else if (n == "OMSetBlendState") g = kOmBlend;
            else if (n == "OMSetDepthStencilState") g = kOmDss;
            else if (n == "RSSetState") g = kRsState;
            else if (n == "RSSetViewports") g = kRsViewports;
            else if (n == "RSSetScissorRects") g = kRsScissors;
            else if (n == "SetPredication") g = kPredicate;
            else if (n == "SOSetTargets") g = kStreamOut;
            else if (starts(n, "CSSet")) g = kCompute;
            else if (n == "ClearState" || n == "SwapDeviceContextState" ||
                     n == "ExecuteCommandList") g = kWholeState;
            t.context_group[i] = static_cast<i8>(g);
            t.context_draw[i] = starts(n, "Draw");
        }
        for (usize i = 0; i < d3d11::DEVICE_METHOD_COUNT; ++i) {
            if (std::string_view(DEVICE_METHOD_NAMES[i]) == "CreateBuffer") {
                t.create_buffer = i;
            }
        }
        for (usize i = 0; i < d3d11::SWAP_CHAIN_METHOD_COUNT; ++i) {
            const std::string_view n = SWAP_CHAIN_METHOD_NAMES[i];
            if (n == "Present") t.present = i;
            if (n == "Present1") t.present1 = i;
        }
        return t;
    }();
    return tables;
}

/// IUnknown's vtable (unknwn.h): QueryInterface, AddRef, Release lead every
/// COM interface, so slots 1 and 2 are AddRef and Release on all of them.
constexpr u32 kAddRefSlot = 1;
constexpr u32 kReleaseSlot = 2;
static_assert(std::string_view(d3d11::CONTEXT_METHOD_NAMES[kAddRefSlot]) == "AddRef");
static_assert(std::string_view(d3d11::CONTEXT_METHOD_NAMES[kReleaseSlot]) ==
              "Release");

/// ID3D11Multithread (d3d11_4.h), as impl_MultithreadMethod serves it:
/// IUnknown, then Enter, Leave, SetMultithreadProtected,
/// GetMultithreadProtected.
constexpr u32 kMtEnter = 3;
constexpr u32 kMtLeave = 4;
constexpr u32 kMtSetProtected = 5;

/// Hold / wait histogram: <256 ns, then doubling, the last bucket >= 512 us.
constexpr usize kTimeBuckets = 13;
usize time_bucket(u64 ns) noexcept {
    if (ns < 256) return 0;
    const usize b = static_cast<usize>(std::bit_width(ns >> 8));  // 256..511 -> 1
    return std::min(b, kTimeBuckets - 1);
}

struct MethodRow {
    u64 count{0};
    u64 hold_ns{0};
    u64 wait_ns{0};
};

/// Everything counted in the current print window (2^16 guest draws).
struct Window {
    u64 frames{0};
    // Guest draws (execute_draw entries with no emulation variant).
    u64 guest_draws{0};
    u64 crossing_draws{0};     ///< came straight from a Draw* crossing
    u64 replayed_draws{0};     ///< ExecuteCommandList replays, internal draws
    u64 indexed{0};
    u64 indirect{0};
    u64 plain_bindings{0};     ///< the bindings make it a plain draw
    u64 plain{0};              ///< ... and it reached the backend once, as is
    u64 backend_draws{0};      ///< BackendRenderPass::draw calls, any kind
    u64 emulation_draws{0};    ///< ... of an emulation pass (DrawVariant)
    u64 pipeline_same{0};
    u64 pass_same{0};
    u64 occlusion_active{0};
    u64 distinct_pipelines{0};  ///< summed over frame ends
    u64 distinct_layout_vs{0};
    std::array<u64, kGroupCount> called{};
    std::array<u64, kGroupCount> changed{};
    std::array<u64, 10> changed_hist{};  ///< groups changed: 0..8, 9+
    std::array<u64, 10> called_hist{};
    u64 vb_slots{0}, vs_cb_slots{0}, ps_cb_slots{0}, vs_srv_slots{0},
        ps_srv_slots{0}, vs_samp_slots{0}, ps_samp_slots{0};
    // Crossings.
    std::array<u64, static_cast<usize>(Iface::Count)> calls{};
    std::array<u64, static_cast<usize>(Iface::Count)> addref{};
    std::array<u64, static_cast<usize>(Iface::Count)> release{};
    std::array<std::array<u64, kTimeBuckets>, 4> hold_hist{};
    std::array<std::array<u64, kTimeBuckets>, 4> wait_hist{};
    std::array<u64, 4> hold_ns{};
    std::array<u64, 4> wait_ns{};
    std::array<u64, 4> contended{};   ///< waits >= 1 us
    std::unordered_map<u32, MethodRow> methods;  ///< (iface << 16) | slot
    // ID3D11Multithread.
    u64 mt_enter{0}, mt_leave{0}, mt_set_true{0}, mt_set_false{0};
    std::unordered_map<u32, u64> create_buffer_by_tid;
};

struct State {
    std::mutex mu;
    Window w;
    u64 window_index{0};
    bool announced{false};
    // Per frame (cleared at each Present).
    std::unordered_set<const void*> frame_pipelines;
    std::unordered_set<u64> frame_layout_vs;
    // The guest draw in flight (execute_draw is entered again for its
    // emulation passes before it returns).
    bool pending{false};
    bool pending_plain_bindings{false};
    u32 pending_backend{0};
    u32 pending_emulation{0};
    // Per context: the previous direct backend draw's pipeline key and pass.
    struct LastBackend {
        u64 pipeline_key{0};
        graphics::BackendRenderPassDesc pass{};
        bool valid{false};
    };
    std::unordered_map<GuestAddr, LastBackend> last_backend;
};

State& state() {
    static State* s = new State();  // immortal, like the other meters
    return *s;
}

/// The groups the calling thread's context setters called since its last
/// guest draw, and whether a Draw* crossing is running on it.
thread_local u32 t_called = 0;
thread_local bool t_in_draw_crossing = false;

void finish_pending_locked(State& s) {
    if (!s.pending) return;
    s.pending = false;
    if (s.pending_plain_bindings && s.pending_backend == 1 &&
        s.pending_emulation == 0) {
        ++s.w.plain;
    }
}

double per(u64 n, u64 d) { return d == 0 ? 0.0 : static_cast<double>(n) / d; }
double pct(u64 n, u64 d) { return 100.0 * per(n, d); }

const char* method_label(Iface iface, u32 slot, char* buf, usize cap) {
    if (iface == Iface::Context && slot < d3d11::CONTEXT_METHOD_NAMES.size()) {
        return d3d11::CONTEXT_METHOD_NAMES[slot];
    }
    if (iface == Iface::Device && slot < DEVICE_METHOD_NAMES.size()) {
        return DEVICE_METHOD_NAMES[slot];
    }
    if (iface == Iface::SwapChain && slot < SWAP_CHAIN_METHOD_NAMES.size()) {
        return SWAP_CHAIN_METHOD_NAMES[slot];
    }
    static constexpr std::array<const char*, 3> kUnknown{
        {"QueryInterface", "AddRef", "Release"}};
    if (slot < 3) {
        std::snprintf(buf, cap, "%s::%s",
                      kIfaceNames[static_cast<usize>(iface)], kUnknown[slot]);
    } else {
        std::snprintf(buf, cap, "%s#%u",
                      kIfaceNames[static_cast<usize>(iface)], slot);
    }
    return buf;
}

}  // namespace

void before_method(const Crossing& c, const Marshaler& m) noexcept {
    const SlotTables& tables = slot_tables();
    if (c.iface == Iface::Context && c.slot < d3d11::CONTEXT_METHOD_COUNT) {
        const i8 g = tables.context_group[c.slot];
        if (g >= 0) t_called |= 1u << static_cast<u32>(g);
        if (tables.context_draw[c.slot]) t_in_draw_crossing = true;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.announced) {
        s.announced = true;
        std::fprintf(stderr,
                     "[r3-census] on: VITREO_D3D11_STATE_CENSUS (diagnostic, "
                     "no behaviour change), a report every 65536 guest "
                     "draws\n");
    }
    const usize i = static_cast<usize>(c.iface);
    ++s.w.calls[i];
    if (c.slot == kAddRefSlot) ++s.w.addref[i];
    if (c.slot == kReleaseSlot) ++s.w.release[i];
    if (c.iface == Iface::Device && c.slot == tables.create_buffer) {
        ++s.w.create_buffer_by_tid[g_vitreo_trace_tid];
    }
    if (c.iface == Iface::Multithread) {
        if (c.slot == kMtEnter) ++s.w.mt_enter;
        if (c.slot == kMtLeave) ++s.w.mt_leave;
        if (c.slot == kMtSetProtected) {
            if (static_cast<u32>(m.arg(1)) != 0) ++s.w.mt_set_true;
            else ++s.w.mt_set_false;
        }
    }
}

void after_method(const Crossing& c) noexcept {
    const SlotTables& tables = slot_tables();
    if (c.iface == Iface::Context && c.slot < d3d11::CONTEXT_METHOD_COUNT &&
        tables.context_draw[c.slot]) {
        t_in_draw_crossing = false;
    }
    const u64 t_exit = now_ns();
    const u64 wait = c.t_locked - c.t_enter;
    const u64 hold = t_exit - c.t_locked;
    const usize k = static_cast<usize>(kind_of(c.iface));
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    ++s.w.wait_hist[k][time_bucket(wait)];
    s.w.wait_ns[k] += wait;
    if (wait >= 1000) ++s.w.contended[k];
    MethodRow& row =
        s.w.methods[(static_cast<u32>(c.iface) << 16) | (c.slot & 0xFFFFu)];
    ++row.count;
    row.wait_ns += wait;
    if (c.outer) {
        ++s.w.hold_hist[k][time_bucket(hold)];
        s.w.hold_ns[k] += hold;
        row.hold_ns += hold;
    }
    if (c.iface == Iface::SwapChain &&
        (c.slot == tables.present || c.slot == tables.present1)) {
        ++s.w.frames;
        s.w.distinct_pipelines += s.frame_pipelines.size();
        s.w.distinct_layout_vs += s.frame_layout_vs.size();
        s.frame_pipelines.clear();
        s.frame_layout_vs.clear();
    }
}

}  // namespace d3d11_census

// -- D3D11Manager hooks ------------------------------------------------------

namespace {

/// Slots of @p cur that differ from @p prev (then copied over), 0 when equal.
template <class A>
u32 census_changed_slots(A& prev, const A& cur) {
    if (std::memcmp(prev.data(), cur.data(), sizeof(cur)) == 0) return 0;
    u32 n = 0;
    for (usize i = 0; i < cur.size(); ++i) {
        if (std::memcmp(&prev[i], &cur[i], sizeof(cur[i])) != 0) ++n;
    }
    prev = cur;
    return n;
}

/// Slots where any of three parallel arrays differ (then copied over).
template <class A, class B, class C>
u32 census_changed_slots3(A& pa, const A& ca, B& pb, const B& cb, C& pc,
                          const C& cc) {
    if (std::memcmp(pa.data(), ca.data(), sizeof(ca)) == 0 &&
        std::memcmp(pb.data(), cb.data(), sizeof(cb)) == 0 &&
        std::memcmp(pc.data(), cc.data(), sizeof(cc)) == 0) {
        return 0;
    }
    u32 n = 0;
    for (usize i = 0; i < ca.size(); ++i) {
        if (pa[i] != ca[i] || pb[i] != cb[i] || pc[i] != cc[i]) ++n;
    }
    pa = ca;
    pb = cb;
    pc = cc;
    return n;
}

template <class T>
bool census_changed(T& prev, const T& cur) {
    if (std::memcmp(&prev, &cur, sizeof(cur)) == 0) return false;
    prev = cur;
    return true;
}

bool census_same_pass(const graphics::BackendRenderPassDesc& a,
                      const graphics::BackendRenderPassDesc& b) {
    return a.color_targets == b.color_targets &&
           a.color_formats == b.color_formats &&
           a.color_levels == b.color_levels &&
           a.color_slices == b.color_slices &&
           a.color_array_sizes == b.color_array_sizes &&
           a.color_targets_3d == b.color_targets_3d &&
           a.color_depth_planes == b.color_depth_planes &&
           a.preserve_color_contents == b.preserve_color_contents &&
           a.depth_stencil_target == b.depth_stencil_target &&
           a.depth_level == b.depth_level && a.depth_slice == b.depth_slice &&
           a.depth_array_size == b.depth_array_size &&
           a.clear_depth == b.clear_depth &&
           a.depth_clear_value == b.depth_clear_value &&
           a.attachmentless_width == b.attachmentless_width &&
           a.attachmentless_height == b.attachmentless_height &&
           a.attachmentless_samples == b.attachmentless_samples &&
           a.attachmentless_layers == b.attachmentless_layers &&
           a.center_sample_positions == b.center_sample_positions &&
           a.occlusion_counter == b.occlusion_counter;
}

}  // namespace

std::vector<std::pair<GuestAddr, bool>>
D3D11Manager::census_device_protection() const {
    std::vector<std::pair<GuestAddr, bool>> out;
    out.reserve(m_devices.size());
    for (const auto& [owner, device] : m_devices) {
        out.emplace_back(owner, device.multithread_protected);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void D3D11Manager::census_guest_draw(GuestAddr context,
                                     const ContextState& state, bool indexed,
                                     bool indirect) {
    using namespace d3d11_census;
    // The previous draw's bindings, per context (the census's own copy).
    static auto* snapshots =
        new std::unordered_map<GuestAddr, ContextBindings>();
    State& s = d3d11_census::state();
    std::unique_lock<std::mutex> lock(s.mu);
    finish_pending_locked(s);
    Window& w = s.w;
    ++w.guest_draws;
    if (indexed) ++w.indexed;
    if (indirect) ++w.indirect;
    u32 called = 0;
    if (t_in_draw_crossing) {
        ++w.crossing_draws;
        called = t_called;
        t_called = 0;
        for (u32 g = 0; g < kGroupCount; ++g) {
            if ((called >> g) & 1u) ++w.called[g];
        }
        ++w.called_hist[std::min<usize>(static_cast<usize>(std::popcount(called)), 9)];
    } else {
        ++w.replayed_draws;
    }

    // What changed by value since this context's previous draw.
    const ContextBindings& cur = state;
    u32 changed = 0;
    auto [it, fresh] = snapshots->try_emplace(context, cur);
    if (fresh) {
        changed = (1u << kDrawGroups) - 1u;  // a context's first draw
    } else {
        ContextBindings& prev = it->second;
        const auto mark = [&changed](Group g, bool c) {
            if (c) changed |= 1u << g;
        };
        mark(kIaLayout, census_changed(prev.input_layout, cur.input_layout));
        const u32 vb = census_changed_slots3(
            prev.vertex_buffers, cur.vertex_buffers, prev.vertex_buffer_strides,
            cur.vertex_buffer_strides, prev.vertex_buffer_offsets,
            cur.vertex_buffer_offsets);
        w.vb_slots += vb;
        mark(kIaVb, vb != 0);
        bool ib = census_changed(prev.index_buffer, cur.index_buffer);
        ib = census_changed(prev.index_buffer_format, cur.index_buffer_format) || ib;
        ib = census_changed(prev.index_buffer_offset, cur.index_buffer_offset) || ib;
        mark(kIaIb, ib);
        mark(kIaTopology,
             census_changed(prev.primitive_topology, cur.primitive_topology));
        mark(kVsShader, census_changed(prev.vertex_shader, cur.vertex_shader));
        const u32 vs_cb = census_changed_slots3(
            prev.vertex_constant_buffers, cur.vertex_constant_buffers,
            prev.vertex_constant_first, cur.vertex_constant_first,
            prev.vertex_constant_count, cur.vertex_constant_count);
        w.vs_cb_slots += vs_cb;
        mark(kVsCb, vs_cb != 0);
        const u32 vs_srv = census_changed_slots(prev.vertex_shader_resources,
                                                cur.vertex_shader_resources);
        w.vs_srv_slots += vs_srv;
        mark(kVsSrv, vs_srv != 0);
        const u32 vs_samp =
            census_changed_slots(prev.vertex_samplers, cur.vertex_samplers);
        w.vs_samp_slots += vs_samp;
        mark(kVsSampler, vs_samp != 0);
        mark(kPsShader, census_changed(prev.pixel_shader, cur.pixel_shader));
        const u32 ps_cb = census_changed_slots3(
            prev.pixel_constant_buffers, cur.pixel_constant_buffers,
            prev.pixel_constant_first, cur.pixel_constant_first,
            prev.pixel_constant_count, cur.pixel_constant_count);
        w.ps_cb_slots += ps_cb;
        mark(kPsCb, ps_cb != 0);
        const u32 ps_srv = census_changed_slots(prev.pixel_shader_resources,
                                                cur.pixel_shader_resources);
        w.ps_srv_slots += ps_srv;
        mark(kPsSrv, ps_srv != 0);
        const u32 ps_samp =
            census_changed_slots(prev.pixel_samplers, cur.pixel_samplers);
        w.ps_samp_slots += ps_samp;
        mark(kPsSampler, ps_samp != 0);
        bool other = census_changed(prev.geometry_shader, cur.geometry_shader);
        other = census_changed(prev.hull_shader, cur.hull_shader) || other;
        other = census_changed(prev.domain_shader, cur.domain_shader) || other;
        other = census_changed_slots(prev.geometry_constant_buffers,
                                     cur.geometry_constant_buffers) != 0 || other;
        other = census_changed_slots(prev.geometry_shader_resources,
                                     cur.geometry_shader_resources) != 0 || other;
        other = census_changed_slots(prev.geometry_samplers,
                                     cur.geometry_samplers) != 0 || other;
        mark(kOtherStages, other);
        bool om = census_changed_slots(prev.render_targets,
                                       cur.render_targets) != 0;
        om = census_changed(prev.depth_stencil_view, cur.depth_stencil_view) || om;
        om = census_changed_slots(prev.output_merger_unordered_access_views,
                                  cur.output_merger_unordered_access_views) !=
                 0 || om;
        mark(kOmTargets, om);
        bool blend = census_changed(prev.blend_state, cur.blend_state);
        blend = census_changed(prev.blend_factor, cur.blend_factor) || blend;
        blend = census_changed(prev.sample_mask, cur.sample_mask) || blend;
        mark(kOmBlend, blend);
        bool dss = census_changed(prev.depth_stencil_state,
                                  cur.depth_stencil_state);
        dss = census_changed(prev.stencil_ref, cur.stencil_ref) || dss;
        mark(kOmDss, dss);
        mark(kRsState,
             census_changed(prev.rasterizer_state, cur.rasterizer_state));
        bool vp = census_changed(prev.viewport_count, cur.viewport_count);
        vp = census_changed_slots(prev.viewports, cur.viewports) != 0 || vp;
        mark(kRsViewports, vp);
        bool sc = census_changed(prev.scissor_rect_count, cur.scissor_rect_count);
        sc = census_changed_slots(prev.scissor_rects, cur.scissor_rects) != 0 || sc;
        mark(kRsScissors, sc);
        bool pred = census_changed(prev.predicate, cur.predicate);
        pred = census_changed(prev.predicate_value, cur.predicate_value) || pred;
        mark(kPredicate, pred);
        mark(kStreamOut, census_changed_slots(prev.stream_out_targets,
                                              cur.stream_out_targets) != 0);
    }
    for (u32 g = 0; g < kDrawGroups; ++g) {
        if ((changed >> g) & 1u) ++w.changed[g];
    }
    ++w.changed_hist[std::min<usize>(static_cast<usize>(std::popcount(changed)), 9)];

    // The plain-draw bindings (design §3.2 item 4): no GS/HS/DS, no SO, no
    // predicate, no PIPELINE_STATISTICS interval, an attachment, no OM UAVs.
    // A line-quad or other emulation pass shows as an emulation backend draw
    // and makes the draw not plain (finish_pending_locked).
    const auto none = [](const auto& a) {
        return std::all_of(a.begin(), a.end(),
                           [](GuestAddr x) { return x == 0; });
    };
    const bool has_attachment =
        cur.depth_stencil_view != 0 || !none(cur.render_targets);
    const bool plain_bindings =
        cur.geometry_shader == 0 && cur.hull_shader == 0 &&
        cur.domain_shader == 0 && none(cur.stream_out_targets) &&
        cur.predicate == 0 && state.statistics_interval == nullptr &&
        none(cur.output_merger_unordered_access_views) && has_attachment;
    if (plain_bindings) ++w.plain_bindings;
    s.pending = true;
    s.pending_plain_bindings = plain_bindings;
    s.pending_backend = 0;
    s.pending_emulation = 0;
    s.frame_layout_vs.insert(
        (static_cast<u64>(cur.input_layout) * 0x9E3779B97F4A7C15ULL) ^
        static_cast<u64>(cur.vertex_shader));

    if ((w.guest_draws & 0xFFFFu) != 0) return;

    // -- The report --------------------------------------------------------
    finish_pending_locked(s);
    const u64 g = w.guest_draws;
    const u64 f = w.frames;
    std::fprintf(stderr,
                 "[r3-census] window=%llu arm=%s frames=%llu guest_draws=%llu "
                 "draws/frame=%.0f crossing=%.1f%% replayed=%.1f%% "
                 "indexed=%.1f%% indirect=%.1f%% backend_draws/draw=%.3f "
                 "emulation/draw=%.3f\n",
                 static_cast<unsigned long long>(s.window_index),
                 diagnostics::ab_cycle_arm_name(),
                 static_cast<unsigned long long>(f),
                 static_cast<unsigned long long>(g), per(g, f),
                 pct(w.crossing_draws, g), pct(w.replayed_draws, g),
                 pct(w.indexed, g), pct(w.indirect, g),
                 per(w.backend_draws, g), per(w.emulation_draws, g));
    std::fprintf(stderr,
                 "[r3-census] draw plain=%.1f%% plain_bindings=%.1f%% "
                 "pipeline_same=%.1f%% pass_same=%.1f%% occlusion=%.1f%% "
                 "distinct_pipelines/frame=%.1f distinct_layout_vs/frame=%.1f\n",
                 pct(w.plain, g), pct(w.plain_bindings, g),
                 pct(w.pipeline_same, w.backend_draws - w.emulation_draws),
                 pct(w.pass_same, w.backend_draws - w.emulation_draws),
                 pct(w.occlusion_active, w.backend_draws),
                 per(w.distinct_pipelines, f), per(w.distinct_layout_vs, f));
    char line[1024];
    int at = std::snprintf(line, sizeof(line), "[r3-census] changed%%");
    for (u32 i = 0; i < kDrawGroups; ++i) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %s=%.1f", kGroupNames[i], pct(w.changed[i], g));
    }
    std::fprintf(stderr, "%s\n", line);
    at = std::snprintf(line, sizeof(line),
                       "[r3-census] called%% (of crossing draws)");
    for (u32 i = 0; i < kGroupCount; ++i) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %s=%.1f", kGroupNames[i],
                            pct(w.called[i], w.crossing_draws));
    }
    std::fprintf(stderr, "%s\n", line);
    std::fprintf(stderr,
                 "[r3-census] slots-changed/draw vb=%.2f vs_cb=%.2f "
                 "ps_cb=%.2f vs_srv=%.2f ps_srv=%.2f vs_samp=%.2f "
                 "ps_samp=%.2f\n",
                 per(w.vb_slots, g), per(w.vs_cb_slots, g),
                 per(w.ps_cb_slots, g), per(w.vs_srv_slots, g),
                 per(w.ps_srv_slots, g), per(w.vs_samp_slots, g),
                 per(w.ps_samp_slots, g));
    at = std::snprintf(line, sizeof(line),
                       "[r3-census] groups-changed-hist%%");
    for (usize i = 0; i < w.changed_hist.size(); ++i) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %zu%s=%.1f", i, i + 1 == w.changed_hist.size()
                                                  ? "+" : "",
                            pct(w.changed_hist[i], g));
    }
    at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                        " | groups-called-hist%%");
    for (usize i = 0; i < w.called_hist.size(); ++i) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %zu%s=%.1f", i, i + 1 == w.called_hist.size()
                                                  ? "+" : "",
                            pct(w.called_hist[i], w.crossing_draws));
    }
    std::fprintf(stderr, "%s\n", line);

    std::array<u64, 4> by_kind{};
    u64 addref = 0;
    u64 release = 0;
    for (usize i = 0; i < w.calls.size(); ++i) {
        by_kind[static_cast<usize>(kind_of(static_cast<Iface>(i)))] +=
            w.calls[i];
        addref += w.addref[i];
        release += w.release[i];
    }
    std::fprintf(stderr,
                 "[r3-census] crossings/frame context=%.0f device=%.0f "
                 "child=%.0f dxgi=%.0f addref=%.0f release=%.0f\n",
                 per(by_kind[0], f), per(by_kind[1], f), per(by_kind[2], f),
                 per(by_kind[3], f), per(addref, f), per(release, f));
    at = std::snprintf(line, sizeof(line), "[r3-census] crossings/frame by-iface");
    for (usize i = 0; i < w.calls.size(); ++i) {
        if (w.calls[i] == 0) continue;
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %s=%.0f(+%.0f/-%.0f)", kIfaceNames[i],
                            per(w.calls[i], f), per(w.addref[i], f),
                            per(w.release[i], f));
        if (at >= static_cast<int>(sizeof(line)) - 64) break;
    }
    std::fprintf(stderr, "%s\n", line);
    for (usize k = 0; k < 4; ++k) {
        at = std::snprintf(
            line, sizeof(line),
            "[r3-census] lock kind=%s hold_ms/frame=%.3f wait_ms/frame=%.3f "
            "contended(>=1us)/frame=%.1f hold-hist(<256ns,x2..)",
            kKindNames[k], per(w.hold_ns[k], f) / 1e6,
            per(w.wait_ns[k], f) / 1e6, per(w.contended[k], f));
        for (u64 n : w.hold_hist[k]) {
            at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                                " %llu", static_cast<unsigned long long>(n));
        }
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " wait-hist");
        for (u64 n : w.wait_hist[k]) {
            at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                                " %llu", static_cast<unsigned long long>(n));
        }
        std::fprintf(stderr, "%s\n", line);
    }
    std::vector<std::pair<u32, MethodRow>> rows(w.methods.begin(),
                                                w.methods.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.hold_ns + a.second.wait_ns >
               b.second.hold_ns + b.second.wait_ns;
    });
    for (usize i = 0; i < rows.size() && i < 10; ++i) {
        char buf[64];
        const auto iface = static_cast<Iface>(rows[i].first >> 16);
        const u32 slot = rows[i].first & 0xFFFFu;
        std::fprintf(stderr,
                     "[r3-census] lock top%zu %s n/frame=%.1f "
                     "hold_us/frame=%.1f wait_us/frame=%.1f\n",
                     i + 1, method_label(iface, slot, buf, sizeof(buf)),
                     per(rows[i].second.count, f),
                     per(rows[i].second.hold_ns, f) / 1e3,
                     per(rows[i].second.wait_ns, f) / 1e3);
    }
    std::vector<std::pair<u64, u32>> creators;
    u64 creates = 0;
    for (const auto& [tid, n] : w.create_buffer_by_tid) {
        creators.emplace_back(n, tid);
        creates += n;
    }
    std::sort(creators.rbegin(), creators.rend());
    at = std::snprintf(line, sizeof(line),
                       "[r3-census] createbuffer/frame total=%.1f",
                       per(creates, f));
    for (usize i = 0; i < creators.size() && i < 8; ++i) {
        char name[48] = "";
        (void)diagnostics::present_cpu::thread_name(creators[i].second, name,
                                                    sizeof(name));
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " tid%u[%s]=%.1f", creators[i].second, name,
                            per(creators[i].first, f));
    }
    std::fprintf(stderr, "%s\n", line);
    at = std::snprintf(line, sizeof(line),
                       "[r3-census] multithread enter/frame=%.2f "
                       "leave/frame=%.2f set_true=%llu set_false=%llu "
                       "devices",
                       per(w.mt_enter, f), per(w.mt_leave, f),
                       static_cast<unsigned long long>(w.mt_set_true),
                       static_cast<unsigned long long>(w.mt_set_false));
    for (const auto& [owner, prot] : census_device_protection()) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " 0x%llx=%s",
                            static_cast<unsigned long long>(owner),
                            prot ? "protected" : "unprotected");
        if (at >= static_cast<int>(sizeof(line)) - 48) break;
    }
    std::fprintf(stderr, "%s\n", line);
    ++s.window_index;
    w = Window{};
}

void D3D11Manager::census_backend_draw(
    GuestAddr context, bool emulation, u64 pipeline_key,
    const graphics::BackendRenderPassDesc& pass,
    const graphics::BackendDrawCall& call) {
    using namespace d3d11_census;
    State& s = d3d11_census::state();
    std::lock_guard<std::mutex> lock(s.mu);
    ++s.w.backend_draws;
    if (pass.occlusion_counter != nullptr) ++s.w.occlusion_active;
    s.frame_pipelines.insert(call.pipeline);
    if (s.pending) {
        ++s.pending_backend;
        if (emulation) ++s.pending_emulation;
    }
    if (emulation) {
        ++s.w.emulation_draws;
        return;
    }
    State::LastBackend& last = s.last_backend[context];
    if (last.valid) {
        if (last.pipeline_key == pipeline_key) ++s.w.pipeline_same;
        if (census_same_pass(last.pass, pass)) ++s.w.pass_same;
    }
    last.valid = true;
    last.pipeline_key = pipeline_key;
    last.pass = pass;
}

}  // namespace vitreo::winapi
