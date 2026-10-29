/// @file test_owner_draw.cpp
/// @brief BS_OWNERDRAW / SS_OWNERDRAW controls ask their parent to paint them
///        with WM_DRAWITEM (MS Learn: DRAWITEMSTRUCT).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/user32_internal.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <windowing/messages/message_queue.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/user32/windows/draw_item_layout.hpp>
#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

constexpr u32 kWmPaint      = 0x000F;
constexpr u32 kWmDrawItem   = 0x002B;
constexpr u16 kAtomButton   = 0x0080;
constexpr u32 kBsOwnerDraw  = 0x0000000B;
constexpr u32 kBsPushButton = 0x00000000;
constexpr u32 kOdtButton    = 4;

class OwnerDrawTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;

    void SetUp() override {
        if (GetParam()) cpu_.init_long_mode();
        else cpu_.init_protected_mode();
        (void)memory_.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);

        mgr_ = std::make_unique<User32ExtManager>(
            memory_, vfs_, hwnd_mgr_, class_mgr_, bridge_, queue_);
        msgsend_ = std::make_unique<MessageSendManager>(hwnd_mgr_, queue_, bridge_);
        paint_ = std::make_unique<PaintingManager>();

        mgr_->set_message_send_manager(msgsend_.get());
        mgr_->set_painting_manager(paint_.get());

        // Record what the parent's window procedure receives.
        msgsend_->set_wndproc_callback(
            [this](HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64) -> u64 {
                seen_hwnd_ = hwnd;
                seen_msg_ = msg;
                seen_wparam_ = wParam;
                seen_lparam_ = lParam;
                (void)memory_.read_bytes(lParam, seen_drawitem_.data(), seen_drawitem_.size());
                ++seen_count_;
                return 0;
            });

        WindowData parent{};
        parent.parent = HWND_DESKTOP;
        // SendMessage only dispatches to the guest wndproc callback when the
        // window actually has one; wndproc==0 means a host-backed window.
        parent.wndproc = 0x00401000;
        parent.client_rect = {0, 0, 400, 300};
        parent.visible = true;
        parent_ = hwnd_mgr_.create(parent);

        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    HWND make_control(u32 button_style) {
        WindowData child{};
        child.parent = parent_;
        child.class_atom = kAtomButton;
        child.class_name = L"Button";
        child.style = button_style;
        child.ctrl_id = 2003;
        child.client_rect = {0, 0, 212, 99};
        child.visible = true;
        child.enabled = true;
        return hwnd_mgr_.create(child);
    }

    /// Drive CallWindowProcA(kNativeControlWndProc, hwnd, WM_PAINT, 0, 0),
    /// which is the path a subclassing app takes to reach the class proc.
    void call_native_wndproc(HWND hwnd, u32 msg) {
        DllExports exports = create_user32_ext_thunks(*mgr_);
        ExportEntry entry{};
        for (const auto& e : exports.entries) {
            if (e.name == "CallWindowProcA") { entry = e; break; }
        }
        ASSERT_TRUE(static_cast<bool>(entry.handler));

        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        if (GetParam()) {
            cpu_.gpr.rcx = kNativeControlWndProc;
            cpu_.gpr.rdx = static_cast<u64>(hwnd);
            cpu_.gpr.r8  = msg;
            cpu_.gpr.r9  = 0;
            (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, 0); // lParam
        } else {
            const u32 args[] = {static_cast<u32>(kNativeControlWndProc), hwnd, msg, 0, 0};
            for (u32 i = 0; i < 5; ++i)
                (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, args[i]);
        }
        entry.handler(m);
    }

    GuestMemory        memory_;
    capsule::Vfs       vfs_;
    HwndManager        hwnd_mgr_;
    WindowClassManager class_mgr_;
    CocoaBridge        bridge_;
    MessageQueue       queue_;
    std::unique_ptr<User32ExtManager>   mgr_;
    std::unique_ptr<MessageSendManager> msgsend_;
    std::unique_ptr<PaintingManager>    paint_;
    cpu::CpuState      cpu_;
    HWND               parent_ = 0;

    HWND seen_hwnd_ = 0;
    u32  seen_msg_ = 0;
    u64  seen_wparam_ = 0;
    u64  seen_lparam_ = 0;
    int  seen_count_ = 0;
    std::array<u8, 64> seen_drawitem_{};
};

TEST_P(OwnerDrawTest, OwnerDrawButtonSendsDrawItemToParent) {
    const HWND ctrl = make_control(kBsOwnerDraw);
    call_native_wndproc(ctrl, kWmPaint);

    ASSERT_EQ(seen_count_, 1) << "parent received no WM_DRAWITEM";
    EXPECT_EQ(seen_hwnd_, parent_);
    EXPECT_EQ(seen_msg_, kWmDrawItem);
    // wParam is the control id; lParam points at the DRAWITEMSTRUCT.
    EXPECT_EQ(seen_wparam_, 2003u);
    ASSERT_NE(seen_lparam_, 0u);

    // Offsets generated from the SDK declaration compiled on Windows, not Vitreo.
    const auto& layout = GetParam() ? vitreo::winapi::detail::draw_item_layout64
                                    : vitreo::winapi::detail::draw_item_layout32;
    const auto read = [&](u32 offset, u32 count = 4) {
        u64 value = 0;
        for (u32 i = 0; i < count; ++i) value |= static_cast<u64>(seen_drawitem_[offset + i]) << (i * 8);
        return value;
    };
    EXPECT_EQ(read(layout.CtlType), kOdtButton);
    EXPECT_EQ(read(layout.CtlID), 2003u);
    EXPECT_EQ(read(layout.itemID), 0u);
    EXPECT_EQ(read(layout.itemAction), 1u);
    EXPECT_EQ(read(layout.itemState), 0u);
    EXPECT_EQ(read(layout.hwndItem, GetParam() ? 8 : 4), ctrl);
    EXPECT_NE(read(layout.hDC, GetParam() ? 8 : 4), 0u);
    EXPECT_EQ(read(layout.rcItem_left), 0u);
    EXPECT_EQ(read(layout.rcItem_top), 0u);
    EXPECT_EQ(read(layout.rcItem_right), 212u);
    EXPECT_EQ(read(layout.rcItem_bottom), 99u);
    EXPECT_EQ(read(layout.itemData, GetParam() ? 8 : 4), 0u);
}

TEST_P(OwnerDrawTest, OrdinaryButtonDoesNotSendDrawItem) {
    const HWND ctrl = make_control(kBsPushButton);
    call_native_wndproc(ctrl, kWmPaint);
    EXPECT_EQ(seen_count_, 0) << "non-owner-draw button must not draw via parent";
}

TEST_P(OwnerDrawTest, NestedDrawItemKeepsOuterCallbackStorage) {
    const HWND outer = make_control(kBsOwnerDraw);
    const HWND inner = make_control(kBsOwnerDraw);
    bool inside = false;
    GuestAddr outer_address = 0;
    int deliveries = 0;
    msgsend_->set_wndproc_callback(
        [&](HWND, u32 msg, u64, u64 param, u64) -> u64 {
            EXPECT_EQ(msg, kWmDrawItem);
            ++deliveries;
            if (inside) {
                EXPECT_NE(param, outer_address);
                return 0;
            }
            outer_address = param;
            std::array<u8, 64> before{}, after{};
            EXPECT_TRUE(memory_.read_bytes(param, before.data(), before.size()).is_ok());
            inside = true;
            call_native_wndproc(inner, kWmPaint);
            EXPECT_TRUE(memory_.read_bytes(param, after.data(), after.size()).is_ok());
            EXPECT_EQ(before, after);
            return 0;
        });
    call_native_wndproc(outer, kWmPaint);
    EXPECT_EQ(deliveries, 2);
}

TEST_P(OwnerDrawTest, NativeControlClassCasingPreservesCallableBaseProcedure) {
    // Windows native_control_case_probe{32,64}.expected: every casing creates
    // the same class and Get/SetWindowLongPtr returns its callable base proc.
    for (const auto* name : {L"Button", L"button", L"BUTTON", L"Edit", L"EDIT",
                             L"RichEdit50W", L"RICHEDIT50W", L"richedit50w"}) {
        WindowData control{};
        control.class_name = name;
        EXPECT_TRUE(is_native_host_control(control));
        EXPECT_NE(effective_wndproc(control), 0u);
        control.wndproc = 0x401000;
        EXPECT_FALSE(is_native_host_control(control));
        EXPECT_EQ(effective_wndproc(control), 0x401000u);
    }
    for (const auto* name : {L"ButtonSuffix", L"Ed\u0131t", L"\u017Ftatic"}) {
        WindowData custom{};
        custom.class_name = name;
        EXPECT_FALSE(is_native_host_control(custom));
        EXPECT_EQ(effective_wndproc(custom), 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(GuestBitness, OwnerDrawTest, ::testing::Bool());

