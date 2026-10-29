#include "winapi/user32/windows/set_cursor_pos.hpp"
#include "windowing/model/diag_flags.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

namespace {

/// Clamp one axis into the half-open span [lo, hi). An empty span (hi <= lo)
/// has no interior; pin to lo so the result is still deterministic.
[[nodiscard]] i32 clamp_axis(i32 v, i32 lo, i32 hi) noexcept {
    if (hi <= lo) return lo;
    return std::clamp(v, lo, hi - 1);
}

} // namespace

CursorPoint confine_cursor_point(i32 x, i32 y, const windowing::Rect& screen,
                                 const windowing::Rect* clip) noexcept {
    CursorPoint p{x, y};
    if (clip != nullptr) {
        p.x = clamp_axis(p.x, clip->left, clip->right);
        p.y = clamp_axis(p.y, clip->top, clip->bottom);
    }
    p.x = clamp_axis(p.x, screen.left, screen.right);
    p.y = clamp_axis(p.y, screen.top, screen.bottom);
    return p;
}

void impl_SetCursorPos(Marshaler& m, windowing::CursorHost& cursor_host,
                       const windowing::Rect& screen) {
    const auto x = static_cast<i32>(static_cast<u32>(m.arg(0)));
    const auto y = static_cast<i32>(static_cast<u32>(m.arg(1)));

    windowing::Rect clip{};
    const bool clipped =
        cursor_host.get_cursor_clip(clip.left, clip.top, clip.right,
                                    clip.bottom);
    const CursorPoint target =
        confine_cursor_point(x, y, screen, clipped ? &clip : nullptr);

    const bool input_diag = windowing::diag::input_diag();
    if (input_diag) {
        // The caller names the guest code that computed (x, y); a wild value
        // (INT_MIN is x86's "integer indefinite" from a NaN/Inf float) is
        // only diagnosable at that call site.
        std::fprintf(stderr,
                     "[input] SetCursorPos(%d,%d) -> (%d,%d) caller=0x%llx\n",
                     x, y, target.x, target.y,
                     static_cast<unsigned long long>(
                         m.guest_return_address()));
        // A position the confinement moved came from wrong math upstream of
        // the immediate caller (which is often a thin wrapper that only
        // truncates floats). List the code-looking values on the guest stack
        // so the frame that computed it can be symbolized offline.
        if ((target.x != x || target.y != y) && m.has_cpu()) {
            const bool is64 = m.cpu_is_64bit();
            const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
            const u32 slot = is64 ? 8u : 4u;
            std::fprintf(stderr, "[input]   stack sp=0x%llx:",
                         static_cast<unsigned long long>(sp));
            for (u32 i = 0; i < 128; ++i) {
                const GuestAddr at = sp + static_cast<GuestAddr>(i) * slot;
                const u64 v = is64 ? m.read_u64(at).value_or(0)
                                   : m.read_u32(at).value_or(0);
                const bool code_like =
                    is64 ? (v >= 0x7ff000000000ull && v < 0x800000000000ull)
                         : (v >= 0x00400000ull && v < 0x80000000ull);
                if (code_like) {
                    std::fprintf(stderr, " +0x%x=0x%llx", i * slot,
                                 static_cast<unsigned long long>(v));
                }
            }
            std::fprintf(stderr, "\n");
        }
    }
    if (!cursor_host.set_native_cursor_pos(target.x, target.y)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

} // namespace vitreo::winapi
