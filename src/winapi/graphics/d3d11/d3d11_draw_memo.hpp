// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_draw_memo.hpp
/// @brief Track R round 4 (R4): draw-state memos keyed by binding stamps,
///        one switch for its parts, default ON.
///
/// A D3D11 draw re-derived everything it binds from the context's GuestAddr
/// arrays: per stage, a 1 KB compare of the 128 t# addresses and of the 16
/// s# addresses against the last draw's (the stage summaries), and 2 x 8
/// sampler lookups plus their desc translation. R4 keys those derivations on
/// a stamp instead.
///
/// StampedSlots: a binding array whose every mutation takes a process-unique
/// stamp. Equal stamps therefore mean equal contents, whichever context or
/// snapshot the array lives in, and a derivation keyed by the stamp is exact
/// without comparing contents and without a global invalidation epoch:
///   - its type has no mutable element access, so every write path (setters,
///     hazard auto-unbinds, ClearState, SwapDeviceContextState,
///     ExecuteCommandList / FinishCommandList state restores, deferred
///     replay, the D3D10 facets) goes through set()/fill()/assign() and
///     re-stamps; the compiler enumerates them;
///   - what a derivation reads of a bound object (an SRV's format and
///     buffer window, a sampler's desc) is immutable for the object's life,
///     and a bound object cannot die: the binding holds an internal
///     reference (Learn, *SetShaderResources / *SetSamplers: "the method will
///     hold a reference to the interfaces passed in").
///
/// Parts (each an ab-cycle arm, all of them together by "r4"):
///   stage-memo    the SRV and sampler stage summaries keyed by stamp.
///   sampler-memo  each stage's resolved s# descs (and native states) keyed
///                 by the sampler array's stamp.
///   ia-memo       the input layout's elements matched against the vertex
///                 shader's input signature (attributes, instancing, the
///                 strides they imply), kept on the layout and keyed by the
///                 shader's process-unique serial.
///   srv-memo      each stage's resolved flat t# (texture, view desc, volume
///                 view, buffer window, typed buffer) keyed by the SRV
///                 array's stamp and the texture-backend generation, which
///                 every replacement of a live texture's backend object
///                 bumps (ResizeBuffers, a flip-model buffer taking its ring
///                 allocation).
///   om-memo       the attachments the bound RTVs and DSV resolve to
///                 (targets, formats, mips, slices, volume planes, buffer
///                 targets, the depth view), keyed by the stamps of the
///                 render-target array and the depth-stencil view and the
///                 same texture-backend generation.
///   key-memo      the pipeline key: its ~150 inputs are gathered and
///                 compared with the last draw's, and mixed (a serial
///                 dependency chain) only when they differ. Equal inputs give
///                 an equal key, so this needs no invalidation.
///   vb-memo       the backend buffer behind each bound IA slot (and so
///                 whether the slot is bound), keyed by the vertex-buffer
///                 array's stamp: a buffer's backend object is fixed at
///                 creation, and a bound buffer cannot die.
///
/// VITREO_D3D11_DRAW_MEMO=0 turns every part off. VITREO_D3D11_DRAW_MEMO_VERIFY=1
/// (diagnostic) recomputes each memo hit the old way and aborts on any
/// difference. The process says "[r4] draw memo on" or "... OFF" once, and
/// each part "[r4] <part> on|off" when first used and whenever its arm
/// changes.

#include <vitreo/ab_cycle.hpp>
#include <vitreo/types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace vitreo::winapi::d3d11 {

namespace draw_memo_detail {
/// The process-wide stamp source. 0 is the stamp of the all-zero array.
inline std::atomic<u64>& stamp_source() noexcept {
    static std::atomic<u64> source{0};
    return source;
}
}  // namespace draw_memo_detail

/// A binding array that re-stamps on every change (see the file comment).
/// Reads look like std::array; writes go through set / fill / assign.
template <class T, usize N>
class StampedSlots {
public:
    using value_type = T;
    using const_iterator = typename std::array<T, N>::const_iterator;

    StampedSlots() = default;
    StampedSlots(const StampedSlots&) = default;
    StampedSlots& operator=(const StampedSlots&) = default;

    [[nodiscard]] static constexpr usize size() noexcept { return N; }
    [[nodiscard]] static constexpr bool empty() noexcept { return N == 0; }
    [[nodiscard]] const T& operator[](usize i) const noexcept {
        return m_values[i];
    }
    [[nodiscard]] const T* data() const noexcept { return m_values.data(); }
    [[nodiscard]] const_iterator begin() const noexcept {
        return m_values.begin();
    }
    [[nodiscard]] const_iterator end() const noexcept { return m_values.end(); }
    [[nodiscard]] const std::array<T, N>& values() const noexcept {
        return m_values;
    }
    operator std::span<const T>() const noexcept {  // NOLINT: read view
        return std::span<const T>(m_values.data(), N);
    }
    [[nodiscard]] bool operator==(const StampedSlots& other) const noexcept {
        return m_values == other.m_values;
    }

    /// The stamp: equal stamps, equal contents.
    [[nodiscard]] u64 stamp() const noexcept { return m_stamp; }
    /// One past the last non-empty slot (every slot from here is T{}).
    [[nodiscard]] usize used() const noexcept { return m_used; }

    void set(usize i, const T& value) noexcept {
        if (m_values[i] == value) return;
        m_values[i] = value;
        if (value != T{}) {
            if (i >= m_used) m_used = i + 1;
        } else if (i + 1 == m_used) {
            while (m_used != 0 && m_values[m_used - 1] == T{}) --m_used;
        }
        restamp();
    }
    void fill(const T& value) noexcept {
        m_values.fill(value);
        m_used = value != T{} ? N : 0;
        restamp();
    }
    void assign(const std::array<T, N>& values) noexcept {
        if (m_values == values) return;
        m_values = values;
        m_used = N;
        while (m_used != 0 && m_values[m_used - 1] == T{}) --m_used;
        restamp();
    }

private:
    void restamp() noexcept {
        m_stamp = draw_memo_detail::stamp_source().fetch_add(
                      1, std::memory_order_relaxed) +
                  1;
    }

    std::array<T, N> m_values{};
    u64 m_stamp{0};
    usize m_used{0};
};

/// One binding that re-stamps on every change, read as its value.
template <class T>
class StampedValue {
public:
    StampedValue() = default;
    StampedValue(const StampedValue&) = default;
    StampedValue& operator=(const StampedValue&) = default;
    StampedValue& operator=(const T&) = delete;  // writes go through set()

    operator T() const noexcept { return m_value; }  // NOLINT: a read
    [[nodiscard]] T get() const noexcept { return m_value; }
    [[nodiscard]] u64 stamp() const noexcept { return m_stamp; }
    void set(const T& value) noexcept {
        if (m_value == value) return;
        m_value = value;
        m_stamp = draw_memo_detail::stamp_source().fetch_add(
                      1, std::memory_order_relaxed) +
                  1;
    }

private:
    T m_value{};
    u64 m_stamp{0};
};

namespace draw_memo {

enum class Part : u8 {
    StageMemo = 0,
    SamplerMemo = 1,
    IaMemo = 2,
    SrvMemo = 3,
    OmMemo = 4,
    KeyMemo = 5,
    VbMemo = 6,
};
inline constexpr unsigned kParts = 7;
inline constexpr const char* kGroupArm = "r4";

[[nodiscard]] constexpr const char* name(Part part) noexcept {
    switch (part) {
    case Part::StageMemo: return "stage-memo";
    case Part::SamplerMemo: return "sampler-memo";
    case Part::IaMemo: return "ia-memo";
    case Part::SrvMemo: return "srv-memo";
    case Part::OmMemo: return "om-memo";
    case Part::KeyMemo: return "key-memo";
    case Part::VbMemo: return "vb-memo";
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
    static std::atomic<int> value[kParts]{-1, -1, -1, -1, -1, -1, -1};
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

/// Tests only: a deliberate defect for the RED arms -- every memo hits
/// whatever its key says.
enum class Mutation : int { None = 0, IgnoreKeys = 1 };
inline std::atomic<int>& mutation_for_test() noexcept {
    static std::atomic<int> value{0};
    return value;
}
[[nodiscard]] inline bool ignore_keys() noexcept {
    return mutation_for_test().load(std::memory_order_relaxed) ==
           static_cast<int>(Mutation::IgnoreKeys);
}

/// The process-wide switch, read once and announced once on stderr.
[[nodiscard]] inline bool enabled() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_D3D11_DRAW_MEMO");
        const bool off = text != nullptr && text[0] == '0';
        std::fprintf(stderr, off ? "[r4] VITREO_D3D11_DRAW_MEMO=0: draw memo OFF\n"
                                 : "[r4] draw memo on\n");
        return !off;
    }();
    return on;
}

/// VITREO_D3D11_DRAW_MEMO_VERIFY=1: DIAGNOSTIC. Read once.
[[nodiscard]] inline bool verify() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_D3D11_DRAW_MEMO_VERIFY");
        const bool v = text != nullptr && text[0] != '\0' && text[0] != '0';
        if (v) {
            std::fprintf(stderr, "[r4] VITREO_D3D11_DRAW_MEMO_VERIFY: every "
                                 "memo hit is recomputed (diagnostic)\n");
        }
        return v;
    }();
    return on;
}

/// Whether @p part is on for the calling draw: a test pin, the switch, and
/// the part's in-process ab-cycle arm (its own name or "r4").
[[nodiscard]] inline bool active(Part part) noexcept {
    const int pinned = detail::forced(part).load(std::memory_order_relaxed);
    if (pinned != 0) return pinned == static_cast<int>(Force::On);
    if (!enabled()) return false;
    const unsigned index = static_cast<unsigned>(part);
    static thread_local ::vitreo::diagnostics::AbCycleSampler own[kParts];
    static thread_local ::vitreo::diagnostics::AbCycleSampler group[kParts];
    static thread_local int seen[kParts]{-1, -1, -1, -1, -1, -1, -1};
    const bool on =
        !::vitreo::diagnostics::ab_cycle_off_sampled(own[index], name(part)) &&
        !::vitreo::diagnostics::ab_cycle_off_sampled(group[index], kGroupArm);
    if (seen[index] != static_cast<int>(on)) {
        seen[index] = static_cast<int>(on);
        if (detail::announced(part).exchange(static_cast<int>(on),
                                             std::memory_order_relaxed) !=
            static_cast<int>(on)) {
            std::fprintf(stderr, on ? "[r4] %s on\n"
                                    : "[r4] %s off (ab-cycle arm)\n",
                         name(part));
        }
    }
    return on;
}

/// A process-unique serial for a record a memo is keyed by (never 0).
[[nodiscard]] inline u64 next_serial() noexcept {
    static std::atomic<u64> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// Every part's active() at once, as a bit mask (bit = Part): one
/// thread-local lookup per draw instead of one per part per query. Same
/// pins, switch and arms as active().
[[nodiscard]] inline u32 active_mask() noexcept {
    struct Arms {
        ::vitreo::diagnostics::AbCycleSampler own[kParts];
        ::vitreo::diagnostics::AbCycleSampler group;
        int seen[kParts]{-1, -1, -1, -1, -1, -1, -1};
    };
    static thread_local Arms arms;
    Arms& t = arms;
    const bool on_env = enabled();
    const bool group_off =
        on_env && ::vitreo::diagnostics::ab_cycle_off_sampled(t.group, kGroupArm);
    u32 mask = 0;
    for (unsigned i = 0; i < kParts; ++i) {
        const Part part = static_cast<Part>(i);
        const int pinned = detail::forced(part).load(std::memory_order_relaxed);
        bool on = false;
        if (pinned != 0) {
            on = pinned == static_cast<int>(Force::On);
        } else if (on_env) {
            on = !group_off &&
                 !::vitreo::diagnostics::ab_cycle_off_sampled(t.own[i],
                                                              name(part));
            if (t.seen[i] != static_cast<int>(on)) {
                t.seen[i] = static_cast<int>(on);
                if (detail::announced(part).exchange(
                        static_cast<int>(on), std::memory_order_relaxed) !=
                    static_cast<int>(on)) {
                    std::fprintf(stderr, on ? "[r4] %s on\n"
                                            : "[r4] %s off (ab-cycle arm)\n",
                                 name(part));
                }
            }
        }
        if (on) mask |= 1u << i;
    }
    return mask;
}

/// When to print an engagement counter: 1, 2, 4, ... 2^20, then every 2^20.
[[nodiscard]] constexpr bool count_tick(u64 n) noexcept {
    return n != 0 && ((n & (n - 1)) == 0 || (n & 0xFFFFFull) == 0);
}

}  // namespace draw_memo
}  // namespace vitreo::winapi::d3d11
