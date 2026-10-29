// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_query.cpp
/// @brief ID3D11Query / ID3D11Predicate data: the Begin / End / GetData state
///        machine, occlusion sample counts, pipeline statistics and
///        predication.
///
/// Sources: Learn (ID3D11DeviceContext::Begin / End / GetData /
/// SetPredication, D3D11_QUERY, D3D11_QUERY_MISC_FLAG,
/// D3D11_ASYNC_GETDATA_FLAG), D3D11.3 functional spec chapter 20, and the
/// Windows measurements in tools/oracle_probe/d3d11_a10_queries_probe and
/// d3d11_a10_query_rules_probe (.expected beside each).
///
/// State machine (D3D11.3 20.3). A query is created signaled with no data
/// (never issued). Begin starts building -- always from an empty range, so a
/// second Begin restarts it -- and is ignored for EVENT and TIMESTAMP. End
/// issues it; End without a Begin is Begin+End, an empty range. GetData
/// validates its arguments first (flags other than DONOTFLUSH, a DataSize
/// that is neither 0 nor the type's exact size, pData NULL with a size, or a
/// PREDICATEHINT predicate: E_INVALIDARG), then answers
/// DXGI_ERROR_INVALID_CALL for a never-issued or building query, S_FALSE
/// until the data is final (flushing unless DONOTFLUSH), and S_OK with the
/// data -- or, with DataSize 0, as a status poll whatever pData is.
///
/// Occlusion. Each context runs a sequence of INTERVALS: a new one starts at
/// every Begin and End of an OCCLUSION / OCCLUSION_PREDICATE query there, and
/// the first draw of an interval opens one backend GpuOcclusionCounter
/// (Metal: a visibility-result slot in counting mode) that every draw of the
/// interval counts into, whichever render pass or command buffer it lands
/// in. A bracket's count is the sum of the intervals it spans, so
/// overlapping and nested brackets each count exactly their own draws. The
/// count is final once the newest submission generation its counters' passes
/// were recorded in has completed on the GPU -- the generation each counter
/// reports (GpuOcclusionCounter::recorded_generation), taken by the backend
/// from the recording the pass actually went into, never one read before the
/// pass existed (another thread can submit in between).
///
/// Predication. Before each operation on Learn's SetPredication list the
/// bound predicate is evaluated; the operation is skipped when the
/// predicate's data equals PredicateValue. Stream-output statistics and
/// overflow predicates are counted on the GPU timeline (the streamed counts
/// can be GPU-sourced: geometry-shader emits, tessellation, indirect draws):
/// every streaming draw writes a stats record, each SO query building on the
/// context keeps the records of its bracket, and the data is final once that
/// work completed. An SO overflow predicate, which cannot be a hint, waits
/// for it like a guaranteed occlusion predicate. An occlusion predicate needs
/// its GPU result: when
/// the result is final it is used at once; when it is not, a PREDICATEHINT
/// predicate runs the operation ("if it is not yet sure ... draw it anyway",
/// Learn D3D11_QUERY_MISC_FLAG), and a guaranteed one waits for the
/// generation that holds the predicate's draws. The wait is the one design
/// choice here: Metal has no conditional rendering and no predicated blits,
/// so the only GPU-side alternative would be to rewrite every draw and
/// dispatch as an indirect call whose arguments a kernel zeroes -- which
/// still leaves clears, copies and updates needing the CPU answer. D3D11.3
/// 20.2 rules out "allowing a guaranteed predicated rendering operation to
/// proceed ... because of timing issues". A predicate that has no data
/// (never issued, or building) reads FALSE (measured).
///
/// Pipeline statistics. Every counter is counted, never estimated
/// (D3D11.3 20.4.7; where it allows a range, what WARP does, measured by
/// tools/oracle_probe/d3d11_pstats_probe -- see graphics/shaders/
/// pipeline_stats.hpp for the rules). The CPU counts what each direct draw
/// and dispatch decides by itself: IAVertices, and -- from the draw's own
/// vertices, or the index buffer's CPU copy when the GPU cannot have written
/// it -- IAPrimitives, VSInvocations (WARP's reuse batches), HSInvocations
/// (patches), CInvocations of a draw that reaches the clipper directly, and
/// CSInvocations (groups x threads; an indirect dispatch's argument block is
/// copied on the GPU timeline and folded in once executed). Everything else
/// is counted on the GPU timeline, only while a statistics query builds,
/// into a GPU record per Begin/End interval (as for occlusion): an indirect
/// draw or a GPU-written index buffer is assembled by vitreo_ia_resolve and
/// counted by vitreo_stats_vertices; a pixel shader variant counts
/// PSInvocations on entry; a rasterized draw's vertex shader captures its
/// outputs while it draws and vitreo_stats_primitives clips them
/// (CPrimitives); the geometry emulation's counts give GSInvocations,
/// GSPrimitives and the rasterized stream's clipper counts
/// (vitreo_stats_geometry); a tessellated draw runs on the software
/// tessellator, whose batches give DSInvocations and the clipper counts
/// (vitreo_stats_tessellation). A bracket is final once that work completed,
/// so GetData never waits for it. What a pass could not count (a capture the
/// vertex shader cannot make, a resolve past its capacity) is reported as
/// E_NOTIMPL ("isn't implemented with the passed parameter combination",
/// Direct3D 11 Return Codes) with a one-time UNIMPLEMENTED line naming the
/// counters, rather than a number.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/pipeline_stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vitreo::winapi {

namespace {

// VITREO_D3D11_QUERY_WAIT_DIAG=1: DIAGNOSTIC, not part of any contract. Every
// 5 s, per query type: GetData polls, polls answered S_FALSE, queries that
// became ready after at least one S_FALSE, and the summed time from a query's
// first S_FALSE poll to the poll that returned its result. A guest spinning on
// GetData until it gets an answer spends exactly that time waiting on the GPU.
struct QueryWaitDiag {
    struct PerType {
        u64 polls{0};
        u64 unready{0};
        u64 waited{0};
        u64 wait_ns{0};
    };
    std::mutex mu;
    std::unordered_map<GuestAddr, std::chrono::steady_clock::time_point>
        first_unready;
    std::array<PerType, 16> types{};
    std::chrono::steady_clock::time_point last{
        std::chrono::steady_clock::now()};
};

[[nodiscard]] bool query_wait_diag_enabled() {
    static const bool on =
        std::getenv("VITREO_D3D11_QUERY_WAIT_DIAG") != nullptr;
    return on;
}

void note_query_poll(GuestAddr query, u32 type, bool ready) {
    static QueryWaitDiag diag;
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    std::lock_guard<std::mutex> lock(diag.mu);
    auto& per = diag.types[type & 15u];
    ++per.polls;
    if (!ready) {
        ++per.unready;
        diag.first_unready.try_emplace(query, now);
    } else if (const auto it = diag.first_unready.find(query);
               it != diag.first_unready.end()) {
        ++per.waited;
        per.wait_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                                 it->second)
                .count());
        diag.first_unready.erase(it);
    }
    const double since =
        std::chrono::duration<double>(now - diag.last).count();
    if (since < 5.0) return;
    for (u32 t = 0; t < diag.types.size(); ++t) {
        const auto& p = diag.types[t];
        if (p.polls == 0) continue;
        std::fprintf(stderr,
                     "[d3d11-query-wait] type=%u polls=%.0f/s unready=%.0f/s "
                     "waited=%.1f/s wait=%.2f ms/s (%.3f ms each)\n",
                     t, p.polls / since, p.unready / since, p.waited / since,
                     static_cast<double>(p.wait_ns) / 1e6 / since,
                     p.waited ? static_cast<double>(p.wait_ns) / 1e6 /
                                    static_cast<double>(p.waited)
                              : 0.0);
    }
    diag.types = {};
    diag.last = now;
}

[[nodiscard]] constexpr bool is_occlusion_type(u32 type) {
    return type == D3D11_QUERY_OCCLUSION ||
           type == D3D11_QUERY_OCCLUSION_PREDICATE;
}

/// DIAGNOSTIC, observation only: with the VITREO_D3D11_SKIP_STATS switch on
/// (the environment variable or the /tmp/vitreo-d3d11-skip-stats marker,
/// d3d11_skip_stats_enabled -- re-probed at most once per second, never an
/// access() syscall per call), every OCCLUSION / OCCLUSION_PREDICATE GetData
/// that returns S_OK with data is counted as a zero or a non-zero answer,
/// and the tally is printed every 5 s in d3d11_skip_stat's line format:
///   [d3d11-skip] n=<count> fmt=GetData S_OK OCCLUSION samples=0
///   [d3d11-skip] n=<count> fmt=GetData S_OK OCCLUSION samples>0
///   [d3d11-skip] n=<count> fmt=GetData S_OK OCCLUSION_PREDICATE FALSE ...
///   [d3d11-skip] n=<count> fmt=GetData S_OK OCCLUSION_PREDICATE TRUE ...
/// It changes no answer. The caller checks the switch first, so with it off
/// the cost is d3d11_skip_stats_enabled's load.
void note_occlusion_answer(u32 type, u64 samples) {
    static constexpr std::array<const char*, 4> kFormats{
        "GetData S_OK OCCLUSION samples=0",
        "GetData S_OK OCCLUSION samples>0",
        "GetData S_OK OCCLUSION_PREDICATE FALSE (samples=0)",
        "GetData S_OK OCCLUSION_PREDICATE TRUE (samples>0)",
    };
    static std::mutex mu;
    static std::array<u64, 4> counts{};
    static auto last = std::chrono::steady_clock::now();
    const usize index = (type == D3D11_QUERY_OCCLUSION_PREDICATE ? 2u : 0u) +
                        (samples != 0 ? 1u : 0u);
    std::lock_guard<std::mutex> lk(mu);
    ++counts[index];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    for (usize i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        std::fprintf(stderr, "[d3d11-skip] n=%llu fmt=%.160s\n",
                     static_cast<unsigned long long>(counts[i]), kFormats[i]);
    }
    counts = {};
}

/// Types whose data is gathered from the draws and dispatches of the
/// context building them.
[[nodiscard]] constexpr bool counts_work(u32 type) {
    return is_occlusion_type(type) ||
           type == D3D11_QUERY_PIPELINE_STATISTICS ||
           stream_out_query_stream(type) != -2;
}

[[nodiscard]] constexpr bool begin_is_ignored(u32 type) {
    // Learn (D3D11_QUERY): EVENT and TIMESTAMP "Begin is disabled".
    return type == D3D11_QUERY_EVENT || type == D3D11_QUERY_TIMESTAMP;
}

template <typename QueryT>
void reset_range(QueryT& query) {
    query.so_stats.clear();
    query.occlusion_intervals.clear();
    query.occlusion_unmeasured = false;
    query.pipeline.fill(0);
    query.pipeline_unmeasured = 0;
    query.pending_arguments.clear();
    query.statistics_records.clear();
    query.last_work_generation = 0;
    query.untracked_work = false;
    query.gpu_point.reset();
}

template <typename QueryT>
void note_work(QueryT& query, u64 generation) {
    if (generation == 0) {
        query.untracked_work = true;
    } else {
        query.last_work_generation =
            std::max(query.last_work_generation, generation);
    }
}

template <typename QueryT>
[[nodiscard]] u64 occlusion_samples(const QueryT& query) {
    u64 total = 0;
    for (const auto& interval : query.occlusion_intervals) {
        total += interval->samples();
    }
    return total;
}

/// D3D11_QUERY_DATA_SO_STATISTICS for the query's stream, summed over the
/// stats records the GPU accounting wrote for the bracket's streaming draws
/// (stream selection: stream_out_query_counts). False when a record could
/// not be read back.
template <typename QueryT>
[[nodiscard]] bool stream_out_counts(const QueryT& query, u64& written,
                                     u64& needed, bool& overflowed) {
    std::array<u64, 4> needed_per{};
    std::array<u64, 4> written_per{};
    for (const auto& record : query.so_stats) {
        std::array<u32, graphics::SO_STATS_WORDS> words{};
        if (!record || !record->read(0, words.data(), sizeof(words))) {
            return false;
        }
        for (u32 k = 0; k < 4; ++k) {
            needed_per[k] += words[2 * k];
            written_per[k] += words[2 * k + 1];
        }
    }
    const auto counts =
        stream_out_query_counts(query.query_type, needed_per, written_per);
    written = counts.written;
    needed = counts.needed;
    overflowed = counts.overflowed;
    return true;
}

/// Vertices of one input-assembler primitive of @p topology (a patch
/// list's control points); 0 for a topology that draws nothing.
[[nodiscard]] constexpr u32 ia_vertices_per_primitive(u32 topology) {
    switch (topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return 1;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: return 2;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return 3;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return 4;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return 6;
    default:
        if (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            return topology -
                   D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1;
        }
        return 0;
    }
}

/// vitreo_ia_resolve's assembly: 0 list, 1 strip, 2 triangle strip with
/// adjacency.
[[nodiscard]] constexpr u32 ia_assembly(u32 topology) {
    switch (topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return 1;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return 2;
    default: return 0;
    }
}

[[nodiscard]] graphics::PrimitiveAssembly primitive_assembly(u32 assembly) {
    return assembly == 2   ? graphics::PrimitiveAssembly::StripAdjacency
           : assembly == 1 ? graphics::PrimitiveAssembly::Strip
                           : graphics::PrimitiveAssembly::List;
}

/// Which of an input-assembler primitive's vertices the rasterizer draws
/// (vitreo_stats_primitives' raster_pick): an adjacency primitive's own
/// vertices are 1,2 of a line's four and 0,2,4 of a triangle's six.
void set_raster_pick(u32 topology, graphics::StatsKernelParams& params) {
    switch (ia_vertices_per_primitive(topology)) {
    case 1: params.raster_pick = 0; params.raster_vertices = 1; break;
    case 2: params.raster_pick = 0x0100u; params.raster_vertices = 2; break;
    case 4: params.raster_pick = 0x0201u; params.raster_vertices = 2; break;
    case 6: params.raster_pick = 0x040200u; params.raster_vertices = 3; break;
    default: params.raster_pick = 0x020100u; params.raster_vertices = 3; break;
    }
}

void set_clip_layout(const graphics::StatsClipLayout& layout,
                     graphics::StatsKernelParams& params) {
    params.position_slot = layout.position_slot;
    params.distances = layout.distances;
    for (u32 k = 0; k < layout.distances && k < 8; ++k) {
        params.distance_slots[k] =
            (layout.distance_slot[k] << 2) | layout.distance_component[k];
    }
}

constexpr u32 kStatisticsIaBits =
    pipeline_statistic_bit(PipelineStatistic::IAVertices) |
    pipeline_statistic_bit(PipelineStatistic::IAPrimitives) |
    pipeline_statistic_bit(PipelineStatistic::VSInvocations) |
    pipeline_statistic_bit(PipelineStatistic::HSInvocations) |
    pipeline_statistic_bit(PipelineStatistic::CInvocations) |
    pipeline_statistic_bit(PipelineStatistic::CPrimitives);
constexpr u32 kStatisticsAllBits = (1u << 11) - 1u;

/// Capture and resolve buffers larger than this are declined: the draw is
/// still drawn, its statistics reported unmeasured.
constexpr u64 kMaxStatisticsBytes = 256ull << 20;

}  // namespace

// -----------------------------------------------------------------------------
// Begin / End
// -----------------------------------------------------------------------------

void D3D11Manager::stop_counting(Query& query) {
    if (query.state != Query::State::Building) return;
    const auto state = m_context_states.find(query.building_context);
    if (state != m_context_states.end()) {
        auto& list = state->second.counting_queries;
        list.erase(std::remove(list.begin(), list.end(),
                               query.interface_address),
                   list.end());
        // The next draw there opens a new interval without this query.
        if (is_occlusion_type(query.query_type)) {
            state->second.occlusion_interval.reset();
        }
        if (query.query_type == D3D11_QUERY_PIPELINE_STATISTICS) {
            state->second.statistics_interval.reset();
        }
    }
    query.building_context = 0;
}

void D3D11Manager::query_begin(GuestAddr context, GuestAddr async) {
    auto& state = m_context_states[context];
    // Learn (SetPredication): "While the predicate is set for predication,
    // calls to Begin and End are invalid."
    if (async != 0 && async == state.predicate) {
        d3d11_diag("ID3D11DeviceContext::Begin on the predicate currently "
                   "set for predication -> dropped");
        return;
    }
    const auto found = m_queries.find(async);
    const auto owner = m_interfaces.find(context);
    if (found == m_queries.end() || owner == m_interfaces.end() ||
        found->second.device_owner != owner->second.owner) {
        return;
    }
    Query& query = found->second;
    if (begin_is_ignored(query.query_type)) return;
    // D3D11.3 20.3.4: "A second D3DISSUE_BEGIN will result in the range
    // being reset" -- measured: Begin; full; Begin; half; End = half.
    stop_counting(query);
    reset_range(query);
    query.state = Query::State::Building;
    query.building_context = context;
    if (counts_work(query.query_type)) {
        state.counting_queries.push_back(async);
    }
    if (is_occlusion_type(query.query_type)) {
        state.occlusion_interval.reset();
    }
    if (query.query_type == D3D11_QUERY_PIPELINE_STATISTICS) {
        state.statistics_interval.reset();
    }
}

void D3D11Manager::query_end(GuestAddr context, GuestAddr async) {
    auto& state = m_context_states[context];
    if (async != 0 && async == state.predicate) {
        d3d11_diag("ID3D11DeviceContext::End on the predicate currently "
                   "set for predication -> dropped");
        return;
    }
    const auto found = m_queries.find(async);
    const auto owner = m_interfaces.find(context);
    if (found == m_queries.end() || owner == m_interfaces.end() ||
        found->second.device_owner != owner->second.owner) {
        return;
    }
    Query& query = found->second;
    const u32 type = query.query_type;
    if (!begin_is_ignored(type)) {
        if (query.state == Query::State::Building) {
            stop_counting(query);
        } else {
            // D3D11.3 20.3.4: End without Begin is "equivalent to ... Issue
            // with both BEGIN and END" -- an empty range (measured: S_OK,
            // OCCLUSION 0, predicates FALSE).
            reset_range(query);
            if (is_occlusion_type(type)) state.occlusion_interval.reset();
            if (type == D3D11_QUERY_PIPELINE_STATISTICS) {
                state.statistics_interval.reset();
            }
        }
    }
    if (is_occlusion_type(type)) {
        // The bracket's draws are all recorded now, and no pass joins its
        // intervals after an End on this context (stop_counting closed the
        // open one): each counter's generation is final. GetData answers S_OK
        // only once the newest has completed (Learn GetData: S_OK "indicates
        // that the data at pData is available").
        for (const auto& interval : query.occlusion_intervals) {
            if (!interval) continue;
            query.last_work_generation = std::max(
                query.last_work_generation, interval->recorded_generation());
        }
    }
    query.gpu_point.reset();
    const u64 end_generation = m_backend.recording_generation();
    if (type == D3D11_QUERY_TIMESTAMP ||
        type == D3D11_QUERY_TIMESTAMP_DISJOINT ||
        type == D3D11_QUERY_EVENT) {
        // Learn (D3D11_QUERY): EVENT "determines whether or not the GPU is
        // finished processing commands"; TIMESTAMP is "a 64-bit value that
        // increments ... at the rate reported by TIMESTAMP_DISJOINT", taken
        // when the GPU reaches the End. Both are answered by a point
        // recorded in the command stream here.
        // VITREO_D3D11_HOST_TIMESTAMPS=1: host clock at End, no command
        // buffer split (bisection knob while a GPU hang is isolated).
        static const bool host_timestamps =
            std::getenv("VITREO_D3D11_HOST_TIMESTAMPS") != nullptr;
        query.gpu_point =
            host_timestamps ? nullptr : m_backend.record_timestamp();
        if (!query.gpu_point) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!End: the "
                             "graphics backend cannot time GPU work; "
                             "EVENT/TIMESTAMP queries report host time "
                             "at End\n");
            });
            using clock = std::chrono::steady_clock;
            query.host_ticks_at_end = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock::now().time_since_epoch())
                    .count());
        }
    } else if (query.untracked_work) {
        // Work the backend could not place in a submission generation (a
        // backend without generations): wait for a point after it instead.
        query.gpu_point = m_backend.record_timestamp();
        if (!query.gpu_point) (void)m_backend.wait_idle();
    }
    query.point_generation = query.gpu_point ? end_generation : 0;
    query.state = Query::State::Issued;
    d3d11_diag("ID3D11DeviceContext::End query=0x%llx type=%u",
               static_cast<unsigned long long>(async), type);
}

// -----------------------------------------------------------------------------
// GetData
// -----------------------------------------------------------------------------

bool D3D11Manager::query_result_ready(const Query& query) {
    if (query.gpu_point && !query.gpu_point->ready()) return false;
    return query.last_work_generation == 0 ||
           m_backend.generation_complete(query.last_work_generation);
}

// Learn (D3D11_ASYNC_GETDATA_FLAG): the flush is what lets a query polled in
// a loop complete -- DONOTFLUSH "can potentially cause an infinite loop"
// because the commands the query waits on are never sent. It is needed only
// while the query's point, or work its range counted, is still in the open
// recording; once submitted, the query completes without it. Flushing anyway
// cut one submission per poll, and UE polls its queries between draws: every
// submission past two in flight waits for the GPU, so the RHI thread ran in
// lockstep with a GPU that was idle three quarters of the time.
bool D3D11Manager::query_needs_flush(const Query& query) {
    const u64 recording = m_backend.recording_generation();
    // 0: a thread capturing a command list, or a backend without
    // generations. Nothing says the work was sent, so send it.
    if (recording == 0) return true;
    u64 pending = query.last_work_generation;
    if (query.gpu_point) {
        if (query.point_generation == 0) return true;
        pending = std::max(pending, query.point_generation);
    }
    return pending >= recording;
}

u32 D3D11Manager::query_get_data(GuestAddr context, GuestAddr async,
                                 GuestAddr data, u32 data_size, u32 flags) {
    const auto found = m_queries.find(async);
    const auto owner = m_interfaces.find(context);
    if (found == m_queries.end() || owner == m_interfaces.end() ||
        found->second.device_owner != owner->second.owner) {
        d3d11_diag("ID3D11DeviceContext::GetData query=0x%llx -> "
                   "E_INVALIDARG",
                   static_cast<unsigned long long>(async));
        return d3d11::E_INVALIDARG;
    }
    Query& query = found->second;
    const u32 type = query.query_type;
    // Arguments are checked before the state (measured on a never-issued
    // query: bad flags, a wrong size and (NULL, size) are E_INVALIDARG, not
    // DXGI_ERROR_INVALID_CALL).
    if ((flags & ~D3D11_ASYNC_GETDATA_DONOTFLUSH) != 0) {
        return d3d11::E_INVALIDARG;
    }
    // Learn (D3D11_QUERY_MISC_PREDICATEHINT): "Predication data cannot be
    // returned to your application via GetData" -- measured: every call,
    // status polls included, is E_INVALIDARG.
    if ((query.misc_flags & D3D11_QUERY_MISC_PREDICATEHINT) != 0) {
        return d3d11::E_INVALIDARG;
    }
    // Learn: DataSize "Must be 0 when pData is NULL"; any size other than
    // the type's exact one is refused (measured: size-1, size+1, size*2).
    if (data_size != 0 && (data == 0 || data_size != query_data_size(type))) {
        return d3d11::E_INVALIDARG;
    }
    if (query.state != Query::State::Issued) {
        d3d11_diag("ID3D11DeviceContext::GetData query=0x%llx type=%u %s -> "
                   "DXGI_ERROR_INVALID_CALL",
                   static_cast<unsigned long long>(async), type,
                   query.state == Query::State::Building ? "building"
                                                         : "never issued");
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!query_result_ready(query)) {
        // Learn (D3D11_ASYNC_GETDATA_FLAG): without DONOTFLUSH the call
        // flushes the command buffer, so a query polled in a loop completes.
        if ((flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) == 0 &&
            query_needs_flush(query)) {
            m_backend.flush();
        }
        if (!query_result_ready(query)) {
            if (query_wait_diag_enabled()) note_query_poll(async, type, false);
            return 1u;  // S_FALSE
        }
    }
    if (query_wait_diag_enabled()) note_query_poll(async, type, true);
    if (type == D3D11_QUERY_PIPELINE_STATISTICS) {
        // Fold in the indirect dispatches' argument blocks, now that the GPU
        // has run the copies that captured them.
        for (const auto& pending : query.pending_arguments) {
            std::array<u32, 3> words{};
            if (!pending.arguments ||
                !pending.arguments->read(0, words.data(), sizeof(words))) {
                query.pipeline_unmeasured |=
                    pipeline_statistic_bit(PipelineStatistic::CSInvocations);
                continue;
            }
            query.pipeline[static_cast<usize>(
                PipelineStatistic::CSInvocations)] +=
                static_cast<u64>(words[0]) * words[1] * words[2] *
                pending.threads_per_group;
        }
        query.pending_arguments.clear();
        // And the GPU-counted records of the intervals the bracket spans.
        for (const auto& record : query.statistics_records) {
            std::array<u32, graphics::STATS_RECORD_WORDS> words{};
            if (!record || !record->read(0, words.data(), sizeof(words))) {
                query.pipeline_unmeasured |= kStatisticsAllBits;
                continue;
            }
            for (u32 c = 0; c < graphics::STATS_COUNTERS; ++c) {
                query.pipeline[c] +=
                    graphics::stats_record_value(words.data(), c);
            }
            query.pipeline_unmeasured |=
                words[graphics::STATS_FLAGS_WORD] & kStatisticsAllBits;
        }
        query.statistics_records.clear();
        if (query.pipeline_unmeasured != 0) {
            std::string names;
            for (usize i = 0; i < PIPELINE_STATISTIC_NAMES.size(); ++i) {
                if ((query.pipeline_unmeasured & (1u << i)) == 0) continue;
                if (!names.empty()) names += ", ";
                names += PIPELINE_STATISTIC_NAMES[i];
            }
            d3d11_unimplemented_once(
                "GetData", "PIPELINE_STATISTICS " + names +
                               " (a draw in the bracket could not be counted; "
                               "see the diagnostic naming it) -> E_NOTIMPL");
            return d3d11::E_NOTIMPL;
        }
    }
    if (is_occlusion_type(type) && query.occlusion_unmeasured) {
        d3d11_unimplemented_once(
            "GetData", "OCCLUSION: the backend could not count samples for "
                       "a draw in the bracket -> E_NOTIMPL");
        return d3d11::E_NOTIMPL;
    }
    // Learn (GetData): "If DataSize is 0, GetData is only used to check
    // status" -- whatever pData is (measured).
    if (data_size == 0) return d3d11::S_OK;

    bool ok = false;
    switch (type) {
    case D3D11_QUERY_EVENT:
        ok = m_memory.write_u32(data, 1u).is_ok();
        break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
        // D3D11.3 20.4.6: samples that passed depth and stencil; 20.4.8: the
        // predicate is FALSE exactly when that count would be 0.
        const u64 samples = occlusion_samples(query);
        ok = type == D3D11_QUERY_OCCLUSION
                 ? m_memory.write_u64(data, samples).is_ok()
                 : m_memory.write_u32(data, samples != 0 ? 1u : 0u).is_ok();
        if (ok && d3d11_skip_stats_enabled()) {
            note_occlusion_answer(type, samples);
        }
        break;
    }
    case D3D11_QUERY_TIMESTAMP: {
        const u64 ticks = query.gpu_point ? query.gpu_point->gpu_time_ns()
                                          : query.host_ticks_at_end;
        ok = m_memory.write_u64(data, ticks).is_ok();
        break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
        // D3D11_QUERY_DATA_TIMESTAMP_DISJOINT: Frequency + Disjoint.
        ok = m_memory.write_u64(data, kTimestampFrequencyHz).is_ok() &&
             m_memory.write_u32(data + 8, 0).is_ok();
        break;
    case D3D11_QUERY_PIPELINE_STATISTICS:
        ok = m_memory
                 .write_bytes(data,
                              reinterpret_cast<const Byte*>(
                                  query.pipeline.data()),
                              sizeof(query.pipeline))
                 .is_ok();
        break;
    default: {
        // SO_STATISTICS* (two UINT64s) and SO_OVERFLOW_PREDICATE* (a BOOL,
        // TRUE when primitives did not fit: D3D11.3 20.4.10).
        u64 written = 0;
        u64 needed = 0;
        bool overflowed = false;
        if (!stream_out_counts(query, written, needed, overflowed)) {
            d3d11_unimplemented_once(
                "GetData", "SO_STATISTICS / SO_OVERFLOW_PREDICATE: the "
                           "backend cannot read the GPU's stream-output "
                           "counts back -> E_NOTIMPL");
            return d3d11::E_NOTIMPL;
        }
        ok = query_type_is_predicate(type)
                 ? m_memory.write_u32(data, overflowed ? 1u : 0u).is_ok()
                 : m_memory.write_u64(data, written).is_ok() &&
                       m_memory.write_u64(data + 8, needed).is_ok();
        break;
    }
    }
    return ok ? d3d11::S_OK : d3d11::E_INVALIDARG;
}

// -----------------------------------------------------------------------------
// Counting draws and dispatches
// -----------------------------------------------------------------------------

graphics::GpuOcclusionCounter* D3D11Manager::occlusion_counter_for_draw(
    GuestAddr context) {
    auto found = m_context_states.find(context);
    if (found == m_context_states.end()) return nullptr;
    auto& state = found->second;
    bool any = false;
    for (const GuestAddr address : state.counting_queries) {
        const auto query = m_queries.find(address);
        any |= query != m_queries.end() &&
               is_occlusion_type(query->second.query_type);
    }
    if (!any) return nullptr;
    const bool opened = !state.occlusion_interval;
    if (opened) state.occlusion_interval = m_backend.create_occlusion_counter();
    // Only whether the draw's pass can be placed in a generation at all (0:
    // this thread captures a command list, or the backend has none). The
    // generation itself is the counter's, which the backend takes from the
    // recording the pass went into (recorded_generation, folded in at End):
    // one read here, before the pass exists, can name a recording another
    // thread submits before the pass is recorded -- S_OK one generation
    // early, with a count the GPU has not written.
    const bool tracked = m_backend.recording_generation() != 0;
    for (const GuestAddr address : state.counting_queries) {
        const auto query = m_queries.find(address);
        if (query == m_queries.end() ||
            !is_occlusion_type(query->second.query_type)) {
            continue;
        }
        if (!state.occlusion_interval) {
            query->second.occlusion_unmeasured = true;
            continue;
        }
        if (opened) {
            query->second.occlusion_intervals.push_back(
                state.occlusion_interval);
        }
        if (!tracked) note_work(query->second, 0);
    }
    return state.occlusion_interval.get();
}

graphics::StatsClipLayout stats_clip_layout(
    const std::vector<graphics::DxbcSignatureElement>& outputs,
    const std::vector<u32>& record_registers, u32 stream) {
    graphics::StatsClipLayout layout{};
    const auto slot_of = [&](u32 reg, u32& slot) {
        const auto found =
            std::find(record_registers.begin(), record_registers.end(), reg);
        if (found == record_registers.end()) return false;
        slot = static_cast<u32>(found - record_registers.begin());
        return true;
    };
    // SV_ClipDistance components in clip-array order: by semantic index,
    // then component (the rasterizer's order, D3D11.3 15.4.1).
    std::vector<std::tuple<u32, u32, u32>> distances;
    for (const auto& element : outputs) {
        if (element.stream != stream) continue;
        if (element.system_value_type == 1u) {  // D3D_NAME_POSITION
            layout.valid = slot_of(element.register_num, layout.position_slot);
        } else if (element.system_value_type == 2u) {  // CLIP_DISTANCE
            for (u32 c = 0; c < 4; ++c) {
                if ((element.component_mask & (1u << c)) != 0u) {
                    distances.emplace_back(element.semantic_index,
                                           element.register_num, c);
                }
            }
        }
    }
    std::sort(distances.begin(), distances.end());
    for (const auto& [index, reg, component] : distances) {
        u32 slot = 0;
        if (layout.distances >= 8 || !slot_of(reg, slot)) {
            layout.valid = false;
            break;
        }
        layout.distance_slot[layout.distances] = slot;
        layout.distance_component[layout.distances] = component;
        ++layout.distances;
    }
    return layout;
}

bool D3D11Manager::pipeline_statistics_building(GuestAddr context) const {
    const auto found = m_context_states.find(context);
    if (found == m_context_states.end()) return false;
    for (const GuestAddr address : found->second.counting_queries) {
        const auto query = m_queries.find(address);
        if (query != m_queries.end() &&
            query->second.query_type == D3D11_QUERY_PIPELINE_STATISTICS) {
            return true;
        }
    }
    return false;
}

void D3D11Manager::mark_statistics_unmeasured(GuestAddr context, u32 bits,
                                              const char* why) {
    const auto found = m_context_states.find(context);
    if (found == m_context_states.end() || bits == 0) return;
    for (const GuestAddr address : found->second.counting_queries) {
        const auto query = m_queries.find(address);
        if (query != m_queries.end() &&
            query->second.query_type == D3D11_QUERY_PIPELINE_STATISTICS) {
            query->second.pipeline_unmeasured |= bits;
        }
    }
    d3d11_unimplemented_once("GetData",
                             std::string("PIPELINE_STATISTICS: ") + why);
}

std::shared_ptr<graphics::BackendBuffer>
D3D11Manager::statistics_record_for_draw(GuestAddr context) {
    if (!pipeline_statistics_building(context)) return nullptr;
    auto& state = m_context_states[context];
    const bool opened = !state.statistics_interval;
    if (opened) {
        graphics::BackendBufferDesc desc{};
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.byte_width = graphics::STATS_RECORD_WORDS * sizeof(u32);
        const std::array<u32, graphics::STATS_RECORD_WORDS> zeros{};
        state.statistics_interval = m_backend.create_buffer(desc, zeros.data());
        if (state.statistics_interval && !state.statistics_interval->valid()) {
            state.statistics_interval.reset();
        }
    }
    if (!state.statistics_interval) {
        mark_statistics_unmeasured(
            context, kStatisticsAllBits,
            "the backend could not allocate a statistics record");
        return nullptr;
    }
    const u64 generation = m_backend.recording_generation();
    for (const GuestAddr address : state.counting_queries) {
        const auto query = m_queries.find(address);
        if (query == m_queries.end() ||
            query->second.query_type != D3D11_QUERY_PIPELINE_STATISTICS) {
            continue;
        }
        if (opened) {
            query->second.statistics_records.push_back(
                state.statistics_interval);
        }
        note_work(query->second, generation);
    }
    return state.statistics_interval;
}

/// The lines, in d3d11_skip_stat's format (n = draws in the last 5 s):
///   [d3d11-skip] n=<count> fmt=Draw recovered: the encoder reopened its pass ...
///   [d3d11-skip] n=<count> fmt=Draw skipped by the encoder: its pass was not open
/// A draw is reopened when another thread's submit (or recorded op) came
/// between its pass's begin and the draw; dropped when no pass with its id
/// was open at all. The backend counts from process start whether or not the
/// switch is on, so the first report after it is turned on gives the totals
/// so far ("... (since process start)"): a drop at startup -- a LUT baked
/// once -- is not lost for want of the marker. Changes nothing; with the
/// switch off it is never called.
void D3D11Manager::note_draw_encode_counts() {
    static std::mutex mu;
    static graphics::BackendDrawEncodeCounts reported{};
    static bool primed = false;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (primed && now - last < std::chrono::seconds(5)) return;
    const graphics::BackendDrawEncodeCounts counts =
        m_backend.draw_encode_counts();
    const char* const since = primed ? "" : " (since process start)";
    const u64 reopened = counts.reopened - reported.reopened;
    const u64 dropped = counts.dropped - reported.dropped;
    if (reopened != 0) {
        std::fprintf(stderr,
                     "[d3d11-skip] n=%llu fmt=Draw recovered: the encoder "
                     "reopened its pass (another thread submitted or recorded "
                     "between the pass's begin and the draw)%s\n",
                     static_cast<unsigned long long>(reopened), since);
    }
    if (dropped != 0) {
        std::fprintf(stderr,
                     "[d3d11-skip] n=%llu fmt=Draw skipped by the encoder: its "
                     "pass was not open%s\n",
                     static_cast<unsigned long long>(dropped), since);
    }
    primed = true;
    reported = counts;
    last = now;
    // The frontend's own skip lines, from the same draw-paced hook: a skip
    // that happened only before the switch was turned on (a bake-once draw)
    // has no later skip event to print it.
    d3d11_skip_stat_report();
}

u64 D3D11Manager::recorded_work_generation() {
    const u64 recorded = m_backend.last_recorded_generation();
    return recorded != 0 ? recorded : m_backend.recording_generation();
}

void D3D11Manager::note_statistics_recorded(GuestAddr context) {
    const auto found = m_context_states.find(context);
    if (found == m_context_states.end() || !found->second.statistics_interval) {
        return;
    }
    const u64 generation = recorded_work_generation();
    for (const GuestAddr address : found->second.counting_queries) {
        const auto query = m_queries.find(address);
        if (query != m_queries.end() &&
            query->second.query_type == D3D11_QUERY_PIPELINE_STATISTICS) {
            note_work(query->second, generation);
        }
    }
}

graphics::BackendComputePipeline* D3D11Manager::statistics_kernel(
    const char* entry_point) {
    auto& slot = m_stats_kernels[entry_point];
    if (!slot) {
        static const std::string msl = graphics::pipeline_stats_msl();
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = msl;
        desc.compute_entry_point = entry_point;
        desc.cache_key = std::hash<std::string>{}(msl) ^
                         std::hash<std::string>{}(entry_point);
        slot = m_backend.create_compute_pipeline(desc);
    }
    return slot && slot->valid() ? slot.get() : nullptr;
}

bool D3D11Manager::rasterization_disabled(const ContextState& state) const {
    if (state.pixel_shader != 0) return false;
    // The default depth state (none bound) has DepthEnable TRUE.
    bool depth = true;
    bool stencil = false;
    if (state.depth_stencil_state != 0) {
        const auto dss = m_depth_stencil_states.find(state.depth_stencil_state);
        if (dss != m_depth_stencil_states.end()) {
            depth = dss->second.desc.depth_enable != 0;
            stencil = dss->second.desc.stencil_enable != 0;
        }
    }
    return !depth && !stencil;
}

bool D3D11Manager::depth_clip_enabled(const ContextState& state) const {
    if (state.rasterizer_state == 0) return true;  // default DepthClipEnable
    const auto rs = m_rasterizer_states.find(state.rasterizer_state);
    return rs == m_rasterizer_states.end() ||
           rs->second.desc.depth_clip_enable != 0;
}

void D3D11Manager::count_pipeline_draw(
    GuestAddr context, bool indexed, u32 count, u32 start_location,
    i32 base_vertex, u32 instance_count,
    const graphics::BackendBuffer* indirect_arguments, u32 indirect_offset,
    StatisticsDraw& plan) {
    plan = StatisticsDraw{};
    if (!pipeline_statistics_building(context)) return;
    auto& state = m_context_states[context];
    if (state.vertex_shader == 0) return;
    const u32 topology = state.primitive_topology;
    const u32 vpp = ia_vertices_per_primitive(topology);
    if (vpp == 0) return;  // nothing is drawn (Windows draws nothing)
    const u32 assembly = ia_assembly(topology);
    const bool tessellated = state.hull_shader != 0 && state.domain_shader != 0;
    const GeometryShader* gs = nullptr;
    if (state.geometry_shader != 0) {
        const auto found = m_geometry_shaders.find(state.geometry_shader);
        if (found != m_geometry_shaders.end()) gs = &found->second;
    }
    const bool geometry_program = gs != nullptr && gs->program;
    // The input assembler's primitives reach the clipper directly without a
    // geometry or tessellation stage -- and are rasterized unless 15.2
    // disables it or a stream-output object rasterizes no stream.
    const bool rasterized_directly =
        !tessellated && !geometry_program && !rasterization_disabled(state) &&
        (gs == nullptr || gs->rasterized_stream == 0);
    const bool indirect = indirect_arguments != nullptr;

    // The CPU's share: what a direct draw decides by itself -- from its own
    // vertices, or from the index buffer's CPU copy when that is true to the
    // GPU's. Anything else is assembled on the GPU below.
    bool assembled_on_cpu = false;
    u64 primitives = 0;
    u64 shaded = 0;
    u32 stride = 0;
    std::vector<u32> keys;
    if (!indirect) {
        if (!indexed) {
            bool known = true;
            primitives = primitives_in(topology, count, known);
            if (assembly == 2) {
                std::vector<u32> vertices(count);
                for (u32 i = 0; i < count; ++i) vertices[i] = i;
                keys = graphics::assemble_primitive_vertices(
                    vertices, false, 0, 0, vpp, primitive_assembly(assembly));
                shaded = graphics::stats_vertex_invocations(keys, vpp);
                keys.clear();
            } else {
                shaded = graphics::stats_vertex_invocations_non_indexed(
                    primitives, vpp, assembly == 1);
            }
            stride = count;
            assembled_on_cpu = true;
        } else {
            // The index range's CPU copy, when it is true to the GPU's: not
            // while a GPU write is newer, and never for a buffer the GPU can
            // write itself (as execute_draw's index rewrite).
            const auto ib = m_buffers.find(state.index_buffer);
            const bool wide = state.index_buffer_format == 42;  // R32_UINT
            const u32 index_size = wide ? 4u : 2u;
            constexpr u32 kGpuWritableBinds = D3D11_BIND_STREAM_OUTPUT |
                                              D3D11_BIND_UNORDERED_ACCESS;
            const u64 first =
                state.index_buffer_offset / index_size + start_location;
            if (ib != m_buffers.end() && !ib->second.gpu_newer &&
                (ib->second.desc.bind_flags & kGpuWritableBinds) == 0 &&
                (first + count) * index_size <= ib->second.shadow.size()) {
                std::vector<u32> indices(count);
                u32 max_index = 0;
                const u32 cut = wide ? 0xFFFFFFFFu : 0xFFFFu;
                bool any = false;
                for (u32 i = 0; i < count; ++i) {
                    u32 value = 0;
                    std::memcpy(&value,
                                ib->second.shadow.data() +
                                    (first + i) * index_size,
                                index_size);
                    indices[i] = value;
                    if (value != cut) {
                        max_index = std::max(max_index, value);
                        any = true;
                    }
                }
                keys = graphics::assemble_primitive_vertices(
                    indices, true, cut, 0, vpp, primitive_assembly(assembly));
                primitives = keys.size() / vpp;
                shaded = graphics::stats_vertex_invocations(keys, vpp);
                stride = any ? max_index + 1u : 0u;
                assembled_on_cpu = true;
            }
        }
    }
    const u64 instances = indirect ? 0u : instance_count;
    for (const GuestAddr address : state.counting_queries) {
        const auto query = m_queries.find(address);
        if (query == m_queries.end() ||
            query->second.query_type != D3D11_QUERY_PIPELINE_STATISTICS) {
            continue;
        }
        auto& stats = query->second.pipeline;
        const auto add = [&](PipelineStatistic statistic, u64 value) {
            stats[static_cast<usize>(statistic)] += value;
        };
        // IAVertices: "not subtracting any caching" (D3D11.3 20.4.7).
        if (!indirect) add(PipelineStatistic::IAVertices, u64{count} * instances);
        if (!assembled_on_cpu) continue;
        add(PipelineStatistic::IAPrimitives, primitives * instances);
        add(PipelineStatistic::VSInvocations, shaded * instances);
        // "HSInvocations increments once per patch".
        if (tessellated) add(PipelineStatistic::HSInvocations, primitives * instances);
        // "The clipper will execute for each triangle" (culled and
        // degenerate ones too, measured).
        if (rasterized_directly) {
            add(PipelineStatistic::CInvocations, primitives * instances);
        }
    }

    const bool needs_record = !assembled_on_cpu || rasterized_directly;
    if (!needs_record) return;
    const auto record = statistics_record_for_draw(context);
    if (!record) return;

    // The rasterized draw's capture: its vertex shader's outputs, keyed
    // draw-relative (SV_VertexID's value + instance x stride).
    const VertexShader* capturing = nullptr;
    if (rasterized_directly) {
        const auto vs = m_vertex_shaders.find(state.vertex_shader);
        if (vs != m_vertex_shaders.end() && ensure_vertex_capture(vs->second) &&
            vs->second.stats_clip.valid) {
            capturing = &vs->second;
        } else {
            mark_statistics_unmeasured(
                context, pipeline_statistic_bit(PipelineStatistic::CPrimitives),
                "CPrimitives of a vertex shader whose outputs cannot be "
                "captured (descriptor tables, an early return, or no "
                "SV_Position)");
        }
    }
    const u32 record_vec4s =
        capturing != nullptr
            ? static_cast<u32>(capturing->capture_registers.size())
            : 0u;
    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    graphics::StatsKernelParams clip{};
    clip.vertices_per_primitive = vpp;
    set_raster_pick(topology, clip);
    clip.record_vec4s = record_vec4s;
    clip.record_base = 1;  // the {stride, capacity} header
    if (capturing != nullptr) set_clip_layout(capturing->stats_clip, clip);
    clip.flags = graphics::STATS_FLAG_CLIP |
                 (depth_clip_enabled(state) ? graphics::STATS_FLAG_DEPTH_CLIP
                                            : 0u);

    if (assembled_on_cpu) {
        // The CPU assembled instance 0: the kernel shifts each instance's
        // keys by the stride, and a non-indexed draw's keys are formulas.
        if (capturing == nullptr) return;
        const u64 capacity = u64{stride} * instances;
        const u64 capture_bytes =
            graphics::CAPTURE_HEADER_BYTES + capacity * record_vec4s * 16u;
        if (capture_bytes > kMaxStatisticsBytes ||
            primitives * instances > 0xFFFFFFFFull) {
            mark_statistics_unmeasured(
                context, pipeline_statistic_bit(PipelineStatistic::CPrimitives),
                "CPrimitives of a draw whose captured vertices exceed the "
                "size limit");
            return;
        }
        std::vector<u32> header(graphics::CAPTURE_HEADER_BYTES / 4u, 0u);
        header[0] = stride;
        header[1] = static_cast<u32>(capacity);
        storage.byte_width = static_cast<usize>(capture_bytes);
        plan.capture = m_backend.create_buffer(storage, nullptr);
        if (plan.capture &&
            !plan.capture->update(0, header.data(), header.size() * 4u)) {
            plan.capture.reset();
        }
        if (indexed && !keys.empty()) {
            graphics::BackendBufferDesc ids_desc{};
            ids_desc.shader_resource = true;
            ids_desc.byte_width = keys.size() * sizeof(u32);
            plan.ids = m_backend.create_buffer(ids_desc, keys.data());
        }
        clip.key_mode = indexed ? 1u : assembly == 0 ? 2u : assembly == 1 ? 3u : 4u;
        clip.primitives_per_instance = static_cast<u32>(primitives);
        clip.instances = static_cast<u32>(instances);
        clip.key_stride = stride;
        clip.capacity = static_cast<u32>(primitives * instances);
        if (!plan.capture || (indexed && !keys.empty() && !plan.ids)) {
            mark_statistics_unmeasured(
                context, pipeline_statistic_bit(PipelineStatistic::CPrimitives),
                "CPrimitives: capture buffers could not be allocated");
            return;
        }
        if (clip.capacity == 0) return;  // no complete primitive
        plan.record = record;
        plan.clip = clip;
        plan.active = true;
        return;
    }

    // The GPU assembles the draw (an indirect one, or a GPU-written index
    // buffer): vitreo_ia_resolve, then vitreo_stats_vertices. The resolve
    // takes no record capacity, so the input-assembler counts never depend
    // on the capture's; the capture's own capacity is then placed in its
    // header, and the clipper reports a key past it unmeasured.
    poll_indirect_feedback();
    bool known = true;
    const u64 direct_primitives =
        indirect ? 0u : primitives_in(topology, count, known);
    const u64 wanted_records =
        (indexed && state.index_buffer_format != 42 ? 65536u
                                                    : m_indirect_record_capacity) *
        (indirect ? 1u : std::max<u64>(instances, 1u));
    const u32 record_capacity =
        capturing == nullptr
            ? 0u
            : static_cast<u32>(std::min<u64>(
                  wanted_records,
                  kMaxStatisticsBytes /
                      std::max<u64>(u64{record_vec4s} * 16u, 16u)));
    const u64 primitive_capacity = std::min<u64>(
        indirect ? u64{m_indirect_record_capacity} * 4u
                 : direct_primitives * instances,
        kMaxStatisticsBytes / (4u * vpp));
    if (primitive_capacity == 0) return;
    graphics::BackendComputePipeline* resolve_kernel = nullptr;
    if (!m_ia_resolve) {
        graphics::BackendComputePipelineDesc pd{};
        pd.compute_entry_point = graphics::kIaResolveEntryPoint;
        pd.compute_shader_msl = graphics::ia_resolve_msl();
        pd.cache_key = 0x1A7E501Full;
        m_ia_resolve = m_backend.create_compute_pipeline(pd);
    }
    if (m_ia_resolve && m_ia_resolve->valid()) resolve_kernel = m_ia_resolve.get();
    graphics::BackendComputePipeline* vertices_kernel =
        statistics_kernel(graphics::kStatsVerticesEntryPoint);
    storage.byte_width = graphics::IA_RESOLVED_WORDS * sizeof(u32);
    plan.resolved = m_backend.create_buffer(storage, nullptr);
    storage.byte_width = static_cast<usize>(primitive_capacity * vpp * 4u);
    plan.ids = m_backend.create_buffer(storage, nullptr);
    storage.byte_width =
        graphics::CAPTURE_HEADER_BYTES +
        (capturing != nullptr ? u64{record_capacity} * record_vec4s * 16u : 0u);
    plan.capture = m_backend.create_buffer(storage, nullptr);
    std::shared_ptr<graphics::BackendBuffer> host_arguments;
    if (!indirect) {
        // D3D11_DRAW_[INDEXED_]INSTANCED_INDIRECT_ARGS of the direct draw.
        const std::array<u32, 5> words{
            count, instance_count, start_location,
            indexed ? static_cast<u32>(base_vertex) : 0u, 0u};
        storage.byte_width = sizeof(words);
        host_arguments = m_backend.create_buffer(storage, words.data());
    }
    const auto ib = indexed ? m_buffers.find(state.index_buffer) : m_buffers.end();
    if (resolve_kernel == nullptr || vertices_kernel == nullptr ||
        !plan.resolved || !plan.ids || !plan.capture ||
        (!indirect && !host_arguments) ||
        (indexed && (ib == m_buffers.end() || !ib->second.backend))) {
        plan = StatisticsDraw{};
        mark_statistics_unmeasured(
            context, kStatisticsIaBits,
            "the input-assembler statistics of a GPU-assembled draw could not "
            "be recorded");
        return;
    }
    graphics::IaResolveParams params{};
    params.indexed = indexed ? 1u : 0u;
    params.vertices_per_primitive = vpp;
    params.assembly = assembly;
    params.record_capacity = 0xFFFFFFFFu;
    params.primitive_capacity = static_cast<u32>(primitive_capacity);
    if (indexed) {
        const bool wide = state.index_buffer_format == 42;  // R32_UINT
        const u32 index_size = wide ? 4u : 2u;
        params.index_32bit = wide ? 1u : 0u;
        params.index_first = state.index_buffer_offset / index_size;
        const u32 bytes = ib->second.desc.byte_width;
        params.index_elements =
            bytes > state.index_buffer_offset
                ? (bytes - state.index_buffer_offset) / index_size
                : 0u;
    }
    const graphics::BackendBuffer* args =
        indirect ? indirect_arguments : host_arguments.get();
    const u32 args_offset = indirect ? indirect_offset : 0u;
    graphics::BackendDispatchCall resolve{};
    resolve.pipeline = resolve_kernel;
    resolve.group_count_x = resolve.group_count_y = resolve.group_count_z = 1;
    resolve.threads_per_group_x = graphics::IA_RESOLVE_THREADS;
    resolve.constants = &params;
    resolve.constants_size = sizeof(params);
    resolve.buffers[graphics::IA_RESOLVE_ARGS_INDEX] =
        const_cast<graphics::BackendBuffer*>(args);
    resolve.buffer_offsets[graphics::IA_RESOLVE_ARGS_INDEX] = args_offset;
    resolve.buffers[graphics::IA_RESOLVE_INDICES_INDEX] =
        indexed ? ib->second.backend.get() : plan.resolved.get();
    resolve.buffers[graphics::IA_RESOLVE_IDS_INDEX] = plan.ids.get();
    resolve.buffers[graphics::IA_RESOLVE_OUT_INDEX] = plan.resolved.get();
    resolve.buffers[graphics::IA_RESOLVE_CAPTURE_INDEX] = plan.capture.get();
    graphics::StatsKernelParams vertices{};
    vertices.vertices_per_primitive = vpp;
    vertices.batch = graphics::stats_vertex_batch(vpp);
    vertices.flags = graphics::STATS_FLAG_ADD_IA_PRIMITIVES |
                     graphics::STATS_FLAG_ADD_VS_INVOCATIONS |
                     (indirect ? graphics::STATS_FLAG_ADD_IA_VERTICES : 0u) |
                     (tessellated ? graphics::STATS_FLAG_ADD_HS_INVOCATIONS : 0u) |
                     (rasterized_directly ? graphics::STATS_FLAG_ADD_C_INVOCATIONS
                                          : 0u);
    graphics::BackendDispatchCall count_call{};
    count_call.pipeline = vertices_kernel;
    count_call.threads_per_group_x = graphics::STATS_BATCH_THREADS;
    count_call.group_count_x = static_cast<u32>(
        (primitive_capacity + vertices.batch - 1) / vertices.batch);
    count_call.group_count_y = count_call.group_count_z = 1;
    count_call.constants = &vertices;
    count_call.constants_size = sizeof(vertices);
    count_call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    count_call.buffers[graphics::STATS_COUNTS_INDEX] = plan.resolved.get();
    count_call.buffers[graphics::STATS_IDS_INDEX] = plan.ids.get();
    count_call.buffers[graphics::STATS_ARGS_INDEX] =
        const_cast<graphics::BackendBuffer*>(args);
    count_call.buffer_offsets[graphics::STATS_ARGS_INDEX] = args_offset;
    std::shared_ptr<graphics::BackendBuffer> capacity_word;
    if (capturing != nullptr) {
        storage.byte_width = sizeof(u32);
        capacity_word = m_backend.create_buffer(storage, &record_capacity);
    }
    if (!m_backend.dispatch(resolve) || !m_backend.dispatch(count_call) ||
        (capturing != nullptr &&
         (!capacity_word ||
          !m_backend.copy_buffer_region(plan.capture.get(), sizeof(u32),
                                        capacity_word.get(), 0,
                                        sizeof(u32))))) {
        plan = StatisticsDraw{};
        mark_statistics_unmeasured(
            context, kStatisticsIaBits,
            "the input-assembler statistics dispatch failed");
        return;
    }
    if (indirect) {
        // A draw past the capacity raises it for the next one (the
        // primitive capacity is sized from it).
        m_indirect_feedback.push_back({plan.resolved,
                                       m_backend.recording_generation(),
                                       m_indirect_record_capacity});
    }
    note_statistics_recorded(context);  // the dispatches' generation
    if (capturing == nullptr) {
        plan = StatisticsDraw{};
        return;
    }
    clip.key_mode = 0;
    clip.flags |= graphics::STATS_FLAG_RESOLVED_COUNTS;
    clip.capacity = static_cast<u32>(primitive_capacity);
    plan.record = record;
    plan.clip = clip;
    plan.active = true;
}

void D3D11Manager::finish_statistics_draw(StatisticsDraw& plan) {
    if (!plan.active) return;
    plan.active = false;
    graphics::BackendComputePipeline* kernel =
        statistics_kernel(graphics::kStatsPrimitivesEntryPoint);
    if (kernel == nullptr) return;
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x = (plan.clip.capacity + graphics::STATS_THREADS - 1) /
                         graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &plan.clip;
    call.constants_size = sizeof(plan.clip);
    call.buffers[graphics::STATS_RECORD_INDEX] = plan.record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] =
        plan.resolved ? plan.resolved.get() : plan.record.get();
    call.buffers[graphics::STATS_IDS_INDEX] =
        plan.ids ? plan.ids.get() : plan.record.get();
    call.buffers[graphics::STATS_RECORDS_INDEX] = plan.capture.get();
    (void)m_backend.dispatch(call);
}

void D3D11Manager::count_geometry_statistics(GuestAddr context,
                                             GeometryShader& gs,
                                             u32 invocations,
                                             graphics::BackendBuffer* counter,
                                             graphics::BackendBuffer* expanded,
                                             bool rasterized) {
    const auto record = statistics_record_for_draw(context);
    if (!record || invocations == 0) return;
    graphics::BackendComputePipeline* kernel =
        statistics_kernel(graphics::kStatsGeometryEntryPoint);
    if (kernel == nullptr) {
        mark_statistics_unmeasured(
            context,
            pipeline_statistic_bit(PipelineStatistic::GSInvocations) |
                pipeline_statistic_bit(PipelineStatistic::GSPrimitives) |
                pipeline_statistic_bit(PipelineStatistic::CInvocations) |
                pipeline_statistic_bit(PipelineStatistic::CPrimitives),
            "the geometry statistics kernel is unavailable");
        return;
    }
    if (!gs.stats_clip_ready) {
        gs.stats_clip_ready = true;
        const auto container =
            graphics::parse_dxbc(gs.bytecode.data(), gs.bytecode.size());
        for (u32 stream = 0; stream < 4; ++stream) {
            gs.stats_clip[stream] = stats_clip_layout(
                container.output_signature, gs.output_record_registers,
                stream);
        }
    }
    auto& state = m_context_states[context];
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = gs.output_topology == 1   ? 1u
                                    : gs.output_topology == 3 ? 2u
                                                              : 3u;
    params.invocations = invocations;
    params.gs_instances = gs.instance_count;
    params.vertices_per_invocation = graphics::gs_vertices_per_invocation(
        gs.output_topology, gs.max_output_vertices);
    params.stream_mask = gs.stream_mask;
    params.raster_stream = gs.rasterized_stream < 4 ? gs.rasterized_stream : 4u;
    params.record_vec4s = gs.output_record_vec4s;
    if (rasterized && params.raster_stream < 4) {
        const auto& layout = gs.stats_clip[params.raster_stream];
        if (layout.valid) {
            set_clip_layout(layout, params);
            params.flags = graphics::STATS_FLAG_CLIP |
                           (depth_clip_enabled(state)
                                ? graphics::STATS_FLAG_DEPTH_CLIP
                                : 0u);
        } else {
            mark_statistics_unmeasured(
                context,
                pipeline_statistic_bit(PipelineStatistic::CInvocations) |
                    pipeline_statistic_bit(PipelineStatistic::CPrimitives),
                "CPrimitives of a geometry shader stream without SV_Position");
        }
    }
    params.capacity = invocations;
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (invocations + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = counter;
    call.buffers[graphics::STATS_RECORDS_INDEX] = expanded;
    if (!m_backend.dispatch(call)) {
        mark_statistics_unmeasured(
            context,
            pipeline_statistic_bit(PipelineStatistic::GSInvocations) |
                pipeline_statistic_bit(PipelineStatistic::GSPrimitives),
            "the geometry statistics dispatch failed");
    }
    note_statistics_recorded(context);
}

void D3D11Manager::count_tessellation_statistics(
    GuestAddr context, const DomainShader& ds, u32 vpp, u32 index_capacity,
    graphics::BackendBuffer* block, graphics::BackendBuffer* indices,
    graphics::BackendBuffer* records, bool rasterized) {
    const auto record = statistics_record_for_draw(context);
    if (!record) return;
    graphics::BackendComputePipeline* kernel =
        statistics_kernel(graphics::kStatsTessellationEntryPoint);
    if (kernel == nullptr) {
        mark_statistics_unmeasured(
            context,
            pipeline_statistic_bit(PipelineStatistic::DSInvocations) |
                pipeline_statistic_bit(PipelineStatistic::CInvocations) |
                pipeline_statistic_bit(PipelineStatistic::CPrimitives),
            "the tessellation statistics kernel is unavailable");
        return;
    }
    auto& state = m_context_states[context];
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = vpp;
    params.flags = graphics::STATS_FLAG_ADD_DS_INVOCATIONS;
    params.record_vec4s = static_cast<u32>(ds.record_registers.size());
    if (rasterized) {
        if (ds.stats_clip.valid) {
            set_clip_layout(ds.stats_clip, params);
            params.flags |= graphics::STATS_FLAG_CLIP |
                            (depth_clip_enabled(state)
                                 ? graphics::STATS_FLAG_DEPTH_CLIP
                                 : 0u);
        } else {
            mark_statistics_unmeasured(
                context,
                pipeline_statistic_bit(PipelineStatistic::CInvocations) |
                    pipeline_statistic_bit(PipelineStatistic::CPrimitives),
                "CPrimitives of a domain shader without SV_Position");
        }
    }
    params.capacity = std::max(index_capacity / std::max(vpp, 1u), 1u);
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (params.capacity + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = block;
    call.buffers[graphics::STATS_IDS_INDEX] = indices;
    call.buffers[graphics::STATS_RECORDS_INDEX] = records;
    if (!m_backend.dispatch(call)) {
        mark_statistics_unmeasured(
            context, pipeline_statistic_bit(PipelineStatistic::DSInvocations),
            "the tessellation statistics dispatch failed");
    }
    note_statistics_recorded(context);
}

void D3D11Manager::count_pipeline_dispatch(
    GuestAddr context, u64 groups, u64 threads_per_group,
    const graphics::BackendBuffer* indirect_arguments, u32 indirect_offset) {
    auto found = m_context_states.find(context);
    if (found == m_context_states.end()) return;
    auto& state = found->second;
    std::shared_ptr<graphics::BackendBuffer> snapshot;
    bool snapshot_tried = false;
    // The snapshot copy's generation, read once it is recorded.
    u64 generation = 0;
    for (const GuestAddr address : state.counting_queries) {
        const auto query = m_queries.find(address);
        if (query == m_queries.end() ||
            query->second.query_type != D3D11_QUERY_PIPELINE_STATISTICS) {
            continue;
        }
        Query& q = query->second;
        if (indirect_arguments == nullptr) {
            // CSInvocations: "Number of individual Compute Shader threads
            // invoked" (D3D11.3 20.4.7).
            q.pipeline[static_cast<usize>(PipelineStatistic::CSInvocations)] +=
                groups * threads_per_group;
            continue;
        }
        if (!snapshot_tried) {
            snapshot_tried = true;
            graphics::BackendBufferDesc desc{};
            desc.byte_width = 12;
            snapshot = m_backend.create_buffer(desc, nullptr);
            if (snapshot && (!snapshot->valid() ||
                             !m_backend.copy_buffer_region(
                                 snapshot.get(), 0, indirect_arguments,
                                 indirect_offset, 12))) {
                snapshot.reset();
            }
            if (snapshot) generation = recorded_work_generation();
        }
        if (!snapshot) {
            q.pipeline_unmeasured |=
                pipeline_statistic_bit(PipelineStatistic::CSInvocations);
            continue;
        }
        q.pending_arguments.push_back({snapshot, threads_per_group});
        note_work(q, generation);
    }
}

// -----------------------------------------------------------------------------
// Stream-output statistics
// -----------------------------------------------------------------------------

bool D3D11Manager::stream_out_queries_building(GuestAddr context) const {
    const auto found = m_context_states.find(context);
    if (found == m_context_states.end()) return false;
    for (const GuestAddr address : found->second.counting_queries) {
        const auto query = m_queries.find(address);
        if (query != m_queries.end() &&
            stream_out_query_stream(query->second.query_type) != -2) {
            return true;
        }
    }
    return false;
}

void D3D11Manager::note_stream_out_stats(
    GuestAddr context, const std::shared_ptr<graphics::BackendBuffer>& stats) {
    const auto found = m_context_states.find(context);
    if (found == m_context_states.end() || !stats) return;
    // Called once the passes writing @p stats are recorded.
    const u64 generation = recorded_work_generation();
    for (const GuestAddr address : found->second.counting_queries) {
        const auto query = m_queries.find(address);
        if (query == m_queries.end() ||
            stream_out_query_stream(query->second.query_type) == -2) {
            continue;
        }
        query->second.so_stats.push_back(stats);
        note_work(query->second, generation);
    }
}

// -----------------------------------------------------------------------------
// Predication
// -----------------------------------------------------------------------------

bool D3D11Manager::predicated_off(GuestAddr context) {
    const auto state = m_context_states.find(context);
    if (state == m_context_states.end() || state->second.predicate == 0) {
        return false;
    }
    const auto found = m_queries.find(state->second.predicate);
    if (found == m_queries.end()) return false;
    Query& predicate = found->second;
    const bool hint =
        (predicate.misc_flags & D3D11_QUERY_MISC_PREDICATEHINT) != 0;
    // A predicate with no data -- never issued, or building -- reads FALSE
    // (measured, d3d11_a10_query_rules_probe [predicate]).
    bool value = false;
    if (predicate.state == Query::State::Issued) {
        if (predicate.query_type == D3D11_QUERY_OCCLUSION_PREDICATE) {
            if (predicate.occlusion_unmeasured) {
                d3d11_unimplemented_once(
                    "SetPredication",
                    "an occlusion predicate whose samples could not be "
                    "counted; the operation runs");
                return false;
            }
            if (!query_result_ready(predicate)) {
                if (hint) return false;  // "draw it anyway" when unsure
                // Guaranteed predication: wait for the GPU to finish the
                // predicate's draws (see the file comment).
                if (predicate.last_work_generation != 0) {
                    (void)m_backend.wait_generation(
                        predicate.last_work_generation);
                }
                if (predicate.gpu_point && !predicate.gpu_point->ready()) {
                    (void)m_backend.wait_idle();
                }
                if (!query_result_ready(predicate)) {
                    d3d11_unimplemented_once(
                        "SetPredication",
                        "the occlusion predicate's GPU result never became "
                        "available; the operation runs");
                    return false;
                }
            }
            value = occlusion_samples(predicate) != 0;
        } else {
            // An SO overflow predicate cannot be a hint (CreatePredicate
            // refuses it), so it is guaranteed: wait for the GPU counts of
            // its bracket, as for an occlusion predicate above.
            if (!query_result_ready(predicate)) {
                if (predicate.last_work_generation != 0) {
                    (void)m_backend.wait_generation(
                        predicate.last_work_generation);
                }
                if (predicate.gpu_point && !predicate.gpu_point->ready()) {
                    (void)m_backend.wait_idle();
                }
            }
            u64 written = 0;
            u64 needed = 0;
            bool overflowed = false;
            if (!query_result_ready(predicate) ||
                !stream_out_counts(predicate, written, needed, overflowed)) {
                d3d11_unimplemented_once(
                    "SetPredication",
                    "the stream-output overflow predicate's GPU counts are "
                    "unavailable; the operation runs");
                return false;
            }
            value = overflowed;
        }
    }
    // Learn: skipped "if the resulting predicate data of the predicate is
    // equal to the PredicateValue".
    return value == (state->second.predicate_value != 0);
}

}  // namespace vitreo::winapi
