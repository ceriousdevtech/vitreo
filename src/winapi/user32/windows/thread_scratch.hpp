#pragma once

/// @file thread_scratch.hpp
/// @brief Per-thread guest scratch for structures USER passes to a window
/// procedure by pointer (STYLESTRUCT, WINDOWPOS, ...).
///
/// On Windows these structures live on the sending thread's stack: every
/// thread and every nesting level has its own copy. A single shared guest
/// buffer lets two threads (or a nested SetWindowLong from inside
/// WM_STYLECHANGING) overwrite each other's in/out structure while a window
/// procedure is still reading it. Each host thread (one per guest thread)
/// gets its own slot of the region, used as a stack.

#include <vitreo/common.hpp>

#include <atomic>

namespace vitreo::winapi {

class ThreadScratch {
public:
    static constexpr u32 kSlotSize = 0x800;

    void configure(GuestAddr base, u32 size) noexcept {
        m_base.store(base, std::memory_order_release);
        m_slots.store(size / kSlotSize, std::memory_order_release);
    }

    [[nodiscard]] bool configured() const noexcept {
        return m_base.load(std::memory_order_acquire) != 0 &&
               m_slots.load(std::memory_order_acquire) != 0;
    }

    /// A reservation that is released when it goes out of scope. address()
    /// is 0 when the region is not configured or this thread's slot is full.
    class Frame {
    public:
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&& other) noexcept
            : m_address(other.m_address), m_bytes(other.m_bytes) {
            other.m_address = 0;
            other.m_bytes = 0;
        }
        ~Frame() {
            if (m_address != 0) state().used -= m_bytes;
        }
        [[nodiscard]] GuestAddr address() const noexcept { return m_address; }

    private:
        friend class ThreadScratch;
        Frame(GuestAddr address, u32 bytes) : m_address(address), m_bytes(bytes) {}
        GuestAddr m_address;
        u32 m_bytes;
    };

    /// Reserve @p bytes (rounded up to 16) on the calling thread's slot.
    [[nodiscard]] Frame reserve(u32 bytes) noexcept {
        const GuestAddr base = m_base.load(std::memory_order_acquire);
        const u32 slots = m_slots.load(std::memory_order_acquire);
        if (base == 0 || slots == 0) return Frame{0, 0};
        auto& s = state();
        if (s.slot < 0) {
            s.slot = static_cast<i32>(m_next_slot.fetch_add(1, std::memory_order_relaxed) % slots);
        }
        const u32 rounded = (bytes + 15u) & ~15u;
        if (s.used + rounded > kSlotSize) return Frame{0, 0};
        const GuestAddr address =
            base + static_cast<GuestAddr>(s.slot) * kSlotSize + s.used;
        s.used += rounded;
        return Frame{address, rounded};
    }

private:
    struct State {
        i32 slot = -1;
        u32 used = 0;
    };
    static State& state() noexcept {
        thread_local State s;
        return s;
    }

    std::atomic<GuestAddr> m_base{0};
    std::atomic<u32> m_slots{0};
    std::atomic<u32> m_next_slot{0};
};

} // namespace vitreo::winapi
