#pragma once

#include <cstddef>
#include <deque>

namespace vitreo::windowing {

// The device motion of host mouse events, without Vitreo's own cursor warps.
//
// Windows: DirectInput's relative mouse axes and WM_INPUT's RAWMOUSE
// lLastX/lLastY report how far the device moved. SetCursorPos and the
// ClipCursor confinement move the cursor without any device motion, so
// neither appears in them.
//
// macOS: kCGMouseEventDeltaX/Y (NSEvent deltaX/deltaY) are the cursor's
// movement "since the last mouse movement event", and CGWarpMouseCursorPosition
// moves the cursor "without generating events". The first mouse event after a
// warp therefore carries the warp's displacement as if the device had moved
// that far. Every warp records its displacement here; an event's delta loses
// the warps made before the event happened, and keeps those made after it.
class CursorWarpDeltas {
public:
    /// A warp at host time @p time (seconds since startup, the NSEvent
    /// timestamp clock) moved the cursor by (@p dx, @p dy) points, y down.
    void note_warp(double time, double dx, double dy) {
        if (m_warps.size() == kMaxPending) {
            // Nothing has moved the mouse for a long run of warps: fold the
            // two oldest. An event between them is not expected, since every
            // device movement produces one.
            const Warp oldest = m_warps.front();
            m_warps.pop_front();
            m_warps.front().dx += oldest.dx;
            m_warps.front().dy += oldest.dy;
        }
        m_warps.push_back(Warp{time, dx, dy});
    }

    /// Turn the delta (@p dx, @p dy) of a host mouse event at @p event_time
    /// into device motion: remove every warp made before that event.
    void remove_warps(double event_time, double& dx, double& dy) {
        while (!m_warps.empty() && m_warps.front().time <= event_time) {
            dx -= m_warps.front().dx;
            dy -= m_warps.front().dy;
            m_warps.pop_front();
        }
    }

    [[nodiscard]] bool empty() const noexcept { return m_warps.empty(); }

private:
    static constexpr std::size_t kMaxPending = 64;
    struct Warp {
        double time;
        double dx;
        double dy;
    };
    std::deque<Warp> m_warps;
};

} // namespace vitreo::windowing
