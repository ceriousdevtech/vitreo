// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file delta_record.hpp
/// @brief The delta draw record (Track R round 3, R3): one switch for its
///        three parts, default ON.
///
/// After WP-R2 (slim_record.hpp) a D3D11 draw still reached Metal as a
/// ~2 KB CsDraw inside a ~3.4 KB queue op holding ~15 retained handles, with
/// every sampler and depth-stencil state looked up by hashing its desc and a
/// heap-allocated pass object around it. Three parts make recording cheaper on
/// the recording thread without changing what is encoded:
///
///   delta-record  MetalSubmitQueue: a draw is recorded as the fields that
///                 changed since the draw recorded before it in the same
///                 stream (a DeltaDraw in the recording's arena; consecutive
///                 draws of one pass share one queue op), and the encode
///                 thread applies it to the full state it keeps. Handles are
///                 retained when a draw first carries them, not per draw, and
///                 a buffer's GPU-use stamp is taken once per recording
///                 generation and storage. The first draw of every recording,
///                 and the first after anything else wrote draw state into the
///                 stream, carries every field (a keyframe).
///   native-state  Sampler and depth-stencil states built once, when the API
///                 object is created (GraphicsBackendDevice::
///                 create_sampler_state / create_depth_stencil_state), and
///                 named by pointer in BackendDrawCall instead of hashed per
///                 draw.
///   pass-continue A render pass whose EndPass would only be dropped again by
///                 the next pass folding into it (pass-fold) is ended lazily,
///                 and pass objects come from a per-thread pool.
///
/// VITREO_METAL_DELTA_RECORD=0 turns all three off, for bisection and the
/// gate's cross-run arm; so do VITREO_METAL_SLIM_RECORD=0, VITREO_GPU_DUMP and
/// VITREO_METAL_PASS_BREAK_DIAG (full-state ops on every draw). Under
/// VITREO_AB_CYCLE_S each part is cycled in-process by its own name or, all
/// three together, by "r3" (VITREO_AB_CYCLE_ONLY=r3). The process says which
/// once ("[r3] delta draw record on" or "[r3] ...: delta draw record OFF"),
/// and each part says "[r3] <part> on|off" when first used and whenever its
/// arm changes. VITREO_METAL_DELTA_VERIFY=1 (diagnostic only) also carries
/// each draw's full-state hash and checks it where the encoder applies the
/// delta, and checks every native-state pointer against its desc.

#include <vitreo/ab_cycle.hpp>
#include <vitreo/types.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace vitreo::graphics::delta_record {

enum class Part : u8 { DeltaRecord = 0, NativeState = 1, PassContinue = 2 };
inline constexpr unsigned kParts = 3;
/// The ab-cycle name that cycles every part together.
inline constexpr const char* kGroupArm = "r3";

[[nodiscard]] constexpr const char* name(Part part) noexcept {
    switch (part) {
    case Part::DeltaRecord: return "delta-record";
    case Part::NativeState: return "native-state";
    case Part::PassContinue: return "pass-continue";
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
inline std::atomic<int>& announced(Part part) noexcept {
    static std::atomic<int> value[kParts]{-1, -1, -1};
    return value[static_cast<unsigned>(part)];
}
[[nodiscard]] inline bool env_set(const char* name) noexcept {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
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

/// The process-wide switch, read once and announced once on stderr.
[[nodiscard]] inline bool enabled() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_METAL_DELTA_RECORD");
        const char* slim = std::getenv("VITREO_METAL_SLIM_RECORD");
        const char* why = nullptr;
        if (text != nullptr && text[0] == '0') {
            why = "VITREO_METAL_DELTA_RECORD=0";
        } else if (slim != nullptr && slim[0] == '0') {
            why = "VITREO_METAL_SLIM_RECORD=0";
        } else if (detail::env_set("VITREO_GPU_DUMP")) {
            why = "VITREO_GPU_DUMP";
        } else if (detail::env_set("VITREO_METAL_PASS_BREAK_DIAG")) {
            why = "VITREO_METAL_PASS_BREAK_DIAG";
        }
        if (why != nullptr) {
            std::fprintf(stderr, "[r3] %s: delta draw record OFF\n", why);
        } else {
            std::fprintf(stderr, "[r3] delta draw record on\n");
        }
        return why == nullptr;
    }();
    return on;
}

/// VITREO_METAL_DELTA_VERIFY=1: DIAGNOSTIC. Read once.
[[nodiscard]] inline bool verify() noexcept {
    static const bool on = [] {
        const bool v = detail::env_set("VITREO_METAL_DELTA_VERIFY") &&
                       std::getenv("VITREO_METAL_DELTA_VERIFY")[0] != '0';
        if (v) {
            std::fprintf(stderr, "[r3] VITREO_METAL_DELTA_VERIFY: every delta "
                                 "draw is checked against its full state "
                                 "(diagnostic)\n");
        }
        return v;
    }();
    return on;
}

/// Whether @p part is on for the call asking: a test pin, the switch, and
/// the part's in-process ab-cycle arm (its own name or the group's). Asked
/// per draw, so the arm is re-read once per AbCycleSampler::kResample calls
/// of the calling thread.
[[nodiscard]] inline bool active(Part part) noexcept {
    const int pinned = detail::forced(part).load(std::memory_order_relaxed);
    if (pinned != 0) return pinned == static_cast<int>(Force::On);
    if (!enabled()) return false;
    const unsigned index = static_cast<unsigned>(part);
    static thread_local diagnostics::AbCycleSampler own[kParts];
    static thread_local diagnostics::AbCycleSampler group[kParts];
    static thread_local int seen[kParts]{-1, -1, -1};
    const bool on =
        !diagnostics::ab_cycle_off_sampled(own[index], name(part)) &&
        !diagnostics::ab_cycle_off_sampled(group[index], kGroupArm);
    if (seen[index] != static_cast<int>(on)) {
        seen[index] = static_cast<int>(on);
        if (detail::announced(part).exchange(static_cast<int>(on),
                                             std::memory_order_relaxed) !=
            static_cast<int>(on)) {
            std::fprintf(stderr, on ? "[r3] %s on\n"
                                    : "[r3] %s off (ab-cycle arm)\n",
                         name(part));
        }
    }
    return on;
}

/// When to print an engagement counter: 1, 2, 4, ... 2^20, then every 2^20.
[[nodiscard]] constexpr bool count_tick(u64 n) noexcept {
    return n != 0 && ((n & (n - 1)) == 0 || (n & 0xFFFFFull) == 0);
}

}  // namespace vitreo::graphics::delta_record
