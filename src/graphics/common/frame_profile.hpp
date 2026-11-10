// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file frame_profile.hpp
/// @brief Where one guest frame's CPU and GPU time goes, from the graphics
/// frontend down to the Metal encoder: `VITREO_D3D12_PROFILE=1`.
///
/// Every number is a relaxed atomic added behind one cached flag, so a call
/// site costs one predictable branch when the profile is off and a clock read
/// plus an atomic add when it is on. Nothing is printed per event: once a
/// second the frontend's Present calls tick(), which writes one
/// `[d3d12-profile]` line of per-frame averages and resets the counters.
///
/// The profile names no application. It counts API families, replay phases,
/// encoder boundaries with their causes, CPU waits with their reasons and
/// draw-time pipeline compiles: what any guest's frame is made of.

#include <vitreo/common.hpp>

#include <atomic>
#include <time.h>

namespace vitreo::graphics::frame_profile {

/// Times, in nanoseconds, summed over the report interval.
enum class Time : u8 {
    ApiRecord,       ///< command-list and allocator methods
    ApiExecute,      ///< ExecuteCommandLists (replay included)
    ApiPresent,      ///< IDXGISwapChain::Present*
    ApiSync,         ///< queue Signal/Wait, fence methods
    ApiPso,          ///< Create*PipelineState, state objects, libraries
    ApiResource,     ///< resources, heaps, views, descriptor copies, Map
    ApiOther,        ///< every other frontend method
    LockWait,        ///< time spent acquiring the frontend lock
    Replay,          ///< replay of recorded lists into backend work
    BackendFlush,    ///< the backend flush at the end of an Execute
    Encode,          ///< the Metal worker encoding one list
    CpuWait,         ///< CPU blocked on the GPU or a fence
    VariantCompile,  ///< draw-time pipeline variants built in replay
    PresentWait,     ///< frame-latency / drawable waits inside Present
    Count
};

/// Event counts summed over the report interval.
enum class Counter : u8 {
    ApiCalls,
    Executes,
    ListsReplayed,
    Draws,           ///< draws replayed by the frontend
    Dispatches,      ///< dispatches replayed by the frontend
    EncodedDraws,    ///< draws the Metal worker encoded
    EncodedDispatches,
    RenderEncoders,  ///< render encoders opened
    ComputeEncoders,
    BlitEncoders,
    PassesMerged,    ///< BeginPass folded into the open encoder
    PassesBegun,     ///< BeginPass ops seen by the encoder
    VariantCompiles,
    WindowResolves,  ///< descriptor-heap window slots resolved
    WindowSyncs,     ///< heap window synchronised for a draw
    ResidencyDecls,  ///< resources declared resident (useResource/useHeap)
    UploadBytes,     ///< replay upload-ring bytes
    UploadChunks,    ///< upload-ring chunks newly created
    TableSets,       ///< descriptor-table slots written in replay
    RootCbvSizeScans,///< root CBV/SRV/UAV size lookups
    LockContended,   ///< frontend lock acquisitions that had to wait
    Count
};

[[nodiscard]] bool enabled() noexcept;

/// Tests: force the profile on or off (it is otherwise read once from the
/// environment).
void set_enabled_for_testing(bool on) noexcept;

/// Nanoseconds on a monotonic clock. On arm64 the generic timer is read
/// directly (a register read, not a call): the profile times every API call,
/// and clock_gettime cost as much as a short record call.
[[nodiscard]] inline u64 now_ns() noexcept {
#if defined(__aarch64__)
    u64 ticks;
    u64 frequency;
    asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (frequency == 24'000'000u) return ticks * 125u / 3u;
    return frequency != 0 ? ticks / frequency * 1'000'000'000u +
                                ticks % frequency * 1'000'000'000u / frequency
                          : clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
}

void add(Time time, u64 ns) noexcept;
void add(Counter counter, u64 amount = 1) noexcept;

/// A render-encoder boundary and why it happened. @p reason is a string
/// literal (compared by address, counted per reason).
void note_break(const char* reason) noexcept;
/// The frontend issued internal GPU work (emulation dispatches, captures)
/// for one guest command, for @p reason (a literal, counted per reason).
void note_internal(const char* reason) noexcept;
/// The CPU waited @p ns for the GPU or a fence, for @p reason (a literal).
void note_wait(const char* reason, u64 ns) noexcept;
/// Whole API duration by static method name, for exclusive frontend calls.
/// Includes acquisition waits; separate from GPU/CPU waits above.
void note_exclusive_api(const char* reason, u64 ns) noexcept;

/// Called once per guest Present: counts the frame, tracks the worst frame
/// interval and prints the line once a second.
void tick() noexcept;

/// A snapshot of the counters since the last take (tests and benchmarks).
struct Snapshot {
    u64 times[static_cast<usize>(Time::Count)]{};
    u64 counters[static_cast<usize>(Counter::Count)]{};
    u64 frames{0};
};
/// Read and reset every counter (and the reason tables).
[[nodiscard]] Snapshot take() noexcept;
/// The reasons recorded since the last take/print as "reason=n ..." text.
void format_reasons(char* out, usize size) noexcept;

/// RAII: adds the scope's duration to @p time when the profile is on.
class Scope {
public:
    explicit Scope(Time time) noexcept
        : m_time(time), m_start(enabled() ? now_ns() : 0) {}
    ~Scope() {
        if (m_start != 0) add(m_time, now_ns() - m_start);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Time m_time;
    u64 m_start;
};

}  // namespace vitreo::graphics::frame_profile

namespace vitreo::graphics::frame_profile {

/// RAII: one CPU wait for @p reason (a string literal), timed when the
/// profile is on.
class WaitScope {
public:
    explicit WaitScope(const char* reason) noexcept
        : m_reason(reason), m_start(enabled() ? now_ns() : 0) {}
    ~WaitScope() {
        if (m_start != 0) note_wait(m_reason, now_ns() - m_start);
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    const char* m_reason;
    u64 m_start;
};

}  // namespace vitreo::graphics::frame_profile
