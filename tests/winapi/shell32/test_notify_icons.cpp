/// @file test_notify_icons.cpp
/// @brief Shell_NotifyIconA/W and Shell_NotifyIconGetRect against Windows.
///
/// The tray and click probes (tools/oracle_probe/notifyicon_tray_probe.c,
/// notifyicon_click_probe.c) are replayed call for call against
/// NotifyIconManager in both bitnesses and printed in the probes' format; the
/// lines are compared with Windows' own .expected output. The presenter is a
/// fake that reports a 32x48 rectangle for a visible icon (the size Windows
/// reported in the click probe). Balloon queueing follows Learn.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/shell32/notify_icons.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using windowing::NotifyIconInput;

namespace {

constexpr GuestAddr kRegion = 0x20000;
constexpr GuestAddr kData = kRegion;            // NOTIFYICONDATA
constexpr GuestAddr kIdentifier = kRegion + 0x800;
constexpr GuestAddr kRect = kRegion + 0x900;
constexpr u32 kHwnd = 0x10010;
constexpr u32 kOwner = 0x10020;
constexpr u32 kIcon = 0x20010;
constexpr u32 kCallback = 0x8001;  // WM_APP + 1

constexpr u32 NIF_MESSAGE = 0x01, NIF_ICON = 0x02, NIF_TIP = 0x04, NIF_STATE = 0x08,
              NIF_INFO = 0x10, NIF_GUID = 0x20, NIF_REALTIME = 0x40;
constexpr u32 NIS_HIDDEN = 0x01;

// {6b2c1f0a-3d4e-4f51-9a61-7c882e105d44} in GUID memory order.
constexpr std::array<u8, 16> kGuid = {0x0a, 0x1f, 0x2c, 0x6b, 0x4e, 0x3d, 0x51, 0x4f,
                                      0x9a, 0x61, 0x7c, 0x88, 0x2e, 0x10, 0x5d, 0x44};

struct Nid {
    u32 cb = 0;
    u32 hwnd = kHwnd;
    u32 uid = 0;
    u32 flags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    u32 callback = kCallback;
    u32 icon = kIcon;
    std::string tip = "probe";
    u32 state = 0;
    u32 state_mask = 0;
    std::string info;
    std::string title;
    u32 version = 0;
    u32 info_flags = 0;
    bool with_guid = false;
};

class NotifyIconTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kRegion, 0x1000, MemoryProtection::ReadWrite).is_ok());
        if (GetParam()) cpu.init_long_mode();
        else cpu.init_protected_mode();
        mgr.set_window_check([this](u32 hwnd) { return windows.count(hwnd) != 0; });
        mgr.set_poster([this](u32 hwnd, u32 message, u64 wparam, u64 lparam) {
            posted.push_back({hwnd, message, wparam, lparam});
        });
        mgr.set_icon_image([](u32) -> std::optional<NotifyIconManager::IconImage> {
            return NotifyIconManager::IconImage{std::vector<u8>(4 * 4 * 4, 0xff), 4, 4};
        });
        NotifyIconManager::Presenter presenter;
        presenter.present = [this](u64 key, const windowing::NotifyIconPresentation& view) {
            presented[key] = view;
        };
        presenter.withdraw = [this](u64 key) { presented.erase(key); withdrawn.push_back(key); };
        presenter.frame = [](u64) { return std::optional<windowing::Rect>(windowing::Rect{100, 0, 132, 48}); };
        presenter.show_balloon = [this](u64 key, const windowing::NotifyIconBalloon& b) {
            balloons.push_back({key, b});
        };
        presenter.hide_balloon = [this](u64 key) { hidden_balloons.push_back(key); };
        mgr.set_presenter(std::move(presenter));
    }

    [[nodiscard]] bool is64() const { return GetParam(); }
    const NotifyIconDataLayout& layout(bool wide) const {
        return wide ? (is64() ? kNotifyIconDataW64 : kNotifyIconDataW32)
                    : (is64() ? kNotifyIconDataA64 : kNotifyIconDataA32);
    }
    Nid base(u32 id, bool wide = true) {
        Nid d;
        d.cb = layout(wide).full_size;
        d.uid = id;
        return d;
    }
    void write_ptr(GuestAddr at, u32 value) {
        if (is64()) ASSERT_TRUE(memory.write_u64(at, value).is_ok());
        else ASSERT_TRUE(memory.write_u32(at, value).is_ok());
    }
    void write_text(GuestAddr at, const std::string& text, bool wide) {
        for (usize i = 0; i <= text.size(); ++i) {
            const u8 c = i < text.size() ? static_cast<u8>(text[i]) : 0;
            if (wide) ASSERT_TRUE(memory.write_u16(at + 2 * i, c).is_ok());
            else ASSERT_TRUE(memory.write_u8(at + i, c).is_ok());
        }
    }
    void write(const Nid& d, bool wide) {
        const auto& L = layout(wide);
        for (u32 i = 0; i < L.full_size; ++i) ASSERT_TRUE(memory.write_u8(kData + i, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kData, d.cb).is_ok());
        write_ptr(kData + L.hwnd, d.hwnd);
        ASSERT_TRUE(memory.write_u32(kData + L.uid, d.uid).is_ok());
        ASSERT_TRUE(memory.write_u32(kData + L.flags, d.flags).is_ok());
        ASSERT_TRUE(memory.write_u32(kData + L.callback_message, d.callback).is_ok());
        write_ptr(kData + L.icon, d.icon);
        write_text(kData + L.tip, d.tip, wide);
        ASSERT_TRUE(memory.write_u32(kData + L.state, d.state).is_ok());
        ASSERT_TRUE(memory.write_u32(kData + L.state_mask, d.state_mask).is_ok());
        write_text(kData + L.info, d.info, wide);
        ASSERT_TRUE(memory.write_u32(kData + L.version, d.version).is_ok());
        write_text(kData + L.info_title, d.title, wide);
        ASSERT_TRUE(memory.write_u32(kData + L.info_flags, d.info_flags).is_ok());
        if (d.with_guid) {
            for (u32 i = 0; i < 16; ++i) ASSERT_TRUE(memory.write_u8(kData + L.guid + i, kGuid[i]).is_ok());
        }
    }
    u32 notify(u32 message, const Nid& d, bool wide = true) {
        write(d, wide);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(0xDEAD);
        return mgr.notify(m, message, kData, wide);
    }
    std::string res(const char* what, u32 r) {
        char line[160];
        std::snprintf(line, sizeof line, "%s -> %u err=%08x", what, r, get_last_error());
        return line;
    }
    void write_identifier(u32 cb, u32 hwnd, u32 uid, bool guid) {
        for (u32 i = 0; i < 40; ++i) ASSERT_TRUE(memory.write_u8(kIdentifier + i, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kIdentifier, cb).is_ok());
        write_ptr(kIdentifier + (is64() ? 8 : 4), hwnd);
        ASSERT_TRUE(memory.write_u32(kIdentifier + (is64() ? 16 : 8), uid).is_ok());
        if (guid) {
            for (u32 i = 0; i < 16; ++i) {
                ASSERT_TRUE(memory.write_u8(kIdentifier + (is64() ? 20 : 12) + i, kGuid[i]).is_ok());
            }
        }
    }
    u32 identifier_size() const { return is64() ? 40 : 28; }
    u32 get_rect(GuestAddr identifier, GuestAddr rect) {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        return mgr.get_rect(m, identifier, rect);
    }
    std::string rect_of(const char* what) {
        for (u32 i = 0; i < 16; ++i) (void)memory.write_u8(kRect + i, 0);
        const u32 hr = get_rect(kIdentifier, kRect);
        const i32 l = static_cast<i32>(memory.read_u32(kRect).value_or(0));
        const i32 t = static_cast<i32>(memory.read_u32(kRect + 4).value_or(0));
        const i32 r = static_cast<i32>(memory.read_u32(kRect + 8).value_or(0));
        const i32 b = static_cast<i32>(memory.read_u32(kRect + 12).value_or(0));
        char line[160];
        std::snprintf(line, sizeof line, "%s -> hr=%08x nonempty=%d", what, hr, r > l && b > t);
        return line;
    }
    std::vector<std::string> expected(const char* name) {
        const std::string path = std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name +
                                 (is64() ? ".expected" : ".x86.expected");
        std::ifstream in(path);
        EXPECT_TRUE(in.good()) << path;
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        return lines;
    }

    struct Posted {
        u32 hwnd;
        u32 message;
        u64 wparam;
        u64 lparam;
    };

    GuestMemory memory;
    cpu::CpuState cpu;
    NotifyIconManager mgr;
    std::set<u32> windows{kHwnd, kOwner};
    std::vector<Posted> posted;
    std::map<u64, windowing::NotifyIconPresentation> presented;
    std::vector<u64> withdrawn;
    std::vector<std::pair<u64, windowing::NotifyIconBalloon>> balloons;
    std::vector<u64> hidden_balloons;
};

}  // namespace

// notifyicon_tray_probe, row for row.
TEST_P(NotifyIconTest, TrayProbeReplayMatchesWindows) {
    std::vector<std::string> got;
    const auto& W = layout(true);
    const auto& A = layout(false);
    char sizes[160];
    std::snprintf(sizes, sizeof sizes, "sizes W v1=%u v2=%u v3=%u full=%u A v1=%u v2=%u v3=%u full=%u",
                  W.v1_size, W.v2_size, W.v3_size, W.full_size, A.v1_size, A.v2_size, A.v3_size, A.full_size);
    got.emplace_back(sizes);

    Nid d = base(1);
    got.push_back(res("add id1", notify(0, d)));
    write_identifier(identifier_size(), kHwnd, 1, false);
    got.push_back(rect_of("getrect id1"));
    write_identifier(identifier_size(), kHwnd, 99, false);
    got.push_back(rect_of("getrect id99 (never added)"));
    write_identifier(4, kHwnd, 1, false);
    got.push_back(rect_of("getrect cbSize 4"));
    write_identifier(identifier_size(), kHwnd, 1, false);
    got.push_back(rect_of("getrect null rect"));
    {
        char line[96];
        std::snprintf(line, sizeof line, "getrect NULL out -> hr=%08x", get_rect(kIdentifier, 0));
        got.emplace_back(line);
    }
    d.flags = NIF_STATE;
    d.state = NIS_HIDDEN;
    d.state_mask = NIS_HIDDEN;
    got.push_back(res("modify id1 hidden", notify(1, d)));
    got.push_back(rect_of("getrect id1 hidden"));
    d.state = 0;
    got.push_back(res("modify id1 shown", notify(1, d)));
    got.push_back(rect_of("getrect id1 shown"));
    Nid u = base(77);
    u.version = 4;
    got.push_back(res("setversion id77 (never added)", notify(4, u)));
    got.push_back(res("setfocus id77 (never added)", notify(3, u)));
    d = base(1);
    d.version = 3;
    got.push_back(res("setversion id1 3", notify(4, d)));
    d.version = 2;
    got.push_back(res("setversion id1 2", notify(4, d)));
    d.version = 1;
    got.push_back(res("setversion id1 1", notify(4, d)));
    d = base(1);
    got.push_back(res("delete id1", notify(2, d)));
    d = base(2);
    d.flags = 0;
    got.push_back(res("add id2 no flags", notify(0, d)));
    got.push_back(res("delete id2", notify(2, d)));
    d = base(3);
    d.icon = 0;
    got.push_back(res("add id3 NIF_ICON NULL icon", notify(0, d)));
    got.push_back(res("delete id3", notify(2, d)));
    d = base(4);
    d.flags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_STATE;
    d.state = NIS_HIDDEN;
    d.state_mask = NIS_HIDDEN;
    got.push_back(res("add id4 hidden", notify(0, d)));
    write_identifier(identifier_size(), kHwnd, 4, false);
    got.push_back(rect_of("getrect id4 added hidden"));
    got.push_back(res("delete id4", notify(2, d)));
    d = base(5);
    d.hwnd = 0;
    got.push_back(res("add id5 NULL hwnd", notify(0, d)));
    d = base(6);
    d.cb = W.v2_size;
    got.push_back(res("add id6 V2 size", notify(0, d)));
    got.push_back(res("delete id6 V2 size", notify(2, d)));
    d = base(7);
    d.cb = W.v2_size;
    d.flags |= NIF_GUID;
    d.with_guid = true;
    got.push_back(res("add id7 V2 size NIF_GUID", notify(0, d)));
    d.flags &= ~NIF_GUID;
    (void)notify(2, d);
    d = base(8);
    d.cb = W.v3_size;
    got.push_back(res("add id8 V3 size", notify(0, d)));
    got.push_back(res("delete id8 V3 size", notify(2, d)));
    d = base(9);
    d.flags |= NIF_GUID;
    d.with_guid = true;
    got.push_back(res("add guid (uID 9)", notify(0, d)));
    Nid g = base(10);
    g.flags = NIF_TIP | NIF_GUID;
    g.with_guid = true;
    got.push_back(res("modify by guid with uID 10", notify(1, g)));
    g = base(9);
    g.flags = NIF_TIP;
    got.push_back(res("modify uID 9 without guid", notify(1, g)));
    write_identifier(identifier_size(), 0, 0, true);
    got.push_back(rect_of("getrect by guid"));
    write_identifier(identifier_size(), kHwnd, 9, true);
    got.push_back(rect_of("getrect by guid + hwnd/uID"));
    write_identifier(identifier_size(), kHwnd, 9, false);
    got.push_back(rect_of("getrect by hwnd/uID of a guid icon"));
    g = base(9);
    g.flags = NIF_GUID;
    g.with_guid = true;
    got.push_back(res("add guid again", notify(0, g)));
    g.hwnd = 0;
    got.push_back(res("delete by guid, NULL hwnd", notify(2, g)));
    got.push_back(res("delete by guid again", notify(2, g)));
    d = base(11);
    d.cb = W.v1_size;
    d.flags |= NIF_INFO;
    d.info = "text";
    d.title = "title";
    got.push_back(res("add id11 V1 size NIF_INFO", notify(0, d)));
    (void)notify(2, d);
    Nid a = base(12, false);
    a.tip = "ansi";
    got.push_back(res("A add id12", notify(0, a, false)));
    got.push_back(res("A add id12 again", notify(0, a, false)));
    a.cb = A.v1_size;
    got.push_back(res("A modify id12 V1 size", notify(1, a, false)));
    got.push_back(res("A delete id12", notify(2, a, false)));
    got.push_back(res("A delete id12 again", notify(2, a, false)));
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(0xDEAD);
        got.push_back(res("add NULL data", mgr.notify(m, 0, 0, true)));
        set_last_error(0xDEAD);
        got.push_back(res("delete NULL data", mgr.notify(m, 2, 0, true)));
        set_last_error(0xDEAD);
        got.push_back(res("A add NULL data", mgr.notify(m, 0, 0, false)));
        char line[96];
        std::snprintf(line, sizeof line, "getrect NULL identifier -> hr=%08x", get_rect(0, kRect));
        got.emplace_back(line);
    }
    auto rect_line = [&](const char* what) {
        for (u32 i = 0; i < 4; ++i) ASSERT_TRUE(memory.write_u32(kRect + 4 * i, i + 1).is_ok());
        const u32 hr = get_rect(kIdentifier, kRect);
        char line[160];
        std::snprintf(line, sizeof line, "%s -> hr=%08x rect=%d,%d,%d,%d", what, hr,
                      static_cast<i32>(memory.read_u32(kRect).value_or(9)),
                      static_cast<i32>(memory.read_u32(kRect + 4).value_or(9)),
                      static_cast<i32>(memory.read_u32(kRect + 8).value_or(9)),
                      static_cast<i32>(memory.read_u32(kRect + 12).value_or(9)));
        got.emplace_back(line);
    };
    write_identifier(identifier_size(), kHwnd, 98, false);
    rect_line("getrect miss rect");
    d = base(13);
    d.flags |= NIF_STATE;
    d.state = NIS_HIDDEN;
    d.state_mask = NIS_HIDDEN;
    (void)notify(0, d);
    write_identifier(identifier_size(), kHwnd, 13, false);
    rect_line("getrect hidden rect");
    (void)notify(2, d);
    Nid o = base(14);
    o.hwnd = kOwner;
    (void)notify(0, o);
    windows.erase(kOwner);  // DestroyWindow(owner)
    o.flags = NIF_TIP;
    got.push_back(res("modify after owner destroyed", notify(1, o)));
    got.push_back(res("delete after owner destroyed", notify(2, o)));

    std::vector<std::string> want;
    for (const auto& line : expected("notifyicon_tray_probe")) {
        // Explorer's own presence and the registered TaskbarCreated message
        // are not this manager's to answer.
        if (line.rfind("shell tray present", 0) == 0 || line.rfind("TaskbarCreated", 0) == 0) continue;
        want.push_back(line);
    }
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 0; i < want.size(); ++i) EXPECT_EQ(got[i], want[i]) << "row " << i;
}

// notifyicon_click_probe: the callback messages per version. The anchors in
// the probe are Windows' taskbar geometry, so only the events and IDs are
// compared; a version 4 mouse message must carry the pointer as its anchor.
TEST_P(NotifyIconTest, ClickProbeReplayMatchesWindows) {
    auto name_of = [](u32 m) -> std::string {
        switch (m) {
        case 0x200: return "WM_MOUSEMOVE";
        case 0x201: return "WM_LBUTTONDOWN";
        case 0x202: return "WM_LBUTTONUP";
        case 0x203: return "WM_LBUTTONDBLCLK";
        case 0x204: return "WM_RBUTTONDOWN";
        case 0x205: return "WM_RBUTTONUP";
        case 0x7B: return "WM_CONTEXTMENU";
        case 0x400: return "NIN_SELECT";
        case 0x406: return "NIN_POPUPOPEN";
        case 0x407: return "NIN_POPUPCLOSE";
        default: return "?";
        }
    };
    std::vector<std::string> got;
    constexpr i32 kPointerX = 116, kPointerY = 24;
    for (u32 version : {0u, 3u, 4u}) {
        Nid d = base(1);
        ASSERT_EQ(notify(0, d), 1u);
        d.version = version;
        if (version != 0) ASSERT_EQ(notify(4, d), 1u);
        write_identifier(identifier_size(), kHwnd, 1, false);
        char header[96];
        std::snprintf(header, sizeof header, "version %u getrect hr=%08x size=32x48", version,
                      get_rect(kIdentifier, kRect));
        got.emplace_back(header);
        const u64 key = mgr.reopen_target().value_or(0);
        ASSERT_NE(key, 0u);
        auto flush = [&] {
            for (const auto& p : posted) {
                EXPECT_EQ(p.hwnd, kHwnd);
                EXPECT_EQ(p.message, kCallback);
                if (version == 4) {
                    const std::string name = name_of(static_cast<u32>(p.lparam & 0xFFFF));
                    got.push_back("  " + name + " id=" + std::to_string((p.lparam >> 16) & 0xFFFF));
                    if (name.rfind("WM_", 0) == 0 || name == "NIN_SELECT") {
                        EXPECT_EQ(static_cast<i16>(p.wparam & 0xFFFF), kPointerX) << name;
                        EXPECT_EQ(static_cast<i16>((p.wparam >> 16) & 0xFFFF), kPointerY) << name;
                    }
                } else {
                    got.push_back("  wParam=" + std::to_string(p.wparam) + " lParam=" +
                                  name_of(static_cast<u32>(p.lparam)));
                }
            }
            posted.clear();
        };
        auto input = [&](NotifyIconInput in) { mgr.on_input(key, in, kPointerX, kPointerY); };
        got.emplace_back(" left click");
        input(NotifyIconInput::HoverEnter);
        input(NotifyIconInput::LeftDown);
        input(NotifyIconInput::LeftUp);
        input(NotifyIconInput::HoverExit);
        flush();
        got.emplace_back(" right click");
        input(NotifyIconInput::HoverEnter);
        input(NotifyIconInput::RightDown);
        input(NotifyIconInput::RightUp);
        input(NotifyIconInput::HoverExit);
        flush();
        got.emplace_back(" double click");
        input(NotifyIconInput::HoverEnter);
        input(NotifyIconInput::LeftDown);
        input(NotifyIconInput::LeftUp);
        input(NotifyIconInput::LeftDoubleDown);
        input(NotifyIconInput::LeftUp);
        input(NotifyIconInput::HoverExit);
        flush();
        ASSERT_EQ(notify(2, d), 1u);
    }
    std::vector<std::string> want;
    for (const auto& line : expected("notifyicon_click_probe")) {
        if (line.rfind("input desktop", 0) == 0 || line.rfind("promotion key", 0) == 0) continue;
        const auto anchor = line.find(" anchor=");
        want.push_back(anchor == std::string::npos ? line : line.substr(0, anchor));
    }
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 0; i < want.size(); ++i) EXPECT_EQ(got[i], want[i]) << "row " << i;
}

// A Dock click on an app showing no window: the most recently added visible
// icon gets its left click (the click probe's sequence), at the icon.
TEST_P(NotifyIconTest, DockActivationIsTheIconsLeftClick) {
    Nid first = base(1);
    ASSERT_EQ(notify(0, first), 1u);
    Nid second = base(2);
    second.flags |= NIF_STATE;
    second.state = NIS_HIDDEN;
    second.state_mask = NIS_HIDDEN;
    ASSERT_EQ(notify(0, second), 1u);
    const auto key = mgr.reopen_target();
    ASSERT_TRUE(key.has_value());  // the hidden icon is not a target
    first.version = 4;
    ASSERT_EQ(notify(4, first), 1u);
    mgr.on_input(*key, NotifyIconInput::Activate, 0, 0);
    ASSERT_EQ(posted.size(), 4u);
    const u32 events[] = {0x200, 0x201, 0x202, 0x400};
    for (usize i = 0; i < 4; ++i) {
        EXPECT_EQ(posted[i].lparam, events[i] | (1u << 16)) << i;
        EXPECT_EQ(posted[i].wparam, static_cast<u64>(116) | (static_cast<u64>(24) << 16)) << i;
    }
    posted.clear();
    first.version = 0;
    ASSERT_EQ(notify(4, first), 1u);
    mgr.on_input(*key, NotifyIconInput::Activate, 0, 0);
    ASSERT_EQ(posted.size(), 3u);  // no NIN_SELECT before NOTIFYICON_VERSION
    EXPECT_EQ(posted[2].lparam, 0x202u);
    EXPECT_EQ(posted[2].wparam, 1u);
    // An icon without NIF_MESSAGE has nowhere to send a click.
    ASSERT_EQ(notify(2, first), 1u);
    Nid silent = base(3);
    silent.flags = NIF_ICON | NIF_TIP;
    ASSERT_EQ(notify(0, silent), 1u);
    EXPECT_FALSE(mgr.reopen_target().has_value());
}

// Learn (NIF_INFO, NIF_REALTIME, Shell_NotifyIcon remarks): one balloon at a
// time, later ones queued; NIF_REALTIME discards what cannot show now; an
// empty szInfo or NIM_DELETE removes it with NIN_BALLOONHIDE; a timeout
// sends NIN_BALLOONTIMEOUT and shows the next.
TEST_P(NotifyIconTest, BalloonsQueueAndReport) {
    Nid d = base(1);
    ASSERT_EQ(notify(0, d), 1u);
    const u64 key = mgr.reopen_target().value();
    d.flags = NIF_INFO;
    d.info = "first";
    d.title = "Title";
    d.info_flags = 0x02 | 0x10;  // NIIF_WARNING | NIIF_NOSOUND
    ASSERT_EQ(notify(1, d), 1u);
    ASSERT_EQ(balloons.size(), 1u);
    EXPECT_EQ(balloons[0].second.text, "first");
    EXPECT_EQ(balloons[0].second.title, "Title");
    EXPECT_EQ(balloons[0].second.kind, windowing::NotifyIconBalloon::Kind::Warning);
    EXPECT_FALSE(balloons[0].second.sound);
    mgr.on_input(key, NotifyIconInput::BalloonShown, 0, 0);
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(posted[0].lparam, 0x402u);  // NIN_BALLOONSHOW
    EXPECT_EQ(posted[0].wparam, 1u);

    d.info = "second";
    ASSERT_EQ(notify(1, d), 1u);
    EXPECT_EQ(balloons.size(), 1u);  // queued behind the first
    d.flags = NIF_INFO | NIF_REALTIME;
    d.info = "realtime";
    ASSERT_EQ(notify(1, d), 1u);
    mgr.on_input(key, NotifyIconInput::BalloonTimedOut, 0, 0);
    EXPECT_EQ(posted.back().lparam, 0x404u);  // NIN_BALLOONTIMEOUT
    ASSERT_EQ(balloons.size(), 2u);
    EXPECT_EQ(balloons[1].second.text, "second");  // the realtime one was discarded

    d.flags = NIF_INFO;
    d.info.clear();
    ASSERT_EQ(notify(1, d), 1u);
    ASSERT_EQ(hidden_balloons.size(), 1u);
    EXPECT_EQ(posted.back().lparam, 0x403u);  // NIN_BALLOONHIDE

    d.info = "third";
    ASSERT_EQ(notify(1, d), 1u);
    ASSERT_EQ(balloons.size(), 3u);
    ASSERT_EQ(notify(2, d), 1u);
    EXPECT_EQ(hidden_balloons.size(), 2u);
    EXPECT_EQ(posted.back().lparam, 0x403u);
    ASSERT_EQ(withdrawn.size(), 1u);
}

// What the menu bar shows: the icon image, the tooltip (suppressed for a
// version 4 icon without NIF_SHOWTIP), NIS_HIDDEN.
TEST_P(NotifyIconTest, PresentationFollowsTheIcon) {
    Nid d = base(1);
    d.tip = "Spotify";
    ASSERT_EQ(notify(0, d), 1u);
    const u64 key = mgr.reopen_target().value();
    ASSERT_TRUE(presented.count(key));
    EXPECT_EQ(presented[key].tooltip, "Spotify");
    EXPECT_TRUE(presented[key].native_tooltip);
    EXPECT_EQ(presented[key].width, 4u);
    EXPECT_FALSE(presented[key].hidden);
    d.version = 4;
    ASSERT_EQ(notify(4, d), 1u);
    EXPECT_FALSE(presented[key].native_tooltip);
    d.flags = NIF_STATE;
    d.state = NIS_HIDDEN;
    d.state_mask = NIS_HIDDEN;
    ASSERT_EQ(notify(1, d), 1u);
    EXPECT_TRUE(presented[key].hidden);
    ASSERT_EQ(notify(2, d), 1u);
    EXPECT_FALSE(presented.count(key));
}

INSTANTIATE_TEST_SUITE_P(Bitness, NotifyIconTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });
