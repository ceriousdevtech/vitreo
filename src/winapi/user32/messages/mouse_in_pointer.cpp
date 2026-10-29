#include "winapi/user32/messages/mouse_in_pointer.hpp"

#include "winapi/kernel32/error.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::mouse_in_pointer {

namespace {

namespace WM = windowing::WM;

constexpr u32 kMbuttonDblclk = 0x0209;
constexpr u32 kXbuttonDown = 0x020B;
constexpr u32 kXbuttonUp = 0x020C;
constexpr u32 kXbuttonDblclk = 0x020D;

constexpr u32 MK_LBUTTON = 0x0001;
constexpr u32 MK_RBUTTON = 0x0002;
constexpr u32 MK_MBUTTON = 0x0010;
constexpr u32 MK_XBUTTON1 = 0x0020;
constexpr u32 MK_XBUTTON2 = 0x0040;
constexpr u32 kKeyStateMask = 0x007F;

// Opaque, nonzero, and distinct from every raw-input device handle, as
// measured for the Windows mouse pointer device.
constexpr u64 kMouseSourceDevice = 0x00010001;

// GetDpiForSystem reports 96, so pixels and HIMETRIC (0.01 mm) relate by
// 2540/96, rounded to nearest as measured.
constexpr double kHimetricPerPixel = 2540.0 / 96.0;

enum class Tri : u8 { Unset, Disabled, Enabled };
std::atomic<Tri> g_state{Tri::Unset};
std::atomic<u32> g_frame_id{0};

struct ThreadRecord {
    bool valid = false;
    PointerInfo info{};
    windowing::MSG pointer{};
    windowing::MSG legacy{};
    bool promoted = false;
    // DefWindowProc promoted the WM_POINTERDOWN that began the current
    // contact. Windows then omits POINTER_FLAG_PRIMARY from that contact's
    // later updates and its WM_POINTERUP.
    bool contact_down_promoted = false;
};

std::mutex g_mutex;
std::unordered_map<u32, ThreadRecord> g_threads;

u32 mk_to_pointer_buttons(u32 mk) noexcept {
    u32 flags = 0;
    if (mk & MK_LBUTTON) flags |= POINTER_FLAG_FIRSTBUTTON;
    if (mk & MK_RBUTTON) flags |= POINTER_FLAG_SECONDBUTTON;
    if (mk & MK_MBUTTON) flags |= POINTER_FLAG_THIRDBUTTON;
    if (mk & MK_XBUTTON1) flags |= POINTER_FLAG_FOURTHBUTTON;
    if (mk & MK_XBUTTON2) flags |= POINTER_FLAG_FIFTHBUTTON;
    return flags;
}

struct ButtonEvent {
    u32 button = 0;   // POINTER_FLAG_*BUTTON that changed; 0 for moves
    bool down = false;
    u32 change_down = 0;  // POINTER_BUTTON_CHANGE_TYPE for down / up
    u32 change_up = 0;
};

bool classify(const windowing::MSG& msg, ButtonEvent& ev) noexcept {
    const u32 xbutton = static_cast<u32>((msg.wParam >> 16) & 0xFFFFu);
    switch (msg.message) {
    case WM::MOUSEMOVE:
        return true;
    case WM::LBUTTONDOWN: case WM::LBUTTONDBLCLK:
        ev = {POINTER_FLAG_FIRSTBUTTON, true, 1, 2}; return true;
    case WM::LBUTTONUP:
        ev = {POINTER_FLAG_FIRSTBUTTON, false, 1, 2}; return true;
    case WM::RBUTTONDOWN: case WM::RBUTTONDBLCLK:
        ev = {POINTER_FLAG_SECONDBUTTON, true, 3, 4}; return true;
    case WM::RBUTTONUP:
        ev = {POINTER_FLAG_SECONDBUTTON, false, 3, 4}; return true;
    case WM::MBUTTONDOWN: case kMbuttonDblclk:
        ev = {POINTER_FLAG_THIRDBUTTON, true, 5, 6}; return true;
    case WM::MBUTTONUP:
        ev = {POINTER_FLAG_THIRDBUTTON, false, 5, 6}; return true;
    case kXbuttonDown: case kXbuttonDblclk: case kXbuttonUp: {
        const bool down = msg.message != kXbuttonUp;
        if (xbutton == 1) { ev = {POINTER_FLAG_FOURTHBUTTON, down, 7, 8}; return true; }
        if (xbutton == 2) { ev = {POINTER_FLAG_FIFTHBUTTON, down, 9, 10}; return true; }
        return false;
    }
    default:
        return false;
    }
}

u64 screen_lparam(const windowing::MSG& msg) noexcept {
    return (static_cast<u64>(static_cast<u16>(msg.pt.y)) << 16) |
           static_cast<u16>(msg.pt.x);
}

}  // namespace

EnableResult enable(u64 f_enable) noexcept {
    const u32 value = static_cast<u32>(f_enable);
    if (value > 1) return {false, error_codes::ERROR_INVALID_PARAMETER};
    const Tri wanted = value ? Tri::Enabled : Tri::Disabled;
    Tri expected = Tri::Unset;
    if (g_state.compare_exchange_strong(expected, wanted)) return {true, 0};
    if (expected == wanted) return {true, 0};
    return {false, error_codes::ERROR_ACCESS_DENIED};
}

bool enabled() noexcept {
    return g_state.load(std::memory_order_acquire) == Tri::Enabled;
}

bool convert(windowing::MSG& msg, u32 thread_id, u64 performance_count) noexcept {
    if (!msg.hardware_input || msg.pointer_promoted || !enabled()) return false;

    const bool wheel = msg.message == WM::MOUSEWHEEL;
    const bool hwheel = msg.message == WM::MOUSEHWHEEL;
    ButtonEvent ev{};
    if (!wheel && !hwheel && !classify(msg, ev)) return false;

    const u32 mk = static_cast<u32>(msg.wParam & 0xFFFFu);
    u32 buttons_after = mk_to_pointer_buttons(mk);
    if (ev.button != 0) {
        if (ev.down) buttons_after |= ev.button;
        else buttons_after &= ~ev.button;
    }
    u32 buttons_before = buttons_after;
    if (ev.button != 0) {
        if (ev.down) buttons_before &= ~ev.button;
        else buttons_before |= ev.button;
    }
    const bool contact_before = buttons_before != 0;
    const bool contact_after = buttons_after != 0;

    std::lock_guard lock(g_mutex);
    ThreadRecord& rec = g_threads[thread_id];

    u32 message = WM_POINTERUPDATE;
    u32 transition = POINTER_FLAG_UPDATE;
    u32 change = 0;
    if (wheel) {
        message = WM_POINTERWHEEL;
        transition = POINTER_FLAG_WHEEL;
    } else if (hwheel) {
        message = WM_POINTERHWHEEL;
        transition = POINTER_FLAG_HWHEEL;
    } else if (ev.button != 0) {
        change = ev.down ? ev.change_down : ev.change_up;
        if (ev.down && !contact_before) {
            message = WM_POINTERDOWN;
            transition = POINTER_FLAG_DOWN;
            rec.contact_down_promoted = false;
        } else if (!ev.down && !contact_after) {
            message = WM_POINTERUP;
            transition = POINTER_FLAG_UP;
        }
    }

    const bool primary = !(rec.contact_down_promoted && contact_before);
    const u32 state = POINTER_FLAG_INRANGE |
                      (contact_after ? POINTER_FLAG_INCONTACT : 0u) |
                      buttons_after |
                      (primary ? POINTER_FLAG_PRIMARY : 0u);
    if (message == WM_POINTERUP) rec.contact_down_promoted = false;

    const i16 delta = static_cast<i16>((msg.wParam >> 16) & 0xFFFFu);
    const u64 w_param = (wheel || hwheel)
        ? ((static_cast<u64>(static_cast<u16>(delta)) << 16) | kMousePointerId)
        : ((static_cast<u64>(state & 0xFFFFu) << 16) | kMousePointerId);

    PointerInfo info{};
    info.pointer_type = PT_MOUSE;
    info.pointer_id = kMousePointerId;
    info.frame_id = g_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
    info.pointer_flags = state | transition;
    info.source_device = kMouseSourceDevice;
    info.hwnd_target = msg.hwnd;
    info.pixel_x = msg.pt.x;
    info.pixel_y = msg.pt.y;
    info.himetric_x = static_cast<i32>(std::lround(msg.pt.x * kHimetricPerPixel));
    info.himetric_y = static_cast<i32>(std::lround(msg.pt.y * kHimetricPerPixel));
    info.time = msg.time;
    info.history_count = 1;
    info.input_data = (wheel || hwheel) ? delta : 0;
    info.key_states = mk & kKeyStateMask;
    info.performance_count = performance_count;
    info.button_change_type = change;

    rec.valid = true;
    rec.info = info;
    rec.legacy = msg;
    rec.legacy.pointer_promoted = true;
    rec.promoted = false;

    msg.message = message;
    msg.wParam = w_param;
    msg.lParam = screen_lparam(msg);
    rec.pointer = msg;
    return true;
}

u32 pointer_info(u32 thread_id, u32 pointer_id, PointerInfo& out) noexcept {
    if (pointer_id != kMousePointerId) return error_codes::ERROR_INVALID_PARAMETER;
    std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(thread_id);
    if (it == g_threads.end() || !it->second.valid) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    out = it->second.info;
    return 0;
}

u32 pointer_type(u32 pointer_id, u32& out) noexcept {
    if (pointer_id != kMousePointerId) return error_codes::ERROR_INVALID_PARAMETER;
    out = PT_MOUSE;
    return 0;
}

std::optional<windowing::MSG> promote(u32 thread_id, windowing::HWND hwnd,
                                      u32 message, u64 w_param,
                                      u64 l_param) noexcept {
    std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(thread_id);
    if (it == g_threads.end()) return std::nullopt;
    ThreadRecord& rec = it->second;
    if (!rec.valid || rec.promoted || rec.pointer.hwnd != hwnd ||
        rec.pointer.message != message ||
        static_cast<u32>(rec.pointer.wParam) != static_cast<u32>(w_param) ||
        static_cast<u32>(rec.pointer.lParam) != static_cast<u32>(l_param)) {
        return std::nullopt;
    }
    rec.promoted = true;
    if (message == WM_POINTERDOWN) rec.contact_down_promoted = true;
    return rec.legacy;
}

PointerInfoImage pointer_info_image(const PointerInfo& info, bool is_64bit) noexcept {
    PointerInfoImage image;
    auto put32 = [&](usize off, u32 v) { std::memcpy(image.bytes.data() + off, &v, 4); };
    auto put64 = [&](usize off, u64 v) { std::memcpy(image.bytes.data() + off, &v, 8); };
    put32(0, info.pointer_type);
    put32(4, info.pointer_id);
    put32(8, info.frame_id);
    put32(12, info.pointer_flags);
    // HANDLE and HWND are pointer-sized; every later field shifts with them.
    usize off = 16;
    if (is_64bit) {
        put64(off, info.source_device);
        put64(off + 8, info.hwnd_target);
        off += 16;
    } else {
        put32(off, static_cast<u32>(info.source_device));
        put32(off + 4, info.hwnd_target);
        off += 8;
    }
    // ptPixelLocation, ptHimetricLocation, ptPixelLocationRaw,
    // ptHimetricLocationRaw: a mouse has no prediction, so raw == predicted.
    for (int pass = 0; pass < 2; ++pass) {
        put32(off + 0, static_cast<u32>(info.pixel_x));
        put32(off + 4, static_cast<u32>(info.pixel_y));
        put32(off + 8, static_cast<u32>(info.himetric_x));
        put32(off + 12, static_cast<u32>(info.himetric_y));
        off += 16;
    }
    put32(off + 0, info.time);
    put32(off + 4, info.history_count);
    put32(off + 8, static_cast<u32>(info.input_data));
    put32(off + 12, info.key_states);
    put64(off + 16, info.performance_count);
    put32(off + 24, info.button_change_type);
    image.size = is_64bit ? 96 : 88;
    return image;
}

void reset_for_testing() noexcept {
    g_state.store(Tri::Unset);
    g_frame_id.store(0);
    std::lock_guard lock(g_mutex);
    g_threads.clear();
}

}  // namespace vitreo::winapi::mouse_in_pointer
