// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file slim_record.hpp
/// @brief The slim per-draw record (Track R C2, WP-R2): one switch for its
///        three parts, default ON.
///
/// A D3D11 draw reaches Metal as BeginPass, Draw, EndPass on MetalSubmitQueue
/// (d3d11_one_metal_pass_per_draw). Three parts make recording that cheaper on
/// the recording thread without changing what is encoded:
///
///   push-intern  MetalSubmitQueue: a draw's push blocks are copied into the
///                recording's arena, a block equal to the stage's previous one
///                sharing its copy, instead of two recycled vectors a draw
///                (two BlockRecycler mutex round trips).
///   pass-fold    MetalSubmitQueue: a BeginPass that the encoder would carry
///                on in the open encoder (can_continue_pass) reopens the pass
///                recorded last instead of recording one of its own, and the
///                pass is split again at record time wherever the encoder
///                could need a boundary (draw_needs_own_encoder).
///   gs-ring      D3D11 geometry emulation: a draw's transient buffers come
///                from a ring retired by submission generation, not a new
///                MTLBuffer each.
///
/// VITREO_METAL_SLIM_RECORD=0 turns all three off, for bisection and for the
/// gate's cross-run arm. Under VITREO_AB_CYCLE_S each part is also cycled
/// in-process by its own name (VITREO_AB_CYCLE_ONLY=push-intern,pass-fold,
/// gs-ring). The process says which once ("[wpr2] slim draw record on" or
/// "[wpr2] VITREO_METAL_SLIM_RECORD=0: slim draw record OFF"), and each part
/// says "[wpr2] <part> on|off" when first used and whenever its arm changes.

#include <vitreo/ab_cycle.hpp>
#include <vitreo/types.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace vitreo::graphics::slim_record {

enum class Part : u8 { PushIntern = 0, PassFold = 1, GsRing = 2 };
inline constexpr unsigned kParts = 3;

[[nodiscard]] constexpr const char* name(Part part) noexcept {
    switch (part) {
    case Part::PushIntern: return "push-intern";
    case Part::PassFold: return "pass-fold";
    case Part::GsRing: return "gs-ring";
    }
    return "?";
}

/// Tests only: pin one part on or off (None returns it to the environment).
enum class Force : int { None = 0, On = 1, Off = 2 };

namespace detail {
inline std::atomic<int>& forced(Part part) noexcept {
    static std::atomic<int> value[kParts]{};
    return value[static_cast<unsigned>(part)];
}
/// The state last announced for each part (-1: not yet).
inline std::atomic<int>& announced(Part part) noexcept {
    static std::atomic<int> value[kParts]{-1, -1, -1};
    return value[static_cast<unsigned>(part)];
}
}  // namespace detail

inline void force_for_test(Part part, Force force) noexcept {
    detail::forced(part).store(static_cast<int>(force),
                               std::memory_order_relaxed);
}
inline void force_all_for_test(Force force) noexcept {
    for (unsigned i = 0; i < kParts; ++i) {
        force_for_test(static_cast<Part>(i), force);
    }
}

/// The process-wide switch: false only under VITREO_METAL_SLIM_RECORD=0.
/// Read once, and announced once on stderr either way.
[[nodiscard]] inline bool enabled() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_METAL_SLIM_RECORD");
        const bool off = text != nullptr && text[0] == '0';
        std::fprintf(stderr,
                     off ? "[wpr2] VITREO_METAL_SLIM_RECORD=0: slim draw "
                           "record OFF\n"
                         : "[wpr2] slim draw record on\n");
        return !off;
    }();
    return on;
}

/// Whether @p part is on for the call asking: the switch, a test pin, and the
/// part's in-process ab-cycle arm. push-intern and pass-fold ask per draw, so
/// they re-read the arm once per AbCycleSampler::kResample calls of the
/// calling thread (an arm change is seen within ~1k draws, well inside a
/// window). gs-ring asks once per GS buffer, which may be rare enough for a
/// sampled arm to lag a whole window, so it reads the arm every time.
[[nodiscard]] inline bool active(Part part) noexcept {
    const int pinned =
        detail::forced(part).load(std::memory_order_relaxed);
    if (pinned != 0) return pinned == static_cast<int>(Force::On);
    if (!enabled()) return false;
    const unsigned index = static_cast<unsigned>(part);
    static thread_local diagnostics::AbCycleSampler samplers[kParts];
    static thread_local int seen[kParts]{-1, -1, -1};
    const bool on =
        part == Part::GsRing
            ? !diagnostics::ab_cycle_off(name(part))
            : !diagnostics::ab_cycle_off_sampled(samplers[index], name(part));
    if (seen[index] != static_cast<int>(on)) {
        // This thread's arm changed: the first thread to see a change says so.
        seen[index] = static_cast<int>(on);
        if (detail::announced(part).exchange(static_cast<int>(on),
                                             std::memory_order_relaxed) !=
            static_cast<int>(on)) {
            std::fprintf(stderr, on ? "[wpr2] %s on\n"
                                    : "[wpr2] %s off (ab-cycle arm)\n",
                         name(part));
        }
    }
    return on;
}

/// When to print an engagement counter: at 1, 2, 4, ... 2^20, then every
/// 2^20 events, so a run's log shows each part doing its work.
[[nodiscard]] constexpr bool count_tick(u64 n) noexcept {
    return n != 0 && ((n & (n - 1)) == 0 || (n & 0xFFFFFull) == 0);
}

}  // namespace vitreo::graphics::slim_record
