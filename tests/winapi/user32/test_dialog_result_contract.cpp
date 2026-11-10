// Original Windows probe supplies all expected result bit patterns.
#include <gtest/gtest.h>
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "capsule/vfs/vfs.hpp"
#include "../../oracle_data/dialog_result.generated.inc"
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
using namespace vitreo;
using namespace vitreo::winapi;
namespace facts = dialog_result_facts;

class DialogResultContract : public ::testing::Test {
protected:
    static constexpr GuestAddr image = 0x400000;
    static constexpr GuestAddr stack = 0x10000;
    static constexpr u64 proc = 0x123456;
    GuestMemory memory;
    capsule::Vfs vfs;
    windowing::HwndManager windows;
    WindowClassManager classes;
    windowing::CocoaBridge bridge;
    windowing::MessageQueue queue;
    DialogManager dialogs{windows, classes, bridge, queue, memory};
    User32ExtManager extension{memory, vfs, windows, classes, bridge, queue,
                               [] { return std::string{"test GPU"}; }};
    DllExports direct;
    DllExports indirect;
    GuestAddr template_address = 0;

    void SetUp() override {
        ThreadManager::set_current_thread_id(1);
        ASSERT_TRUE(memory.map_region(stack, 0x4000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(image, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.write_bytes(image + facts::resource_rva,
                    facts::resource.data(), facts::resource.size()).is_ok());
        dialogs.set_resource_info(image, facts::resource_rva, facts::resource.size());
        const auto resource = windowing::find_resource(memory, image, facts::resource_rva,
                              facts::resource.size(), windowing::RT::DIALOG, facts::resource_id);
        ASSERT_NE(resource.data, nullptr);
        template_address = resource.guest_addr;
        extension.set_dialog_manager(&dialogs);
        direct = create_dialog_thunks(dialogs);
        indirect = create_user32_ext_thunks(extension);
    }

    void invoke(const ExportEntry& entry, bool is64, const std::array<u64, 5>& args,
                cpu::CpuState& state) {
        if (is64) {
            state.init_long_mode();
            state.gpr.rcx = args[0]; state.gpr.rdx = args[1];
            state.gpr.r8 = args[2]; state.gpr.r9 = args[3];
        } else state.init_protected_mode();
        state.set_sp(stack + 0x1000);
        if (is64) {
            ASSERT_TRUE(memory.write_u64(state.sp() + 5 * sizeof(u64), args[4]).is_ok());
        } else {
            for (usize i = 0; i < args.size(); ++i)
                ASSERT_TRUE(memory.write_u32(state.sp() + (i + 1) * sizeof(u32), args[i]).is_ok());
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(state);
        entry.handler(marshaler);
    }

    void check(const facts::Row& row, bool use_indirect) {
        const std::string name = std::string(use_indirect ? "DialogBoxIndirectParam" : "DialogBoxParam")
                               + (row.wide ? "W" : "A");
        SCOPED_TRACE(name + " abi=" + (row.is64 ? "64" : "32") + " value=" + std::to_string(row.sent));
        const auto& exports = use_indirect ? indirect : direct;
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                         [&](const auto& item) { return item.name == name; });
        ASSERT_NE(entry, exports.entries.end());
        const auto end = std::find_if(direct.entries.begin(), direct.entries.end(),
                         [](const auto& item) { return item.name == "EndDialog"; });
        ASSERT_NE(end, direct.entries.end());
        bool initialized = false;
        windowing::HWND created = 0;
        dialogs.set_dlgproc_callback([&](windowing::HWND hwnd, u32 message, u64, u64 lparam, u64 callback) -> u64 {
            if (message == DialogManager::WM_INITDIALOG) {
                initialized = true;
                created = hwnd;
                EXPECT_EQ(callback, proc);
                EXPECT_EQ(lparam, row.sent);
                cpu::CpuState end_state;
                invoke(*end, row.is64, {hwnd, lparam, 0, 0, 0}, end_state);
                EXPECT_EQ(end_state.gpr.rax, row.end_ok);
                EXPECT_TRUE(windows.is_valid(hwnd)); // EndDialog destruction is deferred.
            }
            return 1;
        });
        cpu::CpuState state;
        invoke(*entry, row.is64, {image, use_indirect ? template_address : facts::resource_id,
                                  0, proc, row.sent}, state);
        ASSERT_TRUE(initialized);
        EXPECT_FALSE(windows.is_valid(created));
        EXPECT_EQ(state.gpr.rax, row.result);
        dialogs.set_dlgproc_callback({});
    }
};

TEST_F(DialogResultContract, DirectAnsiUnicodePreserveWindowsIntPtrResultsInBothAbis) {
    for (const auto& row : facts::rows) check(row, false);
}
TEST_F(DialogResultContract, IndirectAliasesShareIntPtrRoundtripInBothAbis) {
    // Learn specifies the same EndDialog INT_PTR return for indirect APIs.
    for (const auto& row : facts::rows) check(row, true);
}

// Learn GetMessage / s2msg_queue_probe "order": once posted and input
// messages are drained, WM_PAINT for a window with an update region comes
// before WM_TIMER. DialogBox's modal loop retrieves the same way, so a
// dialog that invalidates itself on each timer tick is painted.
TEST_F(DialogResultContract, ModalLoopDeliversPaintBeforeADueTimer) {
    PaintingManager paint{&windows};
    dialogs.set_painting_manager(&paint);
    dialogs.set_user32_ext_manager(&extension);
    const auto entry = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "DialogBoxParamW"; });
    const auto set_timer = std::find_if(indirect.entries.begin(), indirect.entries.end(),
                     [](const auto& item) { return item.name == "SetTimer"; });
    const auto end = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "EndDialog"; });
    ASSERT_NE(entry, direct.entries.end());
    ASSERT_NE(set_timer, indirect.entries.end());
    ASSERT_NE(end, direct.entries.end());
    std::vector<u32> seen;
    dialogs.set_dlgproc_callback([&](windowing::HWND hwnd, u32 message, u64, u64, u64) -> u64 {
        if (message == DialogManager::WM_INITDIALOG) {
            cpu::CpuState timer_state;
            invoke(*set_timer, false, {hwnd, 1, 10, 0, 0}, timer_state);
            EXPECT_NE(timer_state.gpr.rax, 0u);
            return 1;
        }
        if (message != windowing::WM::PAINT && message != windowing::WM::TIMER) return 0;
        seen.push_back(message);
        const bool first_timer =
            message == windowing::WM::TIMER &&
            std::count(seen.begin(), seen.end(), windowing::WM::TIMER) == 1;
        if (first_timer) {
            // The visible dialog invalidates itself, and the timer is due
            // again before the loop retrieves its next message.
            EXPECT_TRUE(paint.invalidate_rect(hwnd, nullptr, false));
            EXPECT_TRUE(paint.needs_paint(hwnd));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        } else if (message == windowing::WM::PAINT) {
            EXPECT_TRUE(paint.needs_paint(hwnd));  // pending until validated
        }
        if (seen.size() == 4) {
            cpu::CpuState end_state;
            invoke(*end, false, {hwnd, 7, 0, 0, 0}, end_state);
        }
        return 0;
    });
    cpu::CpuState state;
    invoke(*entry, false, {image, facts::resource_id, 0, proc, 0}, state);
    // Shown (whole client invalid), the first tick, the paint it asked for
    // ahead of the due timer, then that timer: each WM_PAINT was validated
    // after dispatch, so it is not retrieved again.
    const std::vector<u32> expected{windowing::WM::PAINT, windowing::WM::TIMER,
                                    windowing::WM::PAINT, windowing::WM::TIMER};
    EXPECT_EQ(seen, expected);
    EXPECT_EQ(state.gpr.rax, 7u);
    dialogs.set_dlgproc_callback({});
    dialogs.set_painting_manager(nullptr);
}

// ShowWindow contract: a window that becomes visible has its whole client in
// its update region. DialogBox shows the dialog after WM_INITDIALOG, so its
// first retrieval with a due timer is still WM_PAINT, over the whole client.
TEST_F(DialogResultContract, ShownModalDialogIsPaintedWholeFirst) {
    PaintingManager paint{&windows};
    dialogs.set_painting_manager(&paint);
    dialogs.set_user32_ext_manager(&extension);
    const auto entry = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "DialogBoxParamW"; });
    const auto set_timer = std::find_if(indirect.entries.begin(), indirect.entries.end(),
                     [](const auto& item) { return item.name == "SetTimer"; });
    const auto end = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "EndDialog"; });
    ASSERT_NE(entry, direct.entries.end());
    ASSERT_NE(set_timer, indirect.entries.end());
    ASSERT_NE(end, direct.entries.end());
    std::vector<u32> seen;
    bool whole_client = false;
    dialogs.set_dlgproc_callback([&](windowing::HWND hwnd, u32 message, u64, u64, u64) -> u64 {
        if (message == DialogManager::WM_INITDIALOG) {
            cpu::CpuState timer_state;
            invoke(*set_timer, false, {hwnd, 1, 10, 0, 0}, timer_state);
            EXPECT_FALSE(paint.needs_paint(hwnd));  // still hidden
            std::this_thread::sleep_for(std::chrono::milliseconds(30));  // timer due
            return 1;
        }
        if (message != windowing::WM::PAINT && message != windowing::WM::TIMER) return 0;
        seen.push_back(message);
        if (message == windowing::WM::PAINT) {
            const windowing::WindowData* w = windows.find(hwnd);
            if (w == nullptr) return 0;
            const Rect client{0, 0, w->client_rect.right - w->client_rect.left,
                              w->client_rect.bottom - w->client_rect.top};
            Rect update{};
            whole_client = paint.get_update_rect(hwnd, update, false) &&
                           update.left == client.left && update.top == client.top &&
                           update.right == client.right && update.bottom == client.bottom;
        }
        cpu::CpuState end_state;
        invoke(*end, false, {hwnd, 3, 0, 0, 0}, end_state);
        return 0;
    });
    cpu::CpuState state;
    invoke(*entry, false, {image, facts::resource_id, 0, proc, 0}, state);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen[0], windowing::WM::PAINT);
    EXPECT_TRUE(whole_client);
    dialogs.set_dlgproc_callback({});
    dialogs.set_painting_manager(nullptr);
}

// The modal loop polls its exit predicate (ExitProcess / a queued chain-load)
// before each retrieval: once it turns true inside a dispatch, no further
// message reaches the dialog and DialogBox returns.
TEST_F(DialogResultContract, ModalLoopStopsOnceTheProcessIsExiting) {
    bool exiting = false;
    dialogs.set_modal_exit_predicate([&] { return exiting; });
    const auto entry = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "DialogBoxParamW"; });
    ASSERT_NE(entry, direct.entries.end());
    int after_exit = 0;
    dialogs.set_dlgproc_callback([&](windowing::HWND hwnd, u32 message, u64, u64, u64) -> u64 {
        if (message == DialogManager::WM_INITDIALOG) {
            windowing::MSG first{};
            first.hwnd = hwnd;
            first.message = 0x8001;
            queue.post(first);
            queue.post(first);
            return 1;
        }
        if (message == 0x8001) {
            if (exiting) ++after_exit;
            exiting = true;  // ExitProcess from inside the dialog procedure
        }
        return 0;
    });
    cpu::CpuState state;
    invoke(*entry, false, {image, facts::resource_id, 0, proc, 0}, state);
    EXPECT_EQ(after_exit, 0);
    dialogs.set_dlgproc_callback({});
    dialogs.set_modal_exit_predicate({});
}

// tools/oracle_probe/enddialog_nested_probe{32,64}.expected: EndDialog hides
// the dialog and re-enables its owner at once; a DialogBox opened from the
// same DLGPROC call runs; EndDialog on the outer dialog from inside it does
// not end the nested loop, and the outer DialogBox returns the latest result.
TEST_F(DialogResultContract, EndDialogHidesAndEndsOnlyItsOwnNestedLoop) {
    const auto entry = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "DialogBoxParamW"; });
    const auto end = std::find_if(direct.entries.begin(), direct.entries.end(),
                     [](const auto& item) { return item.name == "EndDialog"; });
    ASSERT_NE(entry, direct.entries.end());
    ASSERT_NE(end, direct.entries.end());
    windowing::WindowData owner_data;
    owner_data.title = L"owner";
    owner_data.visible = true;
    owner_data.enabled = true;
    const windowing::HWND owner = windows.create(owner_data);
    constexpr u32 kStep = 0x8001;
    windowing::HWND outer = 0, nested = 0;
    bool outer_hidden = false, owner_enabled = false, outer_hidden_in_nested = false;
    bool nested_ran_after_outer_end = false;
    u64 nested_result = 0;
    auto post = [&](windowing::HWND h, u64 w) {
        windowing::MSG msg{};
        msg.hwnd = h;
        msg.message = kStep;
        msg.wParam = w;
        queue.post(msg);
    };
    auto end_dialog = [&](windowing::HWND h, u32 result) {
        cpu::CpuState s;
        invoke(*end, false, {h, result, 0, 0, 0}, s);
    };
    dialogs.set_dlgproc_callback([&](windowing::HWND hwnd, u32 message, u64 wparam, u64, u64) -> u64 {
        if (message == DialogManager::WM_INITDIALOG) {
            if (outer == 0) {
                outer = hwnd;
                post(hwnd, 0);
            } else {
                nested = hwnd;
                outer_hidden_in_nested = !windows.find(outer)->visible;
                post(hwnd, 1);
            }
            return 1;
        }
        if (message != kStep) return 0;
        if (hwnd == outer && wparam == 0) {
            end_dialog(outer, 5);
            outer_hidden = !windows.find(outer)->visible;
            owner_enabled = windows.find(owner)->enabled;
            cpu::CpuState s;
            invoke(*entry, false, {image, facts::resource_id, owner, proc, 0}, s);
            nested_result = s.gpr.rax;
        } else if (hwnd == nested && wparam == 1) {
            end_dialog(outer, 9);
            post(nested, 2);
        } else if (hwnd == nested && wparam == 2) {
            nested_ran_after_outer_end = true;
            end_dialog(nested, 3);
        }
        return 0;
    });
    cpu::CpuState state;
    invoke(*entry, false, {image, facts::resource_id, owner, proc, 0}, state);
    EXPECT_TRUE(outer_hidden);                // outer.enddialog visible=0
    EXPECT_TRUE(owner_enabled);               // owner_enabled=1
    EXPECT_TRUE(outer_hidden_in_nested);      // nested.init outer_visible=0
    EXPECT_TRUE(nested_ran_after_outer_end);  // nested_ran_after_outer_end=1
    EXPECT_EQ(nested_result, 3u);             // nested.result=3
    EXPECT_EQ(state.gpr.rax, 9u);             // outer.result=9
    EXPECT_FALSE(windows.is_valid(outer));    // outer_iswindow_after=0
    EXPECT_TRUE(windows.find(owner)->enabled);
    dialogs.set_dlgproc_callback({});
    windows.destroy(owner);
}
