// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vitreo::cpu::jit::ir {

// Backing storage belongs to one execute() invocation, not to a compiler or
// translated block. Linked blocks may reuse slots only after their values die.
// Host-stack allocation keeps nested execution independent without changing
// the native LR/NZCV frame understood by the fault trampoline.
struct alignas(16) SpillSlot {
    std::uint64_t lanes[2];
};

struct SpillFrame {
    static constexpr std::size_t kSlotCount = 64;
    std::array<SpillSlot, kSlotCount> slots;
};

// No initialization of slot contents: generated code must define a slot before
// every read. Slots contain temporary snapshots, never authoritative guest
// state. Fault recovery continues to use architectural commits and their pins.
class SpillFrameScope {
public:
    explicit SpillFrameScope(SpillFrame*& active) noexcept
        : m_active(active), m_previous(active) { m_active = &m_frame; }
    ~SpillFrameScope() { m_active = m_previous; }
    SpillFrameScope(const SpillFrameScope&) = delete;
    SpillFrameScope& operator=(const SpillFrameScope&) = delete;
    SpillFrameScope(SpillFrameScope&&) = delete;
    SpillFrameScope& operator=(SpillFrameScope&&) = delete;

private:
    SpillFrame*& m_active;
    SpillFrame* m_previous;
    SpillFrame m_frame;
};

static_assert(sizeof(SpillSlot) == 16);
static_assert(alignof(SpillFrame) == 16);
static_assert(offsetof(SpillFrame, slots) == 0);

} // namespace vitreo::cpu::jit::ir
