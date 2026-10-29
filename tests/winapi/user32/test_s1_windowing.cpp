/// @file test_s1_windowing.cpp
/// @brief S1 (Steam API audit, windowing) contract tests.
///
/// Every expected value comes from a Windows 11 measurement committed under
/// tools/oracle_probe/ (s1win_api_probe.expected, s1win_class_probe.expected,
/// s1win_style_probe.expected, s1win_msgseq_probe.expected) or from the
/// Microsoft Learn page cited next to it. The calls go through the exported
/// thunks, the way a guest reaches them.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <winapi/user32/windows/window.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/user32/windows/show_window.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

namespace {

constexpr u32 kErrInvalidWindowHandle = 1400;
constexpr u32 kErrInvalidMenuHandle = 1401;
constexpr u32 kErrTlwWithWsChild = 1406;
constexpr u32 kErrCannotFindWndClass = 1407;
constexpr u32 kErrClassAlreadyExists = 1410;
constexpr u32 kErrClassDoesNotExist = 1411;
constexpr u32 kErrClassHasWindows = 1412;
constexpr u32 kErrInvalidIndex = 1413;
constexpr u32 kErrControlIdNotFound = 1421;
constexpr u32 kErrInvalidParameter = 87;
constexpr u32 kErrInvalidName = 123;
constexpr u32 kErrInsufficientBuffer = 122;
constexpr u32 kErrFileNotFound = 2;
constexpr u32 kUntouched = 0xDEAD;

constexpr u32 kWmStyleChanging = 0x007C;
constexpr u32 kWmStyleChanged = 0x007D;

} // namespace

class S1WindowingTest : public ::testing::Test {
public:
    static constexpr GuestAddr kData = 0x10000;
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kScratch = 0x100000;
    static constexpr u64 kInstance = 0x140000000ull;
    static constexpr u64 kProc = 0x401000;

    struct Sent {
        HWND hwnd;
        u32 msg;
        u64 wparam;
        u32 style_old;
        u32 style_new;
        u64 lparam = 0;
        /// WINDOWPOS.flags (WM_WINDOWPOSCHANGING/CHANGED), or the stored
        /// window style when the message arrived.
        u32 swp_flags = 0;
        u32 live_style = 0;
        /// WM_NCCALCSIZE's first RECT.
        i32 r0[4] = {0, 0, 0, 0};
    };

protected:

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kData, 0x20000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kScratch, 0x60000, MemoryProtection::ReadWrite).is_ok());
        class_mgr_.set_executable_instance_provider([] { return kInstance; });
        class_mgr_.set_class_in_use_query(
            [this](u32 id) { return hwnd_mgr_.any_window_of_class(id); });
        mgr_ = std::make_unique<User32ExtManager>(memory_, vfs_, hwnd_mgr_, class_mgr_,
                                                  bridge_, queue_);
        mgr_->set_is_64bit(true);
        mgr_->set_windowpos_scratch(kScratch + 0x2000);
        mgr_->set_thread_scratch(kScratch + 0x10000, 0x40000);
        wm_ = std::make_unique<WindowManager>(hwnd_mgr_, class_mgr_, bridge_, queue_);
        wm_->set_createstruct_scratch(kScratch);
        wm_->set_menu_validator([this](u64 h) { return mgr_->is_menu_handle(static_cast<u32>(h)); });
        send_ = std::make_unique<MessageSendManager>(hwnd_mgr_, queue_, bridge_);
        auto proc = [this](HWND h, u32 msg, u64 w, u64 l, u64) -> u64 {
            Sent s{h, msg, w, 0, 0};
            s.lparam = l;
            if (const WindowData* live = hwnd_mgr_.find(h)) s.live_style = live->style;
            if (msg == kWmStyleChanging || msg == kWmStyleChanged) {
                s.style_old = memory_.read_u32(l).value_or(0);
                s.style_new = memory_.read_u32(l + 4).value_or(0);
                if (msg == kWmStyleChanging && edit_style_) {
                    (void)memory_.write_u32(l + 4, s.style_new | edit_style_);
                }
            }
            if (msg == WM::WINDOWPOSCHANGING || msg == WM::WINDOWPOSCHANGED) {
                s.swp_flags = memory_.read_u32(l + 32).value_or(0);   // x64 WINDOWPOS
            }
            if (msg == WM::NCCREATE || msg == WM::CREATE) {
                s.style_old = memory_.read_u32(l + 48).value_or(0);   // CREATESTRUCTW.style
                s.style_new = memory_.read_u32(l + 72).value_or(0);   // .dwExStyle
            }
            if (msg == WM::NCCALCSIZE) {
                for (int i = 0; i < 4; ++i) {
                    s.r0[i] = static_cast<i32>(memory_.read_u32(l + 4 * i).value_or(0));
                }
            }
            sent_.push_back(s);
            return msg == 0x0081 /*WM_NCCREATE*/ ? 1u : 0u;
        };
        send_->set_wndproc_callback(proc);
        wm_->set_wndproc_callback(proc);
        mgr_->set_message_send_manager(send_.get());
        // As runtime_context.cpp wires them: CreateWindowEx / DestroyWindow
        // show, hide and change state through the SetWindowPos engine.
        WindowOps ops;
        ops.show = [this](HWND h, int cmd) { return mgr_->show_window_cmd(h, cmd); };
        ops.set_pos = [this](HWND h, i32 x, i32 y, i32 cx, i32 cy, u32 flags) {
            (void)mgr_->apply_set_window_pos(h, HWND_TOP, x, y, cx, cy, flags, true);
        };
        ops.initial_state = [this](HWND h, bool minimize) {
            mgr_->apply_initial_state(h, minimize);
        };
        wm_->set_window_ops(std::move(ops));

        show_ = std::make_unique<ShowWindowManager>(hwnd_mgr_, bridge_, queue_);
        show_->set_user32_ext_manager(mgr_.get());
        paint_ = std::make_unique<PaintingManager>(&hwnd_mgr_);
        class_exports_ = make_window_class_exports(class_mgr_);
        window_exports_ = make_window_exports(*wm_);
        show_exports_ = make_show_window_exports(*show_);
        paint_exports_ = create_painting_thunks(*paint_);
        ext_exports_ = create_user32_ext_thunks(*mgr_);
        // The registry keeps the LAST entry of a name (merge order in
        // dll_registry.cpp: classes, windows, show, ..., painting, ...,
        // user32_ext).
        for (auto* table : {&class_exports_, &window_exports_, &show_exports_,
                            &paint_exports_, &ext_exports_}) {
            for (auto& e : table->entries) exports_[e.name] = &e;
        }
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = kStack + 0x800;
        m_ = std::make_unique<Marshaler>(memory_);
        m_->attach_cpu(cpu_);
    }

    void use_x86() {
        cpu_.mode = cpu::CpuMode::Protected;
        mgr_->set_is_64bit(false);
    }

    /// Call an export the way a guest does. Returns RAX (EAX on x86).
    u64 call(const std::string& name, std::vector<u64> args, u32 prime = kUntouched) {
        auto it = exports_.find(name);
        EXPECT_NE(it, exports_.end()) << name;
        if (it == exports_.end()) return 0;
        set_last_error(prime);
        if (cpu_.mode == cpu::CpuMode::Long) {
            u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + (i - 4) * 8, args[i]);
            }
        } else {
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory_.write_u32(static_cast<GuestAddr>(cpu_.sp() + 4 + i * 4),
                                        static_cast<u32>(args[i]));
            }
        }
        it->second->handler(*m_);
        return cpu_.mode == cpu::CpuMode::Long ? cpu_.gpr.rax
                                               : (cpu_.gpr.rax & 0xFFFFFFFFull);
    }

    GuestAddr wstr(const std::wstring& s) {
        const GuestAddr at = kData + next_;
        for (usize i = 0; i < s.size(); ++i) {
            (void)memory_.write_u16(at + i * 2, static_cast<u16>(s[i]));
        }
        (void)memory_.write_u16(at + s.size() * 2, 0);
        next_ += static_cast<u32>((s.size() + 1) * 2 + 15) & ~15u;
        return at;
    }

    GuestAddr buffer(u32 size, u8 fill = 0) {
        const GuestAddr at = kData + next_;
        std::vector<u8> bytes(size, fill);
        (void)memory_.write_bytes(at, bytes.data(), bytes.size());
        next_ += (size + 15) & ~15u;
        return at;
    }

    /// RegisterClassExW with a guest WNDCLASSEXW (x64 layout).
    u64 register_class(const std::wstring& name, u64 hinstance, u32 style = 0,
                       i32 cls_extra = 0, i32 wnd_extra = 0, u64 wndproc = kProc) {
        const GuestAddr wc = buffer(80);
        (void)memory_.write_u32(wc + 0, 80);
        (void)memory_.write_u32(wc + 4, style);
        (void)memory_.write_u64(wc + 8, wndproc);
        (void)memory_.write_u32(wc + 16, static_cast<u32>(cls_extra));
        (void)memory_.write_u32(wc + 20, static_cast<u32>(wnd_extra));
        (void)memory_.write_u64(wc + 24, hinstance);
        (void)memory_.write_u64(wc + 64, wstr(name));
        return call("RegisterClassExW", {wc});
    }

    HWND create(const std::wstring& cls, u32 style, u32 ex = 0, HWND parent = 0,
                u64 menu = 0, u64 hinstance = kInstance) {
        return static_cast<HWND>(call("CreateWindowExW",
            {ex, wstr(cls), wstr(L""), style, 10, 20, 100, 80, parent, menu, hinstance, 0}));
    }

    GuestMemory memory_;
    capsule::Vfs vfs_;
    HwndManager hwnd_mgr_;
    WindowClassManager class_mgr_;
    CocoaBridge bridge_;
    MessageQueue queue_;
    std::unique_ptr<User32ExtManager> mgr_;
    std::unique_ptr<WindowManager> wm_;
    std::unique_ptr<MessageSendManager> send_;
    std::unique_ptr<ShowWindowManager> show_;
    std::unique_ptr<PaintingManager> paint_;
    DllExports class_exports_, window_exports_, show_exports_, paint_exports_, ext_exports_;
    std::map<std::string, ExportEntry*> exports_;
    cpu::CpuState cpu_;
    std::unique_ptr<Marshaler> m_;
    std::vector<Sent> sent_;
    u32 edit_style_ = 0;
    u32 next_ = 0;
};

// ---------------------------------------------------------------------------
// Classes (s1win_api_probe "-- classes", s1win_class_probe)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, RegisterClassErrorsAreMeasuredCodes) {
    EXPECT_GE(register_class(L"S1Cls", kInstance), 0xC000u);
    EXPECT_EQ(register_class(L"S1Cls", kInstance), 0u);
    EXPECT_EQ(get_last_error(), kErrClassAlreadyExists);
    EXPECT_EQ(register_class(L"s1cls", kInstance), 0u);   // case-insensitive
    EXPECT_EQ(get_last_error(), kErrClassAlreadyExists);
    EXPECT_EQ(register_class(L"S1Neg", kInstance, 0, -1), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(register_class(L"", kInstance), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidName);
    // No validation of lpfnWndProc (registerclassexw_null_wndproc).
    EXPECT_NE(register_class(L"S1NullProc", kInstance, 0, 0, 0, 0), 0u);
    // A local class may shadow a system class.
    EXPECT_NE(register_class(L"Button", kInstance), 0u);
}

TEST_F(S1WindowingTest, SameClassNameInTwoModulesAndInstanceLookup) {
    constexpr u64 kOther = 0x7FF800000000ull;
    const u64 a1 = register_class(L"S1Dup", kInstance, 0, 0, 0, 0x1111);
    const u64 a2 = register_class(L"S1Dup", kOther, 0, 0, 0, 0x2222);
    EXPECT_NE(a2, 0u);
    EXPECT_EQ(a1, a2);   // one atom per name
    const HWND w1 = create(L"S1Dup", WS::POPUP, 0, 0, 0, kInstance);
    const HWND w2 = create(L"S1Dup", WS::POPUP, 0, 0, 0, kOther);
    EXPECT_EQ(hwnd_mgr_.find(w1)->wndproc, 0x1111u);
    EXPECT_EQ(hwnd_mgr_.find(w2)->wndproc, 0x2222u);
    // A local class is not found for another module's instance.
    register_class(L"S1Local", kInstance);
    EXPECT_EQ(create(L"S1Local", WS::POPUP, 0, 0, 0, kOther), 0u);
    EXPECT_EQ(get_last_error(), kErrCannotFindWndClass);
    EXPECT_NE(create(L"S1Local", WS::POPUP, 0, 0, 0, 0), 0u);
}

TEST_F(S1WindowingTest, GetClassInfoExReportsRegistrationAndMeasuredErrors) {
    const u64 atom = register_class(L"S1Info", kInstance, CS::HREDRAW | CS::DBLCLKS, 8, 4);
    const GuestAddr out = buffer(80, 0xAB);
    (void)memory_.write_u32(out, 80);
    EXPECT_EQ(call("GetClassInfoExW", {kInstance, wstr(L"S1Info"), out}), atom);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(memory_.read_u32(out + 4).value(), 0x0Au);        // style
    EXPECT_EQ(memory_.read_u32(out + 16).value(), 8u);          // cbClsExtra
    EXPECT_EQ(memory_.read_u32(out + 20).value(), 4u);          // cbWndExtra
    EXPECT_EQ(memory_.read_u64(out + 24).value(), kInstance);   // hInstance
    // cbSize is neither checked nor written (getclassinfoexw_zero_cbsize_ok).
    (void)memory_.write_u32(out, 0);
    EXPECT_EQ(call("GetClassInfoExW", {kInstance, wstr(L"S1Info"), out}), atom);
    EXPECT_EQ(memory_.read_u32(out).value(), 0u);
    // A NULL instance names only global and system classes.
    EXPECT_EQ(call("GetClassInfoExW", {0, wstr(L"S1Info"), out}), 0u);
    EXPECT_EQ(get_last_error(), kErrClassDoesNotExist);
    const GuestAddr untouched = buffer(80, 0xAB);
    EXPECT_EQ(call("GetClassInfoExW", {kInstance, wstr(L"S1Missing"), untouched}), 0u);
    EXPECT_EQ(get_last_error(), kErrClassDoesNotExist);
    EXPECT_EQ(memory_.read_u32(untouched + 4).value(), 0xABABABABu);
    // System classes: measured registrations, hInstance NULL, found with any
    // instance; the dialog class is integer atom 0x8002.
    EXPECT_NE(call("GetClassInfoExW", {0, wstr(L"Button"), out}), 0u);
    EXPECT_EQ(memory_.read_u32(out + 4).value(), 0x8Bu);
    EXPECT_EQ(memory_.read_u32(out + 20).value(), 8u);
    EXPECT_EQ(memory_.read_u64(out + 24).value(), 0u);
    EXPECT_EQ(call("GetClassInfoExW", {0, 0x8002, out}), 0x8002u);
    EXPECT_EQ(memory_.read_u32(out + 20).value(), 30u);
    EXPECT_EQ(memory_.read_u32(out + 4).value(), 0x808u);
}

TEST_F(S1WindowingTest, UnregisterClassMeasuredRules) {
    register_class(L"S1Un", kInstance);
    const HWND w = create(L"S1Un", WS::POPUP);
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"S1Un"), kInstance}), 0u);
    EXPECT_EQ(get_last_error(), kErrClassHasWindows);
    EXPECT_EQ(call("DestroyWindow", {w}), 1u);
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"S1Un"), 0x10000}), 0u);
    EXPECT_EQ(get_last_error(), kErrClassDoesNotExist);
    // NULL names the executable's classes.
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"S1Un"), 0}), 1u);
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"S1Un"), kInstance}), 0u);
    EXPECT_EQ(get_last_error(), kErrClassDoesNotExist);
    // A NULL instance unregisters the process's copy of a system class;
    // CreateWindowEx of it then fails.
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"Edit"), kInstance}), 0u);
    EXPECT_EQ(call("UnregisterClassW", {wstr(L"Button"), 0}), 1u);
    const GuestAddr out = buffer(80);
    EXPECT_EQ(call("GetClassInfoExW", {0, wstr(L"Button"), out}), 0u);
}

TEST_F(S1WindowingTest, ClassLongsUseTheClassRecord) {
    const u64 atom = register_class(L"S1CL", kInstance, CS::HREDRAW | CS::DBLCLKS, 8, 4);
    const HWND w = create(L"S1CL", WS::POPUP);
    EXPECT_EQ(call("GetClassLongW", {w, static_cast<u64>(-26)}), 0x0Au);      // GCL_STYLE
    EXPECT_EQ(call("GetClassLongW", {w, static_cast<u64>(-18)}), 4u);         // GCL_CBWNDEXTRA
    EXPECT_EQ(call("GetClassLongW", {w, static_cast<u64>(-20)}), 8u);         // GCL_CBCLSEXTRA
    EXPECT_EQ(call("GetClassLongW", {w, static_cast<u64>(-32)}), atom);       // GCW_ATOM
    EXPECT_EQ(call("GetClassLongW", {w, 5}), 0u);                              // 5 + 4 > 8
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    // x64: the 32-bit form rejects pointer indices (gcl_wndproc_on_x64).
    EXPECT_EQ(call("GetClassLongW", {w, static_cast<u64>(-24)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    EXPECT_EQ(call("SetClassLongW", {w, 0, 0x1234}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("SetClassLongW", {w, 0, 0x5678}), 0x1234u);
    EXPECT_EQ(call("GetClassWord", {w, 0}), 0x5678u);
    EXPECT_EQ(call("GetClassWord", {w, 7}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    EXPECT_EQ(call("SetClassLongW", {w, static_cast<u64>(-26), CS::VREDRAW}), 0x0Au);
    EXPECT_EQ(call("GetClassLongPtrA", {w, static_cast<u64>(-26)}), 1u);
    EXPECT_EQ(call("SetClassLongW", {w, static_cast<u64>(-32), 5}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SetClassWord", {w, static_cast<u64>(-32), 5}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    EXPECT_EQ(call("SetClassLongPtrW", {w, 0, 0xAABBCCDD11223344ull}), 0x5678u);
    EXPECT_EQ(call("GetClassLongPtrW", {w, 0}), 0xAABBCCDD11223344ull);
    EXPECT_EQ(call("GetClassLongPtrW", {w, static_cast<u64>(-16)}), kInstance); // GCLP_HMODULE
    EXPECT_EQ(call("GetClassLongW", {0x7FFF0, static_cast<u64>(-26)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, GetClassNameTruncatesAndRejectsTinyBuffers) {
    register_class(L"S1Cls", kInstance);
    const HWND w = create(L"S1Cls", WS::POPUP);
    const GuestAddr buf = buffer(64, 0x55);
    EXPECT_EQ(call("GetClassNameW", {w, buf, 32}), 5u);
    EXPECT_EQ(memory_.read_u16(buf).value(), u16{'S'});
    EXPECT_EQ(call("GetClassNameW", {w, buf, 3}), 2u);
    EXPECT_EQ(memory_.read_u16(buf + 4).value(), 0u);
    const GuestAddr tiny = buffer(8, 0x55);
    EXPECT_EQ(call("GetClassNameW", {w, tiny, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInsufficientBuffer);
    EXPECT_EQ(memory_.read_u16(tiny).value(), 0x5555u);
    EXPECT_EQ(call("GetClassNameW", {HWND_DESKTOP, buf, 32}), 6u);  // "#32769"
    EXPECT_EQ(memory_.read_u16(buf).value(), u16{'#'});
}

// ---------------------------------------------------------------------------
// Window longs (s1win_api_probe "-- window longs", s1win_style_probe)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, WindowLongIndicesAndExtraBytes) {
    register_class(L"S1Long", kInstance, 0, 0, 16);
    const HWND owner = create(L"S1Long", WS::POPUP);
    const HWND w = create(L"S1Long", WS::POPUP, 0x80, owner);
    EXPECT_EQ(call("GetWindowLongW", {w, 12}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("GetWindowLongW", {w, 13}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    EXPECT_EQ(call("GetWindowLongW", {w, static_cast<u64>(-100)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    // x64: GWL_WNDPROC / GWL_HINSTANCE / GWL_HWNDPARENT are not valid for
    // the 32-bit function.
    EXPECT_EQ(call("GetWindowLongW", {w, static_cast<u64>(-4)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    EXPECT_EQ(call("SetWindowLongW", {w, 0, 0xCAFE}), 0u);
    EXPECT_EQ(call("SetWindowLongW", {w, 4, 0xBEEF}), 0u);
    EXPECT_EQ(call("GetWindowLongW", {w, 2}), 0xBEEF0000u);          // unaligned
    EXPECT_EQ(call("GetWindowLongPtrW", {w, 4}), 0xBEEFull);
    EXPECT_EQ(call("GetWindowLongPtrW", {w, 9}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
    // GWL_USERDATA starts at 0 and returns the previous value.
    EXPECT_EQ(call("SetWindowLongPtrW", {w, static_cast<u64>(-21), 0x1122334455667788ull}), 0u);
    EXPECT_EQ(call("GetWindowLongW", {w, static_cast<u64>(-21)}), 0x55667788u);
    // GWLP_HWNDPARENT of a top-level window is its owner; GWLP_HINSTANCE.
    EXPECT_EQ(call("GetWindowLongPtrW", {w, static_cast<u64>(-8)}), owner);
    EXPECT_EQ(call("GetWindowLongPtrW", {w, static_cast<u64>(-6)}), kInstance);
    EXPECT_EQ(call("GetWindowLongPtrW", {0x7FFF0, static_cast<u64>(-21)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, WindowLongX86AcceptsPointerIndices) {
    register_class(L"S1Long32", kInstance, 0, 0, 16);
    const HWND w = create(L"S1Long32", WS::POPUP);
    use_x86();
    cpu_.gpr.rsp = kStack + 0x800;
    EXPECT_EQ(call("GetWindowLongW", {w, static_cast<u64>(-6)}), kInstance & 0xFFFFFFFFull);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("GetWindowLongW", {w, static_cast<u64>(-100)}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidIndex);
}

TEST_F(S1WindowingTest, SetWindowLongStyleSendsStyleMessagesAndForcesBits) {
    register_class(L"S1Sty", kInstance);
    const HWND w = create(L"S1Sty", WS::OVERLAPPEDWINDOW);
    EXPECT_EQ(hwnd_mgr_.find(w)->style, 0x04CF0000u);
    EXPECT_EQ(hwnd_mgr_.find(w)->ex_style, 0x100u);
    sent_.clear();
    EXPECT_EQ(call("SetWindowLongW", {w, static_cast<u64>(-16), WS::POPUP | WS::BORDER}),
              0x04CF0000u);
    ASSERT_EQ(sent_.size(), 2u);
    EXPECT_EQ(sent_[0].msg, kWmStyleChanging);
    EXPECT_EQ(sent_[0].wparam, static_cast<u64>(-16));
    EXPECT_EQ(sent_[0].style_old, 0x04CF0000u);
    // WM_STYLECHANGING carries the caller's value; WS_CLIPSIBLINGS is added
    // after it (s1win_msgseq_probe setwindowlong_style_hidden).
    EXPECT_EQ(sent_[0].style_new, 0x80800000u);
    EXPECT_EQ(sent_[1].msg, kWmStyleChanged);
    EXPECT_EQ(sent_[1].style_new, 0x84800000u);
    // WS_EX_WINDOWEDGE follows the new frame.
    EXPECT_EQ(hwnd_mgr_.find(w)->ex_style, 0u);
    // WS_EX_TOPMOST cannot be set through SetWindowLong.
    EXPECT_EQ(call("SetWindowLongW", {w, static_cast<u64>(-20), 0x88}), 0u);
    EXPECT_EQ(hwnd_mgr_.find(w)->ex_style, 0x80u);
    // WS_EX_NOREDIRECTIONBITMAP only at creation.
    EXPECT_EQ(call("SetWindowLongW", {w, static_cast<u64>(-20), 0x200000}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(hwnd_mgr_.find(w)->ex_style, 0x80u);
    // The procedure may edit styleNew in WM_STYLECHANGING.
    edit_style_ = WS::HSCROLL;
    (void)call("SetWindowLongW", {w, static_cast<u64>(-16), WS::POPUP | WS::BORDER});
    edit_style_ = 0;
    EXPECT_EQ(hwnd_mgr_.find(w)->style, 0x84900000u);
}

TEST_F(S1WindowingTest, CreateStylesAndErrors) {
    register_class(L"S1Cr", kInstance);
    EXPECT_EQ(create(L"S1NoClass", WS::POPUP), 0u);
    EXPECT_EQ(get_last_error(), kErrCannotFindWndClass);
    EXPECT_EQ(create(L"S1Cr", WS::CHILD), 0u);
    EXPECT_EQ(get_last_error(), kErrTlwWithWsChild);
    EXPECT_EQ(create(L"S1Cr", WS::CHILD, 0, 0x7FFF0), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(create(L"S1Cr", WS::POPUP, 0x80000000u), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(create(L"S1Cr", WS::POPUP, 0, 0, 0x12345678), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    const HWND ov = create(L"S1Cr", WS::OVERLAPPED);
    EXPECT_EQ(hwnd_mgr_.find(ov)->style, 0x04C00000u);
    EXPECT_EQ(hwnd_mgr_.find(ov)->ex_style, 0x100u);
    const HWND p = create(L"S1Cr", WS::POPUP | WS::CAPTION, 0x20000);  // STATICEDGE
    EXPECT_EQ(hwnd_mgr_.find(p)->ex_style, 0x20000u);
    const HWND tool = create(L"S1Cr", WS::POPUP, 0x800);
    EXPECT_EQ(hwnd_mgr_.find(tool)->ex_style, 0x80u);
    const HWND child = create(L"S1Cr", WS::CHILD | WS::THICKFRAME, 0, ov);
    EXPECT_EQ(hwnd_mgr_.find(child)->style, 0x40040000u);
    EXPECT_EQ(hwnd_mgr_.find(child)->ex_style, 0x100u);
    // A child passed as the owner makes its top-level ancestor the owner.
    const HWND owned = create(L"S1Cr", WS::POPUP, 0, child);
    EXPECT_EQ(hwnd_mgr_.find(owned)->owner, ov);
}

// ---------------------------------------------------------------------------
// Properties (s1win_api_probe "-- props")
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, PropertiesAreCaseInsensitiveAtomsWithMeasuredErrors) {
    register_class(L"S1Prop", kInstance);
    const HWND w = create(L"S1Prop", WS::POPUP);
    EXPECT_EQ(call("SetPropW", {w, wstr(L"S1Prop"), 0x1234}), 1u);
    EXPECT_EQ(call("GetPropW", {w, wstr(L"s1PROP")}), 0x1234u);
    EXPECT_EQ(call("GetPropW", {w, wstr(L"S1NoProp")}), 0u);
    EXPECT_EQ(get_last_error(), kErrFileNotFound);
    EXPECT_EQ(call("RemovePropW", {w, wstr(L"S1Prop")}), 0x1234u);
    EXPECT_EQ(call("GetPropW", {w, wstr(L"S1Prop")}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("SetPropW", {0x7FFF0, wstr(L"S1Prop"), 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("SetPropW", {w, 0, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
}

// ---------------------------------------------------------------------------
// Tree, Z order, enumeration, Find (s1win_api_probe "-- tree")
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, ParentAncestorAndErrors) {
    register_class(L"S1Tree", kInstance);
    const HWND a = create(L"S1Tree", WS::OVERLAPPEDWINDOW);
    const HWND b = create(L"S1Tree", WS::POPUP, 0, a);
    const HWND ov = create(L"S1Tree", WS::OVERLAPPED, 0, a);
    const HWND c1 = create(L"S1Tree", WS::CHILD, 0, a, 101);
    const HWND g = create(L"S1Tree", WS::CHILD, 0, c1, 103);
    EXPECT_EQ(call("GetParent", {b}), a);          // WS_POPUP: the owner
    EXPECT_EQ(call("GetParent", {ov}), 0u);        // owned overlapped: NULL
    EXPECT_EQ(call("GetParent", {0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("GetAncestor", {g, 3}), a);
    EXPECT_EQ(call("GetAncestor", {b, 1}), HWND_DESKTOP);
    EXPECT_EQ(call("GetAncestor", {b, 3}), a);
    EXPECT_EQ(call("GetAncestor", {HWND_DESKTOP, 2}), 0u);
    EXPECT_EQ(call("GetAncestor", {g, 4}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("GetAncestor", {0x7FFF0, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("IsChild", {0x7FFF0, g}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("GetTopWindow", {0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, ZOrderTopmostBandsAndOwnedWindows) {
    register_class(L"S1Z", kInstance);
    const HWND a = create(L"S1Z", WS::OVERLAPPEDWINDOW);
    const HWND b = create(L"S1Z", WS::POPUP, 0, a);
    const HWND c1 = create(L"S1Z", WS::CHILD, 0, a);
    const HWND c2 = create(L"S1Z", WS::CHILD, 0, a);
    // The first child created is at the top (gw_child_a, gw_hwndnext_c1).
    EXPECT_EQ(call("GetWindow", {a, 5}), c1);
    EXPECT_EQ(call("GetWindow", {c1, 2}), c2);
    constexpr u64 kNoMoveSize = 0x3 | 0x10;
    EXPECT_EQ(call("SetWindowPos", {c1, 1 /*BOTTOM*/, 0, 0, 0, 0, kNoMoveSize}), 1u);
    EXPECT_EQ(call("SetWindowPos", {c1, c2, 0, 0, 0, 0, kNoMoveSize}), 1u);
    EXPECT_EQ(call("GetWindow", {a, 5}), c2);
    // A child ignores HWND_TOPMOST and a non-sibling insert-after.
    EXPECT_EQ(call("SetWindowPos", {c1, static_cast<u64>(-1), 0, 0, 0, 0, kNoMoveSize}), 1u);
    EXPECT_EQ(hwnd_mgr_.find(c1)->ex_style & 0x8u, 0u);
    EXPECT_EQ(call("GetWindow", {a, 5}), c2);
    EXPECT_EQ(call("SetWindowPos", {c1, 0x7FFF0, 0, 0, 0, 0, kNoMoveSize}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    // Owned windows follow their owner into and out of the topmost band.
    EXPECT_EQ(call("SetWindowPos", {a, static_cast<u64>(-1), 0, 0, 0, 0, kNoMoveSize}), 1u);
    EXPECT_NE(hwnd_mgr_.find(a)->ex_style & 0x8u, 0u);
    EXPECT_NE(hwnd_mgr_.find(b)->ex_style & 0x8u, 0u);
    auto tops = hwnd_mgr_.enum_children(HWND_DESKTOP);
    ASSERT_GE(tops.size(), 2u);
    EXPECT_EQ(tops[0], b);   // owned above owner
    EXPECT_EQ(tops[1], a);
    EXPECT_EQ(call("SetWindowPos", {a, static_cast<u64>(-2), 0, 0, 0, 0, kNoMoveSize}), 1u);
    EXPECT_EQ(hwnd_mgr_.find(b)->ex_style & 0x8u, 0u);
}

TEST_F(S1WindowingTest, FindWindowSearchesTopLevelsAndMessageWindows) {
    register_class(L"S1Find", kInstance);
    const HWND a = create(L"S1Find", WS::OVERLAPPEDWINDOW);
    hwnd_mgr_.find(a)->title = L"A";
    const HWND c1 = create(L"S1Find", WS::CHILD, 0, a);
    hwnd_mgr_.find(c1)->title = L"C1";
    const HWND msg = static_cast<HWND>(call("CreateWindowExW",
        {0, wstr(L"S1Find"), wstr(L"M"), 0, 0, 0, 0, 0, HWND_MESSAGE, 0, kInstance, 0}));
    ASSERT_NE(msg, 0u);
    hwnd_mgr_.find(msg)->title = L"M";
    EXPECT_EQ(call("FindWindowW", {wstr(L"s1find"), wstr(L"a")}), a);
    EXPECT_EQ(call("FindWindowW", {0, wstr(L"C1")}), 0u);   // children are not searched
    EXPECT_EQ(call("FindWindowExW", {HWND_MESSAGE, 0, wstr(L"S1Find"), wstr(L"M")}), msg);
    EXPECT_EQ(call("FindWindowExW", {0, 0, wstr(L"S1Find"), wstr(L"M")}), msg);
    EXPECT_EQ(call("FindWindowExW", {a, 0, 0, wstr(L"C1")}), c1);
    EXPECT_EQ(call("FindWindowExW", {0x7FFF0, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

// ---------------------------------------------------------------------------
// Geometry and state (s1win_api_probe "-- mapping", "-- state")
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, ChildScreenRectIncludesParentClientOffset) {
    register_class(L"S1Geo", kInstance);
    const HWND a = create(L"S1Geo", WS::OVERLAPPEDWINDOW);
    const HWND c1 = static_cast<HWND>(call("CreateWindowExW",
        {0, wstr(L"S1Geo"), wstr(L""), WS::CHILD | WS::BORDER, 10, 10, 80, 60, a, 101, kInstance, 0}));
    const GuestAddr r = buffer(16);
    EXPECT_EQ(call("GetWindowRect", {c1, r}), 1u);
    const auto* wa = hwnd_mgr_.find(a);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(r).value()),
              wa->rect.left + wa->client_offset.x + 10);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(r + 4).value()),
              wa->rect.top + wa->client_offset.y + 10);
    // Mapping the child's window rect into A's client gives its creation
    // position (c1_in_a_client = 10,10,90,70).
    EXPECT_NE(call("MapWindowPoints", {0, a, r, 2}), 0u);
    EXPECT_EQ(memory_.read_u32(r).value(), 10u);
    EXPECT_EQ(memory_.read_u32(r + 12).value(), 70u);
    const GuestAddr untouched = buffer(16, 0x77);
    EXPECT_EQ(call("GetWindowRect", {0x7FFF0, untouched}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(memory_.read_u32(untouched).value(), 0x77777777u);
    EXPECT_EQ(call("MapWindowPoints", {0x7FFF0, 0, r, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("ClientToScreen", {0x7FFF0, r}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, UnionRectIgnoresEmptyRectangles) {
    const GuestAddr a = buffer(16), b = buffer(16), out = buffer(16, 0x77);
    auto set = [&](GuestAddr p, i32 l, i32 t, i32 r, i32 bo) {
        (void)memory_.write_u32(p, static_cast<u32>(l));
        (void)memory_.write_u32(p + 4, static_cast<u32>(t));
        (void)memory_.write_u32(p + 8, static_cast<u32>(r));
        (void)memory_.write_u32(p + 12, static_cast<u32>(bo));
    };
    set(a, 0, 0, 10, 10);
    set(b, 50, 50, 40, 40);
    EXPECT_EQ(call("UnionRect", {out, a, b}), 1u);
    EXPECT_EQ(memory_.read_u32(out + 8).value(), 10u);
    set(a, 5, 5, 5, 9);
    EXPECT_EQ(call("UnionRect", {out, a, b}), 0u);
    EXPECT_EQ(memory_.read_u32(out + 8).value(), 0u);
    const GuestAddr untouched = buffer(16, 0x77);
    EXPECT_EQ(call("UnionRect", {untouched, 0, b}), 0u);
    EXPECT_EQ(memory_.read_u32(untouched).value(), 0x77777777u);
}

TEST_F(S1WindowingTest, LockWindowUpdateSingleLock) {
    register_class(L"S1Lock", kInstance);
    const HWND a = create(L"S1Lock", WS::POPUP);
    const HWND b = create(L"S1Lock", WS::POPUP);
    EXPECT_EQ(call("LockWindowUpdate", {a}), 1u);
    EXPECT_EQ(call("LockWindowUpdate", {b}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("LockWindowUpdate", {0}), 1u);
    EXPECT_EQ(call("LockWindowUpdate", {0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("LockWindowUpdate", {0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, SmallStateApisMeasuredErrors) {
    register_class(L"S1St", kInstance);
    const HWND a = create(L"S1St", WS::OVERLAPPEDWINDOW);
    for (const char* api : {"IsWindowEnabled", "IsZoomed", "IsIconic", "IsWindowVisible",
                            "IsWindowUnicode"}) {
        EXPECT_EQ(call(api, {0x7FFF0}), 0u) << api;
        EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle) << api;
    }
    EXPECT_EQ(call("IsWindowUnicode", {a}), 1u);
    EXPECT_EQ(call("CallWindowProcW", {0, a, 0x405, 2, 1}), 0u);
    call("SetLastErrorEx", {0x1234, 7});
    EXPECT_EQ(get_last_error(), 0x1234u);
    const GuestAddr fw = buffer(32);
    (void)memory_.write_u32(fw, 4);
    (void)memory_.write_u64(fw + 8, a);
    EXPECT_EQ(call("FlashWindowEx", {fw}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("FlashWindowEx", {0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
    EXPECT_EQ(call("FlashWindow", {0x7FFF0, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("GetGuiResources", {~0ull, 7}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_NE(call("GetGuiResources", {~0ull, 1}), 0u);
    const GuestAddr wi = buffer(60);
    (void)memory_.write_u32(wi, 0);
    EXPECT_EQ(call("GetWindowInfo", {a, wi}), 1u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);   // wrong cbSize still fills
    EXPECT_EQ(memory_.read_u32(wi).value(), 0u);
    EXPECT_EQ(memory_.read_u32(wi + 36).value(), 0x04CF0000u);   // dwStyle
    EXPECT_EQ(memory_.read_u32(wi + 44 + 12).value() >> 16, 0x0500u);   // wCreatorVersion
}

// ---------------------------------------------------------------------------
// Message sequences (s1win_msgseq_probe / s1win_msgseq2_probe .expected)
// ---------------------------------------------------------------------------

namespace {
std::vector<u32> msgs_of(const std::vector<S1WindowingTest::Sent>& sent, HWND h,
                         std::initializer_list<u32> skip = {}) {
    std::vector<u32> out;
    for (const auto& s : sent) {
        if (h != 0 && s.hwnd != h) continue;
        bool skipped = false;
        for (u32 k : skip) skipped = skipped || s.msg == k;
        if (!skipped) out.push_back(s.msg);
    }
    return out;
}
constexpr u32 kWmParentNotify = 0x0210;
constexpr u32 kWmChildActivate = 0x0022;
constexpr u32 kWmNcPaint = 0x0085;
constexpr u32 kWmEraseBkgnd = 0x0014;
}  // namespace

TEST_F(S1WindowingTest, CreateSendsMeasuredSequencePerWindowKind) {
    register_class(L"S1Seq", kInstance);
    // Overlapped: WM_GETMINMAXINFO first; WM_SIZE / WM_MOVE wait for the
    // first ShowWindow (create_overlapped_hidden).
    sent_.clear();
    const HWND ov = create(L"S1Seq", WS::OVERLAPPEDWINDOW);
    EXPECT_EQ(msgs_of(sent_, ov), (std::vector<u32>{WM::GETMINMAXINFO, WM::NCCREATE,
                                                    WM::NCCALCSIZE, WM::CREATE}));
    EXPECT_TRUE(hwnd_mgr_.find(ov)->pending_size_move);
    // Pop-up: WM_SIZE and WM_MOVE at once; WM_NCCALCSIZE(FALSE) gets the
    // window rectangle in screen coordinates (create_popup_hidden).
    sent_.clear();
    const HWND pop = create(L"S1Seq", WS::POPUP);
    EXPECT_EQ(msgs_of(sent_, pop), (std::vector<u32>{WM::NCCREATE, WM::NCCALCSIZE,
                                                     WM::CREATE, WM::SIZE, WM::MOVE}));
    ASSERT_EQ(sent_[1].msg, static_cast<u32>(WM::NCCALCSIZE));
    EXPECT_EQ(sent_[1].wparam, 0u);
    EXPECT_EQ(sent_[1].r0[0], 10);
    EXPECT_EQ(sent_[1].r0[3], 100);
    // Child: its NCCALCSIZE rectangle is offset by the parent's client
    // origin, then WM_PARENTNOTIFY(WM_CREATE, id) goes to the parent
    // (create_child_hidden); WS_EX_NOPARENTNOTIFY suppresses it.
    sent_.clear();
    const HWND kid = create(L"S1Seq", WS::CHILD, 0, pop, 0x2A);
    EXPECT_EQ(msgs_of(sent_, 0), (std::vector<u32>{WM::NCCREATE, WM::NCCALCSIZE, WM::CREATE,
                                                   WM::SIZE, WM::MOVE, kWmParentNotify}));
    EXPECT_EQ(sent_[1].r0[0], 20);   // parent at x=10 + child x=10
    EXPECT_EQ(sent_.back().hwnd, pop);
    EXPECT_EQ(sent_.back().wparam, (0x2Aull << 16) | WM::CREATE);
    EXPECT_EQ(sent_.back().lparam, kid);
    sent_.clear();
    (void)create(L"S1Seq", WS::CHILD, 0x4 /*WS_EX_NOPARENTNOTIFY*/, pop, 0x2B);
    EXPECT_EQ(msgs_of(sent_, pop), std::vector<u32>{});
}

TEST_F(S1WindowingTest, CreateVisibleShowsAfterCreateWithoutVisibleDuringCreate) {
    register_class(L"S1Vis", kInstance);
    sent_.clear();
    const HWND w = create(L"S1Vis", WS::OVERLAPPED | WS::VISIBLE);
    const auto seq = msgs_of(sent_, w, {WM::ACTIVATEAPP, WM::NCACTIVATE, WM::ACTIVATE,
                                        WM::SETFOCUS, kWmNcPaint, kWmEraseBkgnd});
    EXPECT_EQ(seq, (std::vector<u32>{WM::GETMINMAXINFO, WM::NCCREATE, WM::NCCALCSIZE,
                                     WM::CREATE, WM::SHOWWINDOW, WM::WINDOWPOSCHANGING,
                                     WM::WINDOWPOSCHANGED, WM::SIZE, WM::MOVE}));
    for (const auto& s : sent_) {
        if (s.msg == WM::CREATE) {
            EXPECT_EQ(s.live_style & WS::VISIBLE, 0u);      // GWL_STYLE during WM_CREATE
            EXPECT_EQ(s.style_old & WS::VISIBLE, WS::VISIBLE);  // CREATESTRUCT.style
            EXPECT_EQ(s.style_new, 0u);   // no overlapped-forced WS_EX_WINDOWEDGE
        }
        if (s.msg == WM::WINDOWPOSCHANGING) EXPECT_EQ(s.swp_flags, 0x43u);
    }
    EXPECT_NE(hwnd_mgr_.find(w)->style & WS::VISIBLE, 0u);
    EXPECT_FALSE(hwnd_mgr_.find(w)->pending_size_move);
    // A sizing-frame pop-up's CREATESTRUCT does carry WS_EX_WINDOWEDGE.
    sent_.clear();
    (void)create(L"S1Vis", WS::POPUP | WS::THICKFRAME);
    ASSERT_FALSE(sent_.empty());
    for (const auto& s : sent_) {
        if (s.msg == WM::NCCREATE) EXPECT_EQ(s.style_new, 0x100u);
    }
}

TEST_F(S1WindowingTest, CreateMinimizedAndMinTrackSizes) {
    register_class(L"S1Min", kInstance);
    sent_.clear();
    const HWND m = create(L"S1Min", WS::POPUP | WS::MINIMIZE);
    const WindowData* wd = hwnd_mgr_.find(m);
    ASSERT_NE(wd, nullptr);
    EXPECT_TRUE(wd->minimized);
    EXPECT_EQ(wd->rect.width(), 160);
    EXPECT_EQ(wd->rect.height(), 28);
    bool saw = false;
    for (const auto& s : sent_) {
        if (s.hwnd == m && s.msg == WM::WINDOWPOSCHANGING) {
            EXPECT_EQ(s.swp_flags, 0x8130u);   // create_popup_minimize_hidden
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
    // USER's pre-filled ptMinTrackSize (s1win_api_probe create_*_tiny).
    auto sized = [&](u32 style, u32 ex, i32 cx, i32 cy) {
        const HWND h = static_cast<HWND>(call("CreateWindowExW",
            {ex, wstr(L"S1Min"), wstr(L""), style, 5, 5, static_cast<u64>(cx),
             static_cast<u64>(cy), 0, 0, kInstance, 0}));
        const WindowData* d = hwnd_mgr_.find(h);
        return d ? std::make_pair(d->rect.width(), d->rect.height()) : std::make_pair(-1, -1);
    };
    EXPECT_EQ(sized(WS::OVERLAPPEDWINDOW, 0, 0, 0), std::make_pair(136, 39));
    EXPECT_EQ(sized(WS::POPUP | WS::THICKFRAME, 0, 3, 3), std::make_pair(14, 14));
    EXPECT_EQ(sized(WS::POPUP | WS::CAPTION | WS::THICKFRAME, 0, 3, 3), std::make_pair(136, 39));
    EXPECT_EQ(sized(WS::OVERLAPPEDWINDOW, 0x80 /*TOOLWINDOW*/, 3, 3), std::make_pair(16, 16));
    EXPECT_EQ(sized(WS::POPUP, 0, 0, 0), std::make_pair(0, 0));
}

TEST_F(S1WindowingTest, DestroyOrderOwnedThenDestroyPreOrderThenNcDestroyPostOrder) {
    register_class(L"S1Des", kInstance);
    const HWND par = create(L"S1Des", WS::POPUP);
    const HWND owned = create(L"S1Des", WS::POPUP, 0, par);
    const HWND a = create(L"S1Des", WS::CHILD | WS::VISIBLE, 0, par, 1);
    const HWND a1 = create(L"S1Des", WS::CHILD | WS::VISIBLE, 0, a, 2);
    const HWND b = create(L"S1Des", WS::CHILD | WS::VISIBLE, 0, par, 3);
    sent_.clear();
    set_last_error(kUntouched);
    EXPECT_EQ(call("DestroyWindow", {par}), 1u);
    std::vector<std::pair<HWND, u32>> order;
    for (const auto& s : sent_) {
        if (s.msg == WM::DESTROY || s.msg == WM::NCDESTROY) order.emplace_back(s.hwnd, s.msg);
    }
    const std::vector<std::pair<HWND, u32>> expected{
        {owned, WM::DESTROY}, {owned, WM::NCDESTROY},
        {par, WM::DESTROY}, {a, WM::DESTROY}, {a1, WM::DESTROY}, {b, WM::DESTROY},
        {a1, WM::NCDESTROY}, {a, WM::NCDESTROY}, {b, WM::NCDESTROY}, {par, WM::NCDESTROY}};
    EXPECT_EQ(order, expected);   // destroy_parent_with_two_children / destroy_owner_*
    for (HWND h : {par, owned, a, a1, b}) EXPECT_EQ(hwnd_mgr_.find(h), nullptr);
    EXPECT_EQ(call("DestroyWindow", {par}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, DestroyChildNotifiesParentThenHides) {
    register_class(L"S1Dc", kInstance);
    const HWND par = create(L"S1Dc", WS::POPUP);
    const HWND kid = create(L"S1Dc", WS::CHILD | WS::VISIBLE, 0, par, 9);
    sent_.clear();
    EXPECT_EQ(call("DestroyWindow", {kid}), 1u);
    ASSERT_GE(sent_.size(), 4u);
    // destroy_child: WM_PARENTNOTIFY(WM_DESTROY) first, then the hide.
    EXPECT_EQ(sent_[0].hwnd, par);
    EXPECT_EQ(sent_[0].msg, kWmParentNotify);
    EXPECT_EQ(sent_[0].wparam, (9ull << 16) | WM::DESTROY);
    EXPECT_EQ(msgs_of(sent_, kid), (std::vector<u32>{WM::SHOWWINDOW, WM::DESTROY,
                                                     WM::NCDESTROY}));
}

TEST_F(S1WindowingTest, SetParentMeasuredErrorsSequenceAndDesktop) {
    register_class(L"S1Sp", kInstance);
    const HWND main = create(L"S1Sp", WS::POPUP);
    const HWND kid = static_cast<HWND>(call("CreateWindowExW",
        {0, wstr(L"S1Sp"), wstr(L""), WS::CHILD | WS::VISIBLE, 7, 8, 60, 45, main, 1, kInstance, 0}));
    const HWND grandkid = create(L"S1Sp", WS::CHILD, 0, kid, 2);
    EXPECT_EQ(call("SetParent", {kid, kid}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SetParent", {kid, grandkid}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SetParent", {kid, 0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("SetParent", {0x7FFF0, main}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    // setparent_child_to_desktop: hide, move with its parent-relative
    // position (a WS_CHILD window still gets WM_CHILDACTIVATE), show again.
    sent_.clear();
    EXPECT_EQ(call("SetParent", {kid, HWND_DESKTOP}), main);
    const auto seq = msgs_of(sent_, kid, {kWmNcPaint, kWmEraseBkgnd, WM::MOVE, WM::SIZE});
    EXPECT_EQ(seq, (std::vector<u32>{WM::SHOWWINDOW, WM::WINDOWPOSCHANGING, kWmChildActivate,
                                     WM::WINDOWPOSCHANGED, WM::SHOWWINDOW,
                                     WM::WINDOWPOSCHANGING, WM::WINDOWPOSCHANGED}));
    std::vector<u32> flags;
    for (const auto& s : sent_) {
        if (s.hwnd == kid && (s.msg == WM::WINDOWPOSCHANGING || s.msg == WM::WINDOWPOSCHANGED)) {
            flags.push_back(s.swp_flags);
        }
    }
    EXPECT_EQ(flags, (std::vector<u32>{0x05, 0x080D, 0x57, 0x1857}));
    EXPECT_EQ(hwnd_mgr_.find(kid)->style, 0x54000000u);   // + WS_CLIPSIBLINGS
    EXPECT_EQ(hwnd_mgr_.find(kid)->rect.left, 7);
    EXPECT_EQ(call("GetParent", {kid}), HWND_DESKTOP);
    EXPECT_EQ(call("GetAncestor", {kid, 1}), HWND_DESKTOP);
}

TEST_F(S1WindowingTest, ShowWindowStateTransitionsReportMeasuredFlags) {
    register_class(L"S1Sw", kInstance);
    const HWND w = create(L"S1Sw", WS::POPUP);
    EXPECT_EQ(call("ShowWindow", {w, 5}), 0u);
    EXPECT_EQ(call("ShowWindow", {w, 5}), 0x18u);
    // showwindow_minimize_activating: CHANGED drops SWP_SHOWWINDOW for an
    // already visible window; a minimized window is not erased.
    sent_.clear();
    EXPECT_EQ(call("ShowWindow", {w, 6 /*SW_MINIMIZE*/}), 0x18u);
    std::vector<u32> flags;
    for (const auto& s : sent_) {
        if (s.hwnd == w && (s.msg == WM::WINDOWPOSCHANGING || s.msg == WM::WINDOWPOSCHANGED)) {
            flags.push_back(s.swp_flags);
        }
        EXPECT_NE(s.msg, kWmNcPaint);
        EXPECT_NE(s.msg, kWmEraseBkgnd);
    }
    ASSERT_EQ(flags.size(), 2u);
    EXPECT_EQ(flags[0], 0x8174u);
    EXPECT_EQ(flags[1] & 0x40u, 0u);
    EXPECT_TRUE(hwnd_mgr_.find(w)->minimized);
    // Restoring the active window re-sends WM_SETFOCUS and WM_ACTIVATE
    // (showwindow_shownormal_from_min).
    sent_.clear();
    EXPECT_EQ(call("ShowWindow", {w, 1}), 0x18u);
    const auto seq = msgs_of(sent_, w, {kWmNcPaint, kWmEraseBkgnd, WM::NCACTIVATE,
                                        WM::GETMINMAXINFO});
    ASSERT_GE(seq.size(), 2u);
    EXPECT_EQ(seq[seq.size() - 2], static_cast<u32>(WM::SETFOCUS));
    EXPECT_EQ(seq.back(), static_cast<u32>(WM::ACTIVATE));
    EXPECT_EQ(sent_.back().wparam, 1u);   // WA_ACTIVE, not minimized
    // Hiding the last window deactivates the application
    // (showwindow_hide_visible_popup).
    sent_.clear();
    EXPECT_EQ(call("ShowWindow", {w, 0}), 0x18u);
    EXPECT_EQ(msgs_of(sent_, w), (std::vector<u32>{WM::SHOWWINDOW, WM::WINDOWPOSCHANGING,
                                                   WM::WINDOWPOSCHANGED, WM::NCACTIVATE,
                                                   WM::ACTIVATE, WM::ACTIVATEAPP,
                                                   WM::KILLFOCUS}));
}

TEST_F(S1WindowingTest, EndDeferWindowPosWithDifferentParentsMovesNothing) {
    register_class(L"S1Dw", kInstance);
    const HWND top = create(L"S1Dw", WS::POPUP);
    const HWND kid = create(L"S1Dw", WS::CHILD, 0, top, 1);
    const u64 h = call("BeginDeferWindowPos", {2});
    ASSERT_NE(h, 0u);
    EXPECT_EQ(call("DeferWindowPos", {h, kid, 0, 1, 1, 0, 0, 0x1 | 0x4}), h);
    EXPECT_EQ(call("DeferWindowPos", {h, top, 0, 2, 2, 0, 0, 0x1 | 0x4 | 0x10}), h);
    EXPECT_EQ(call("EndDeferWindowPos", {h}), 1u);
    EXPECT_EQ(get_last_error(), 1441u);   // ERROR_HWNDS_HAVE_DIFF_PARENT (end_mixed)
    EXPECT_EQ(hwnd_mgr_.find(kid)->rect.left, 10);
    EXPECT_EQ(hwnd_mgr_.find(top)->rect.left, 10);
}

// ---------------------------------------------------------------------------
// Metrics, SystemParametersInfo, monitors, display devices and modes, DPI
// (tools/oracle_probe/s1win_display_probe.expected, s1win_api_probe.expected
// "-- adjustwindowrect")
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, SystemMetricsMeasuredTableAndDpiScaling) {
    EXPECT_EQ(call("GetSystemMetrics", {2}), 17u);    // SM_CXVSCROLL
    EXPECT_EQ(call("GetSystemMetrics", {11}), 32u);   // SM_CXICON
    EXPECT_EQ(call("GetSystemMetrics", {15}), 20u);   // SM_CYMENU
    EXPECT_EQ(call("GetSystemMetrics", {19}), 1u);    // SM_MOUSEPRESENT
    EXPECT_EQ(call("GetSystemMetrics", {23}), 0u);    // SM_SWAPBUTTON
    EXPECT_EQ(call("GetSystemMetrics", {28}), 136u);  // SM_CXMIN
    EXPECT_EQ(call("GetSystemMetrics", {34}), 136u);  // SM_CXMINTRACK
    EXPECT_EQ(call("GetSystemMetrics", {35}), 39u);   // SM_CYMINTRACK
    EXPECT_EQ(call("GetSystemMetrics", {7}), 3u);     // SM_CXDLGFRAME
    EXPECT_EQ(call("GetSystemMetrics", {57}), 160u);  // SM_CXMINIMIZED
    EXPECT_EQ(call("GetSystemMetrics", {71}), 15u);   // SM_CXMENUCHECK
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("GetSystemMetrics", {97}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("GetSystemMetrics", {~0ull}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("GetSystemMetrics", {0x2000}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    // A Vista+ image: frame 4 + padded border 4; an older one 8 and 0.
    EXPECT_EQ(call("GetSystemMetrics", {32}), 4u);
    EXPECT_EQ(call("GetSystemMetrics", {92}), 4u);
    mgr_->set_legacy_frame_metrics(true);
    EXPECT_EQ(call("GetSystemMetrics", {32}), 8u);
    EXPECT_EQ(call("GetSystemMetrics", {92}), 0u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {92, 96}), 4u);
    mgr_->set_legacy_frame_metrics(false);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {2, 144}), 26u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {13, 168}), 48u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {32, 192}), 5u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {92, 144}), 6u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {28, 240}), 324u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {15, 120}), 25u);
    EXPECT_EQ(call("GetSystemMetricsForDpi", {4, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
}

TEST_F(S1WindowingTest, AdjustWindowRectFrameRules) {
    auto adjust = [&](u32 style, u32 ex, bool menu) {
        const GuestAddr r = buffer(16);
        (void)memory_.write_u32(r, 100); (void)memory_.write_u32(r + 4, 100);
        (void)memory_.write_u32(r + 8, 400); (void)memory_.write_u32(r + 12, 300);
        EXPECT_EQ(call("AdjustWindowRectEx", {r, style, menu ? 1u : 0u, ex}), 1u);
        return std::array<i32, 4>{static_cast<i32>(memory_.read_u32(r).value()),
                                  static_cast<i32>(memory_.read_u32(r + 4).value()),
                                  static_cast<i32>(memory_.read_u32(r + 8).value()),
                                  static_cast<i32>(memory_.read_u32(r + 12).value())};
    };
    EXPECT_EQ(adjust(WS::POPUP | WS::THICKFRAME, 0, false), (std::array<i32, 4>{93, 93, 407, 307}));
    EXPECT_EQ(adjust(WS::POPUP | WS::DLGFRAME, 0, false), (std::array<i32, 4>{97, 97, 403, 303}));
    EXPECT_EQ(adjust(WS::POPUP | WS::BORDER, 0x20000, false), (std::array<i32, 4>{98, 98, 402, 302}));
    EXPECT_EQ(adjust(WS::POPUP, 0, true), (std::array<i32, 4>{100, 80, 400, 300}));
    // The caption is Vitreo's native-chrome 30 (Windows 23): 8 + 30.
    EXPECT_EQ(adjust(WS::OVERLAPPEDWINDOW, 0, false), (std::array<i32, 4>{92, 62, 408, 308}));
    // ForDpi: the sizing frame and padded border scale, edges do not.
    const GuestAddr r = buffer(16);
    (void)memory_.write_u32(r, 0); (void)memory_.write_u32(r + 4, 0);
    (void)memory_.write_u32(r + 8, 100); (void)memory_.write_u32(r + 12, 100);
    EXPECT_EQ(call("AdjustWindowRectExForDpi", {r, WS::OVERLAPPEDWINDOW, 1, 0x200, 120}), 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(r).value()), -11);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(r + 4).value()), -(11 + 36 + 25));
    EXPECT_EQ(call("AdjustWindowRectExForDpi", {r, WS::OVERLAPPEDWINDOW, 1, 0x200, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
}

TEST_F(S1WindowingTest, SystemParametersInfoMeasuredRules) {
    // NONCLIENTMETRICSW: exact cbSize (504 or 500), uiParam ignored; a
    // wrong size fails with the last error untouched.
    const GuestAddr ncm = buffer(520);
    (void)memory_.write_u32(ncm, 504);
    EXPECT_EQ(call("SystemParametersInfoW", {0x29, 0, ncm, 0}), 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(ncm + 408).value()), -12);
    EXPECT_EQ(memory_.read_u32(ncm + 4).value(), 1u);     // iBorderWidth
    EXPECT_EQ(memory_.read_u32(ncm + 500).value(), 4u);   // iPaddedBorderWidth
    (void)memory_.write_u32(ncm, 0);
    EXPECT_EQ(call("SystemParametersInfoW", {0x29, 504, ncm, 0}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    // An image older than subsystem 6.0: iBorderWidth 5, padded 0.
    mgr_->set_legacy_frame_metrics(true);
    (void)memory_.write_u32(ncm, 504);
    EXPECT_EQ(call("SystemParametersInfoW", {0x29, 504, ncm, 0}), 1u);
    EXPECT_EQ(memory_.read_u32(ncm + 4).value(), 5u);
    EXPECT_EQ(memory_.read_u32(ncm + 500).value(), 0u);
    mgr_->set_legacy_frame_metrics(false);
    // ForDpi scales; it takes only the three GET actions.
    (void)memory_.write_u32(ncm, 504);
    EXPECT_EQ(call("SystemParametersInfoForDpi", {0x29, 504, ncm, 0, 144}), 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(ncm + 408).value()), -18);
    EXPECT_EQ(memory_.read_u32(ncm + 8).value(), 26u);
    const GuestAddr rc = buffer(16);
    EXPECT_EQ(call("SystemParametersInfoForDpi", {0x30, 0, rc, 0, 96}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SystemParametersInfoW", {0x30, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
    EXPECT_EQ(call("SystemParametersInfoW", {0x9999, 0, rc, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SystemParametersInfoW", {0, 0, rc, 0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_SPI_VALUE);
    // STICKYKEYS: measured default, SET round-trips, a wrong cbSize fails
    // with the error untouched.
    const GuestAddr sk = buffer(8);
    (void)memory_.write_u32(sk, 8);
    EXPECT_EQ(call("SystemParametersInfoW", {0x3A, 0, sk, 0}), 1u);
    EXPECT_EQ(memory_.read_u32(sk + 4).value(), 0x1FEu);
    (void)memory_.write_u32(sk + 4, 0x1FC);
    EXPECT_EQ(call("SystemParametersInfoW", {0x3B, 8, sk, 0}), 1u);
    (void)memory_.write_u32(sk + 4, 0);
    EXPECT_EQ(call("SystemParametersInfoW", {0x3A, 8, sk, 0}), 1u);
    EXPECT_EQ(memory_.read_u32(sk + 4).value(), 0x1FCu);
    (void)memory_.write_u32(sk, 4);
    EXPECT_EQ(call("SystemParametersInfoW", {0x3A, 8, sk, 0}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    const GuestAddr dw = buffer(4, 0x77);
    EXPECT_EQ(call("SystemParametersInfoW", {0x68, 0, dw, 0}), 1u);   // SPI_GETWHEELSCROLLLINES
    EXPECT_EQ(memory_.read_u32(dw).value(), 3u);
}

TEST_F(S1WindowingTest, MonitorQueriesMeasuredErrors) {
    if (mgr_->guest_monitors().empty()) GTEST_SKIP() << "no NSScreen in this test process";
    register_class(L"S1Mon", kInstance);
    const HWND w = create(L"S1Mon", WS::POPUP);
    const u64 primary = call("MonitorFromWindow", {w, 1});
    ASSERT_NE(primary, 0u);
    EXPECT_EQ(call("MonitorFromWindow", {w, 3}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);   // ERROR_INVALID_FLAGS
    EXPECT_EQ(call("MonitorFromWindow", {0x7FFF0, 1}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("MonitorFromWindow", {0, 0}), 0u);
    EXPECT_EQ(call("MonitorFromWindow", {0, 1}), primary);
    const GuestAddr far_rect = buffer(16);
    (void)memory_.write_u32(far_rect, static_cast<u32>(-100000));
    (void)memory_.write_u32(far_rect + 4, static_cast<u32>(-100000));
    (void)memory_.write_u32(far_rect + 8, static_cast<u32>(-99990));
    (void)memory_.write_u32(far_rect + 12, static_cast<u32>(-99990));
    EXPECT_EQ(call("MonitorFromRect", {far_rect, 0}), 0u);
    EXPECT_NE(call("MonitorFromRect", {far_rect, 2}), 0u);
    EXPECT_EQ(call("MonitorFromRect", {far_rect, 4}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);
    // GetMonitorInfoW: MONITORINFO or MONITORINFOEXW only.
    const GuestAddr mi = buffer(120);
    (void)memory_.write_u32(mi, 104);
    EXPECT_EQ(call("GetMonitorInfoW", {primary, mi}), 1u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(memory_.read_u32(mi + 36).value(), 1u);          // MONITORINFOF_PRIMARY
    EXPECT_EQ(memory_.read_u16(mi + 40).value(), u'\\');       // szDevice
    (void)memory_.write_u32(mi, 72);
    EXPECT_EQ(call("GetMonitorInfoW", {primary, mi}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    (void)memory_.write_u32(mi, 72);
    EXPECT_EQ(call("GetMonitorInfoA", {primary, mi}), 1u);
    (void)memory_.write_u32(mi, 40);
    EXPECT_EQ(call("GetMonitorInfoW", {0x12345, mi}), 0u);
    EXPECT_EQ(get_last_error(), 1461u);   // ERROR_INVALID_MONITOR_HANDLE
    EXPECT_EQ(call("GetMonitorInfoW", {0, mi}), 0u);
    EXPECT_EQ(get_last_error(), 1461u);
}

TEST_F(S1WindowingTest, DisplayDevicesSettingsAndModeChanges) {
    if (mgr_->guest_monitors().empty()) GTEST_SKIP() << "no NSScreen in this test process";
    // DISPLAY_DEVICE.cb is not validated; the primary adapter is flagged.
    const GuestAddr dd = buffer(840);
    (void)memory_.write_u32(dd, 840);
    EXPECT_EQ(call("EnumDisplayDevicesW", {0, 0, dd, 0}), 1u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(memory_.read_u32(dd + 324).value() & 0x5u, 0x5u);
    (void)memory_.write_u32(dd, 0);
    EXPECT_EQ(call("EnumDisplayDevicesW", {0, 0, dd, 0}), 1u);
    (void)memory_.write_u32(dd, 840);
    EXPECT_EQ(call("EnumDisplayDevicesW", {wstr(L"\\\\.\\DISPLAY99"), 0, dd, 0}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("EnumDisplayDevicesW", {wstr(L"\\\\.\\DISPLAY1"), 0, dd, 1}), 1u);
    EXPECT_EQ(memory_.read_u16(dd + 328).value(), u'\\');   // \\?\DISPLAY#...
    EXPECT_EQ(memory_.read_u16(dd + 330).value(), u'\\');
    EXPECT_EQ(memory_.read_u16(dd + 332).value(), u'?');
    // DEVMODEW: Windows' own dmSize, the current mode's position fields.
    const GuestAddr dm = buffer(220);
    (void)memory_.write_u16(dm + 68, 220);
    EXPECT_EQ(call("EnumDisplaySettingsW", {0, 0xFFFFFFFFull, dm}), 1u);
    EXPECT_EQ(memory_.read_u16(dm + 68).value(), 188u);
    EXPECT_EQ(memory_.read_u32(dm + 72).value(), 0x207C00A0u);
    EXPECT_EQ(memory_.read_u16(dm + 64).value(), 0x0401u);
    EXPECT_EQ(call("EnumDisplaySettingsW", {wstr(L"\\\\.\\DISPLAY99"), 0xFFFFFFFFull, dm}), 0u);
    // ChangeDisplaySettingsEx validation.
    (void)memory_.write_u16(dm + 68, 220);
    ASSERT_EQ(call("EnumDisplaySettingsW", {0, 0xFFFFFFFFull, dm}), 1u);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW", {0, dm, 0, 2, 0})), 0);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW", {0, dm, 0, 2 | 0x00100000, 0})), -4);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW",
                                    {wstr(L"\\\\.\\DISPLAY99"), dm, 0, 2, 0})), -5);
    (void)memory_.write_u32(dm + 172, 123);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW", {0, dm, 0, 2, 0})), -2);
    (void)memory_.write_u16(dm + 68, 10);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW", {0, dm, 0, 2, 0})), -2);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsExW", {0, 0, 0, 0, 0})), 0);
    EXPECT_EQ(static_cast<i32>(call("ChangeDisplaySettingsW", {0, 0})), 0);
}

TEST_F(S1WindowingTest, DpiAwarenessAndMonitorDpiMeasuredResults) {
    // Set once (s1win_display_probe "-- dpi").
    auto shcore = [&](void (User32ExtManager::*fn)(Marshaler&), std::vector<u64> args) {
        u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
        for (usize i = 0; i < args.size() && i < 4; ++i) *regs[i] = args[i];
        set_last_error(kUntouched);
        ((*mgr_).*fn)(*m_);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    const GuestAddr out = buffer(8);
    EXPECT_EQ(shcore(&User32ExtManager::impl_GetProcessDpiAwareness, {0, out}), 0u);
    EXPECT_EQ(memory_.read_u32(out).value(), 0u);   // unaware by default
    EXPECT_EQ(shcore(&User32ExtManager::impl_GetProcessDpiAwareness, {0x1234, out}), 0x80070057u);
    EXPECT_EQ(memory_.read_u32(out).value(), 0xFFFFFFFFu);
    EXPECT_EQ(shcore(&User32ExtManager::impl_SetProcessDpiAwareness, {7}), 0x80070057u);
    EXPECT_EQ(shcore(&User32ExtManager::impl_SetProcessDpiAwareness, {2}), 0u);
    EXPECT_EQ(shcore(&User32ExtManager::impl_SetProcessDpiAwareness, {2}), 0x80070005u);
    EXPECT_EQ(get_last_error(), 5u);
    EXPECT_EQ(call("SetProcessDPIAware", {}), 1u);
    EXPECT_EQ(call("SetProcessDpiAwarenessContext", {static_cast<u64>(-4)}), 0u);
    EXPECT_EQ(get_last_error(), 5u);
    EXPECT_EQ(shcore(&User32ExtManager::impl_GetProcessDpiAwareness, {0, out}), 0u);
    EXPECT_EQ(memory_.read_u32(out).value(), 2u);
    EXPECT_EQ(call("GetDpiForWindow", {0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(shcore(&User32ExtManager::impl_GetDpiForMonitor, {0x12345, 0, out, out + 4}),
              0x80070006u);
    if (!mgr_->guest_monitors().empty()) {
        EXPECT_EQ(shcore(&User32ExtManager::impl_GetDpiForMonitor, {0x000D0001, 3, out, out + 4}),
                  0x800700A0u);
        EXPECT_EQ(shcore(&User32ExtManager::impl_GetDpiForMonitor, {0x000D0001, 0, 0, out + 4}),
                  0x800701E7u);
        EXPECT_EQ(shcore(&User32ExtManager::impl_GetDpiForMonitor, {0x000D0001, 0, out, out + 4}), 0u);
        EXPECT_EQ(memory_.read_u32(out).value(), 96u);
    }
}

// ---------------------------------------------------------------------------
// Menus (s1win_misc_probe "-- menus", x64)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, MenuPlacementStateAndErrorsMeasured) {
    constexpr u64 kBogus = 0x12345;
    constexpr u64 kByPosition = 0x400, kPopup = 0x10, kSeparator = 0x800;
    constexpr u64 kChecked = 0x8, kGrayed = 0x1;
    const u64 m = call("CreatePopupMenu", {});
    const u64 sub = call("CreatePopupMenu", {});
    ASSERT_NE(m, 0u);
    ASSERT_NE(sub, 0u);
    EXPECT_EQ(call("IsMenu", {kBogus}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    EXPECT_EQ(call("GetMenuItemCount", {m}), 0u);
    EXPECT_EQ(call("AppendMenuW", {m, 0, 100, wstr(L"One")}), 1u);
    EXPECT_EQ(call("AppendMenuW", {m, kSeparator, 0, 0}), 1u);
    EXPECT_EQ(call("AppendMenuW", {m, kPopup, sub, wstr(L"Sub")}), 1u);
    EXPECT_EQ(call("AppendMenuW", {sub, kChecked, 200, wstr(L"Two")}), 1u);
    EXPECT_EQ(call("AppendMenuW", {kBogus, 0, 1, wstr(L"x")}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    // MF_POPUP with a handle that is not a menu still appends, the handle
    // becoming the item ID, and records ERROR_INVALID_MENU_HANDLE.
    EXPECT_EQ(call("AppendMenuW", {m, kPopup, kBogus, wstr(L"Bad")}), 1u);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    EXPECT_EQ(call("GetMenuItemCount", {m}), 4u);
    EXPECT_EQ(static_cast<u32>(call("GetMenuItemCount", {kBogus})), 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    // Past-the-end positions and missing commands append.
    EXPECT_EQ(call("InsertMenuW", {m, 1, kByPosition, 101, wstr(L"Ins")}), 1u);
    EXPECT_EQ(call("InsertMenuW", {m, 100, 0, 102, wstr(L"Before100")}), 1u);
    EXPECT_EQ(call("InsertMenuW", {m, 99, kByPosition, 103, wstr(L"End")}), 1u);
    EXPECT_EQ(call("InsertMenuW", {m, 9999, 0, 104, wstr(L"Missing")}), 1u);
    EXPECT_EQ(call("InsertMenuW", {m, 0xFFFFFFFFu, kByPosition, 105, wstr(L"Last")}), 1u);
    const std::vector<u32> ids = {102, 100, 101, 0, 0xFFFFFFFFu, 0x12345, 103, 104, 105};
    ASSERT_EQ(call("GetMenuItemCount", {m}), ids.size());
    for (u32 i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(static_cast<u32>(call("GetMenuItemID", {m, i})), ids[i]) << i;
    }
    // CheckMenuItem / EnableMenuItem return the previous state, -1 when the
    // item is missing; a command in a submenu is found through the parent.
    EXPECT_EQ(call("CheckMenuItem", {m, 100, kChecked}), 0u);
    EXPECT_EQ(call("CheckMenuItem", {m, 100, 0}), kChecked);
    EXPECT_EQ(static_cast<u32>(call("CheckMenuItem", {m, 9999, kChecked})), 0xFFFFFFFFu);
    EXPECT_EQ(call("CheckMenuItem", {m, 200, 0}), kChecked);
    EXPECT_EQ(call("EnableMenuItem", {m, 100, kGrayed}), 0u);
    EXPECT_EQ(call("EnableMenuItem", {m, 100, 0}), kGrayed);
    EXPECT_EQ(static_cast<u32>(call("EnableMenuItem", {m, 9999, kGrayed})), 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(static_cast<u32>(call("EnableMenuItem", {kBogus, 100, kGrayed})), 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    // A popup item's state carries its item count in the high byte.
    EXPECT_EQ(call("GetMenuState", {m, 100, 0}), 0u);
    EXPECT_EQ(call("GetMenuState", {m, 4, kByPosition}), 0x110u);   // the "Sub" popup
    EXPECT_EQ(call("SetMenuDefaultItem", {m, 100, 0}), 1u);
    EXPECT_EQ(call("GetMenuDefaultItem", {m, 0, 0}), 100u);
    EXPECT_EQ(call("SetMenuDefaultItem", {m, 9999, 0}), 0u);
    EXPECT_EQ(call("GetMenuDefaultItem", {m, 0, 0}), 100u);
    EXPECT_EQ(call("SetMenuDefaultItem", {m, 0xFFFFFFFFu, 0}), 1u);
    EXPECT_EQ(static_cast<u32>(call("GetMenuDefaultItem", {m, 0, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(call("DestroyMenu", {m}), 1u);
    EXPECT_EQ(call("DestroyMenu", {m}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidMenuHandle);
    EXPECT_EQ(call("IsMenu", {m}), 0u);
}

TEST_F(S1WindowingTest, MenuItemInfoSizesAndTextMeasured) {
    // x64 MENUITEMINFOW: cbSize 0, fMask 4, fType 8, fState 12, wID 16,
    // hSubMenu 24, dwTypeData 56, cch 64; sizeof 80.
    constexpr u32 kMiimState = 0x1, kMiimId = 0x2, kMiimString = 0x40, kMiimFtype = 0x100;
    constexpr u32 kErrMenuItemNotFound = 1456;
    const u64 m = call("CreatePopupMenu", {});
    ASSERT_EQ(call("AppendMenuW", {m, 0, 100, wstr(L"One")}), 1u);
    const GuestAddr mi = buffer(96);
    auto reset = [&](u32 cb, u32 mask) {
        std::vector<u8> zero(96, 0);
        (void)memory_.write_bytes(mi, zero.data(), zero.size());
        (void)memory_.write_u32(mi + 0, cb);
        (void)memory_.write_u32(mi + 4, mask);
    };
    reset(80, kMiimString | kMiimId | kMiimState);
    (void)memory_.write_u32(mi + 16, 106);
    (void)memory_.write_u64(mi + 56, wstr(L"Info"));
    EXPECT_EQ(call("InsertMenuItemW", {m, 0, 1, mi}), 1u);
    for (u32 cb : {44u, 12u, 88u}) {   // only sizeof(MENUITEMINFOW) is accepted
        (void)memory_.write_u32(mi, cb);
        EXPECT_EQ(call("InsertMenuItemW", {m, 0, 1, mi}), 0u) << cb;
        EXPECT_EQ(get_last_error(), kErrInvalidParameter) << cb;
    }
    // Length query, truncation and full copy of the item text.
    reset(80, kMiimString | kMiimId | kMiimState | kMiimFtype);
    EXPECT_EQ(call("GetMenuItemInfoW", {m, 100, 0, mi}), 1u);
    EXPECT_EQ(memory_.read_u32(mi + 64).value(), 3u);
    EXPECT_EQ(memory_.read_u32(mi + 16).value(), 100u);
    const GuestAddr text = buffer(128, 0x77);
    (void)memory_.write_u64(mi + 56, text);
    (void)memory_.write_u32(mi + 64, 2);
    EXPECT_EQ(call("GetMenuItemInfoW", {m, 100, 0, mi}), 1u);
    EXPECT_EQ(memory_.read_u32(mi + 64).value(), 1u);
    EXPECT_EQ(memory_.read_u16(text).value(), u16{'O'});
    EXPECT_EQ(memory_.read_u16(text + 2).value(), 0u);
    (void)memory_.write_u32(mi + 64, 64);
    EXPECT_EQ(call("GetMenuItemInfoW", {m, 100, 0, mi}), 1u);
    EXPECT_EQ(memory_.read_u32(mi + 64).value(), 3u);
    EXPECT_EQ(memory_.read_u16(text + 4).value(), u16{'e'});
    EXPECT_EQ(call("GetMenuItemInfoW", {m, 9999, 0, mi}), 0u);
    EXPECT_EQ(get_last_error(), kErrMenuItemNotFound);
    (void)memory_.write_u32(mi, 12);
    EXPECT_EQ(call("GetMenuItemInfoW", {m, 100, 0, mi}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    // GetMenuInfo / SetMenuInfo take a 40-byte x64 MENUINFO.
    const GuestAddr info = buffer(48);
    (void)memory_.write_u32(info, 40);
    (void)memory_.write_u32(info + 4, 0x1F);   // MIM_ALL
    EXPECT_EQ(call("GetMenuInfo", {m, info}), 1u);
    (void)memory_.write_u32(info, 12);
    EXPECT_EQ(call("GetMenuInfo", {m, info}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
}

// ---------------------------------------------------------------------------
// wsprintf (s1win_misc_probe "-- wsprintf")
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, WsprintfMeasuredFormatting) {
    auto astr = [this](const std::string& s) {
        const GuestAddr at = buffer(static_cast<u32>(s.size() + 1));
        (void)memory_.write_bytes(at, reinterpret_cast<const u8*>(s.data()), s.size());
        return at;
    };
    auto read = [this](GuestAddr at) {
        std::string out;
        for (u32 i = 0; i < 2048; ++i) {
            const u8 c = memory_.read_u8(at + i).value_or(0);
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    };
    const GuestAddr out = buffer(1100);
    EXPECT_EQ(call("wsprintfA", {out, astr("%d|%u|%x|%X|%08x|%-5d|%5d|%s|%c|%lu|%ld|%ls|%%|%i"),
                                 static_cast<u64>(-12), 4000000000u, 255, 255, 0xabc, 3, 4,
                                 astr("str"), 'Z', 7, static_cast<u64>(-8), wstr(L"wide"), 9}),
              61u);
    EXPECT_EQ(read(out), "-12|4000000000|ff|FF|00000abc|3    |    4|str|Z|7|-8|wide|%|9");
    // %p is pointer-width hex; an unsupported conversion prints its letter.
    EXPECT_EQ(call("wsprintfA", {out, astr("%p|%I64d|%f|%e|%n"), 0x1234, 5}), 24u);
    EXPECT_EQ(read(out), "0000000000001234|5|f|e|n");
    // Only '-', '0' and '#' are flags: "%+d" and "% d" print the letter.
    EXPECT_EQ(call("wsprintfA", {out, astr("%-8s|%08d|%+d|% d|%#x"), astr("ab"),
                                 static_cast<u64>(-5), 3, 4, 255}),
              27u);
    EXPECT_EQ(read(out), "ab      |-0000005|+d| d|0x3");
    EXPECT_EQ(call("wsprintfA", {out, astr("%s|%s"), 0, astr("x")}), 2u);
    EXPECT_EQ(read(out), "|x");
    const GuestAddr big = astr(std::string(1499, 'x'));
    EXPECT_EQ(call("wsprintfA", {out, astr("%s"), big}), 1024u);
    EXPECT_EQ(read(out).size(), 1024u);
}

// ---------------------------------------------------------------------------
// Shutdown block reasons, DDE, device notifications (s1win_misc_probe)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, ShutdownBlockReasonMeasured) {
    register_class(L"S1Sbr", kInstance);
    const HWND w = create(L"S1Sbr", WS::OVERLAPPEDWINDOW);
    ASSERT_NE(w, 0u);
    const GuestAddr cch = buffer(4);
    const GuestAddr text = buffer(128, 0x77);
    EXPECT_EQ(call("ShutdownBlockReasonQuery", {w, 0, cch}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("ShutdownBlockReasonCreate", {w, wstr(L"Saving")}), 1u);
    EXPECT_EQ(call("ShutdownBlockReasonQuery", {w, 0, cch}), 1u);
    EXPECT_EQ(memory_.read_u32(cch).value(), 7u);
    (void)memory_.write_u32(cch, 64);
    EXPECT_EQ(call("ShutdownBlockReasonQuery", {w, text, cch}), 1u);
    EXPECT_EQ(memory_.read_u16(text).value(), u16{'S'});
    EXPECT_EQ(memory_.read_u32(cch).value(), 7u);
    // A short buffer still succeeds and reports the needed length.
    (void)memory_.write_u32(cch, 2);
    EXPECT_EQ(call("ShutdownBlockReasonQuery", {w, text, cch}), 1u);
    EXPECT_EQ(memory_.read_u32(cch).value(), 7u);
    EXPECT_EQ(call("ShutdownBlockReasonCreate", {w, wstr(L"Two")}), 1u);
    EXPECT_EQ(call("ShutdownBlockReasonCreate", {0x7FFF0, wstr(L"x")}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("ShutdownBlockReasonCreate", {w, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("ShutdownBlockReasonDestroy", {w}), 1u);
    EXPECT_EQ(call("ShutdownBlockReasonDestroy", {w}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("ShutdownBlockReasonDestroy", {0x7FFF0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
}

TEST_F(S1WindowingTest, DdeClientWithNoServerMeasured) {
    constexpr u32 kDmlInvalidParameter = 0x4006, kDmlNoConv = 0x400A;
    const GuestAddr inst_at = buffer(8);
    EXPECT_EQ(call("DdeInitializeW", {inst_at, 0x401000, 0x10, 0}), 0u);
    const u32 inst = memory_.read_u32(inst_at).value();
    ASSERT_NE(inst, 0u);
    const u64 svc = call("DdeCreateStringHandleW", {inst, wstr(L"S1NoSuchService"), 1200});
    const u64 topic = call("DdeCreateStringHandleW", {inst, wstr(L"S1Topic"), 1200});
    EXPECT_NE(svc, 0u);
    EXPECT_EQ(call("DdeCreateStringHandleW", {inst, wstr(L"x"), 1234}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);   // reading clears it
    EXPECT_EQ(call("DdeConnect", {inst, svc, topic, 0}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlNoConv);
    const GuestAddr result = buffer(4, 7);
    EXPECT_EQ(call("DdeClientTransaction", {0, 0, 0, topic, 1, 0x20B0, 1000, result}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(call("DdeDisconnect", {0}), 0u);
    EXPECT_EQ(call("DdeFreeStringHandle", {inst, svc}), 1u);
    EXPECT_EQ(call("DdeFreeStringHandle", {inst, svc}), 1u);
    EXPECT_EQ(call("DdeFreeStringHandle", {0x12345, topic}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {0x12345}), kDmlInvalidParameter);
    EXPECT_EQ(call("DdeUninitialize", {inst}), 1u);
    EXPECT_EQ(call("DdeUninitialize", {inst}), 0u);
    (void)memory_.write_u32(inst_at, 0x55);
    EXPECT_EQ(call("DdeInitializeW", {inst_at, 0x401000, 0x10, 0}), kDmlInvalidParameter);
}

// DdeCmpStringHandles and DdeNameService
// (tests/oracle_data/ddeml_nameservice_probe{32,64}.expected, x86 == x64).
TEST_F(S1WindowingTest, DdeNameServiceAndCmpStringHandlesMeasured) {
    constexpr u32 kDmlInvalidParameter = 0x4006, kDmlDllUsage = 0x4004;
    constexpr u64 kRegister = 1, kUnregister = 2, kFilterOn = 4, kFilterOff = 8;
    const GuestAddr inst_at = buffer(8);
    EXPECT_EQ(call("DdeInitializeW", {inst_at, 0x401000, 0 /*APPCLASS_STANDARD*/, 0}), 0u);
    const u32 inst = memory_.read_u32(inst_at).value();
    const u64 svc = call("DdeCreateStringHandleW", {inst, wstr(L"VitreoProbeSvc"), 1200});
    const u64 svc_lower = call("DdeCreateStringHandleW", {inst, wstr(L"vitreoprobesvc"), 1200});
    const u64 other = call("DdeCreateStringHandleW", {inst, wstr(L"VitreoProbeOther"), 1200});
    const u64 aaa = call("DdeCreateStringHandleW", {inst, wstr(L"AAA"), 1200});
    const u64 bbb = call("DdeCreateStringHandleW", {inst, wstr(L"bbb"), 1200});
    const u64 zzz = call("DdeCreateStringHandleW", {inst, wstr(L"ZZZ"), 1200});
    // == cmp
    EXPECT_EQ(svc, svc_lower);  // same-handle-for-case=1
    auto cmp = [&](u64 a, u64 b) { return static_cast<i32>(call("DdeCmpStringHandles", {a, b})); };
    EXPECT_EQ(cmp(svc, svc_lower), 0);
    EXPECT_EQ(cmp(aaa, bbb), -1);
    EXPECT_EQ(cmp(bbb, aaa), 1);
    EXPECT_EQ(cmp(aaa, zzz), -1);
    EXPECT_EQ(cmp(zzz, bbb), 1);
    EXPECT_EQ(cmp(aaa, 0), 1);
    EXPECT_EQ(cmp(0, aaa), -1);
    EXPECT_EQ(cmp(0, 0), 0);
    EXPECT_EQ(cmp(aaa, 0xBEEF), 1);
    // == nameservice: every success returns 1 and leaves no error.
    auto ns = [&](u64 h1, u64 h2, u64 c) { return call("DdeNameService", {inst, h1, h2, c}); };
    for (const auto& [h1, c] : std::vector<std::pair<u64, u64>>{
             {svc, kRegister}, {svc, kRegister}, {other, kRegister}, {svc, kUnregister},
             {svc, kUnregister}, {0, kUnregister}, {0, kUnregister}, {0, kFilterOn},
             {0, kFilterOff}, {svc, kFilterOn}, {svc, kRegister | kFilterOn},
             {svc, kRegister | kUnregister}, {0, kFilterOn | kFilterOff}, {svc, 0}, {svc, 0x10}}) {
        EXPECT_EQ(ns(h1, 0, c), 1u) << "cmd " << c;
        EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);
    }
    EXPECT_EQ(ns(0, 0, kRegister), 0u);  // register-null
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(ns(other, svc, kRegister), 0u);  // hsz2-nonzero
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(ns(0xBEEF, 0, kRegister), 0u);  // bogus-hsz
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(call("DdeNameService", {0x12345678, svc, 0, kRegister}), 0u);  // unknown-instance
    EXPECT_EQ(call("DdeUninitialize", {inst}), 1u);
    // clientonly: DNS_REGISTER is DMLERR_DLL_USAGE, DNS_FILTERON succeeds.
    (void)memory_.write_u32(inst_at, 0);
    EXPECT_EQ(call("DdeInitializeW", {inst_at, 0x401000, 0x10 /*APPCMD_CLIENTONLY*/, 0}), 0u);
    const u32 client = memory_.read_u32(inst_at).value();
    const u64 s = call("DdeCreateStringHandleW", {client, wstr(L"VitreoProbeSvc"), 1200});
    EXPECT_EQ(call("DdeNameService", {client, s, 0, kRegister}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {client}), kDmlDllUsage);
    EXPECT_EQ(call("DdeNameService", {client, 0, 0, kFilterOn}), 1u);
    EXPECT_EQ(call("DdeUninitialize", {client}), 1u);
}

// SetGestureConfig / GetGestureConfig
// (tests/oracle_data/clipformats_gesture_probe{32,64}.expected "== gesture").
TEST_F(S1WindowingTest, GestureConfigMeasured) {
    register_class(L"S1Gesture", kInstance);
    const HWND w = create(L"S1Gesture", 0x00CF0000 /*WS_OVERLAPPEDWINDOW*/);
    ASSERT_NE(w, 0u);
    const GuestAddr cfg = buffer(5 * 12);
    const GuestAddr count = buffer(4);
    auto get = [&](u64 flags) {
        for (u32 i = 0; i < 5; ++i) {
            (void)memory_.write_u32(cfg + i * 12, 3 + i);
            (void)memory_.write_u32(cfg + i * 12 + 4, 0xAAAA);
            (void)memory_.write_u32(cfg + i * 12 + 8, 0xBBBB);
        }
        (void)memory_.write_u32(count, 5);
        EXPECT_EQ(call("GetGestureConfig", {w, 0, flags, count, cfg, 12}), 1u);
        std::vector<u32> v;
        for (u32 i = 0; i < 5; ++i) {
            v.push_back(memory_.read_u32(cfg + i * 12 + 4).value());
            v.push_back(memory_.read_u32(cfg + i * 12 + 8).value());
        }
        return v;
    };
    EXPECT_EQ(get(0), std::vector<u32>(10, 0));  // default: want=0 block=0
    EXPECT_EQ(get(1), std::vector<u32>(10, 0));  // default-ancestors
    const GuestAddr two = buffer(24);
    for (const auto& [at, v] : std::vector<std::pair<u32, u32>>{
             {0, 3}, {4, 0}, {8, 1}, {12, 4}, {16, 2}, {20, 8}}) {
        (void)memory_.write_u32(two + at, v);  // {GID_ZOOM,0,GC_ZOOM},{GID_PAN,2,8}
    }
    EXPECT_EQ(call("SetGestureConfig", {w, 0, 2, two, 12}), 1u);  // set-two
    EXPECT_EQ(get(0), (std::vector<u32>{0, 1, 2, 8, 0, 0, 0, 0, 0, 0}));
    const GuestAddr all = buffer(12);
    (void)memory_.write_u32(all, 0);
    (void)memory_.write_u32(all + 4, 1);  // GC_ALLGESTURES
    (void)memory_.write_u32(all + 8, 0);
    EXPECT_EQ(call("SetGestureConfig", {w, 0, 1, all, 12}), 1u);  // set-all
    EXPECT_EQ(get(0), (std::vector<u32>{1, 0, 0x1f, 0, 1, 0, 1, 0, 1, 0}));
    auto set_err = [&](u64 hwnd, u64 reserved, u64 n, u64 p, u64 cb) {
        set_last_error(0);
        const u64 r = call("SetGestureConfig", {hwnd, reserved, n, p, cb});
        return r == 0 ? get_last_error() : 0u;
    };
    EXPECT_EQ(set_err(w, 0, 2, two, 16), error_codes::ERROR_INVALID_PARAMETER);       // bad-cb
    EXPECT_EQ(set_err(w, 1, 2, two, 12), 0u);                                          // reserved ok
    EXPECT_EQ(set_err(w, 0, 0, two, 12), error_codes::ERROR_INVALID_PARAMETER);        // count0
    EXPECT_EQ(set_err(w, 0, 2, 0, 12), error_codes::ERROR_INVALID_PARAMETER);          // null-cfg
    const GuestAddr bad = buffer(12);
    (void)memory_.write_u32(bad, 99);
    (void)memory_.write_u32(bad + 4, 1);
    EXPECT_EQ(set_err(w, 0, 1, bad, 12), error_codes::ERROR_INVALID_PARAMETER);        // bad-id
    EXPECT_EQ(set_err(0x1234, 0, 2, two, 12), error_codes::ERROR_INVALID_WINDOW_HANDLE);  // bad-hwnd
    EXPECT_EQ(set_err(windowing::HWND_DESKTOP, 0, 2, two, 12),
              error_codes::ERROR_INVALID_WINDOW_HANDLE);                               // desktop
}

TEST_F(S1WindowingTest, DeviceNotificationRegistrationMeasured) {
    register_class(L"S1Dn", kInstance);
    const HWND w = create(L"S1Dn", WS::OVERLAPPEDWINDOW);
    // DEV_BROADCAST_DEVICEINTERFACE_W: size, DBT_DEVTYP_DEVICEINTERFACE (5).
    const GuestAddr f = buffer(32);
    (void)memory_.write_u32(f, 32);
    (void)memory_.write_u32(f + 4, 5);
    const u64 dn = call("RegisterDeviceNotificationW", {w, f, 0});
    EXPECT_NE(dn, 0u);
    EXPECT_EQ(get_last_error(), 0u);   // success sets ERROR_SUCCESS
    EXPECT_EQ(call("UnregisterDeviceNotification", {dn}), 1u);
    EXPECT_EQ(call("UnregisterDeviceNotification", {dn}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(call("UnregisterDeviceNotification", {0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    // Neither the window nor the flags are validated.
    EXPECT_NE(call("RegisterDeviceNotificationW", {0x7FFF0, f, 0}), 0u);
    EXPECT_NE(call("RegisterDeviceNotificationW", {w, f, 0x10}), 0u);
    EXPECT_EQ(call("RegisterDeviceNotificationW", {w, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    const GuestAddr oem = buffer(12);
    (void)memory_.write_u32(oem, 12);
    (void)memory_.write_u32(oem + 4, 0);   // DBT_DEVTYP_OEM
    EXPECT_EQ(call("RegisterDeviceNotificationW", {w, oem, 0}), 0u);
    EXPECT_EQ(get_last_error(), 13u);      // ERROR_INVALID_DATA
}

// ---------------------------------------------------------------------------
// Layered windows (s1win_dwm_probe "-- layered", x64 = x86)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, LayeredWindowAttributesMeasuredRules) {
    constexpr u32 kWsExLayered = 0x00080000;
    constexpr u32 kLwaColorKey = 1, kLwaAlpha = 2;
    register_class(L"S1Layer", kInstance);
    const HWND plain = create(L"S1Layer", WS::OVERLAPPEDWINDOW);
    const HWND layered = create(L"S1Layer", WS::POPUP, kWsExLayered);
    ASSERT_NE(plain, 0u);
    ASSERT_NE(layered, 0u);
    const GuestAddr key = buffer(4), alpha = buffer(4), flags = buffer(4);
    // Layered but never given attributes: FALSE, last error untouched.
    EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, key, alpha, flags}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("SetLayeredWindowAttributes", {plain, 0, 128, kLwaAlpha}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("SetLayeredWindowAttributes", {0x7FFF0, 0, 128, kLwaAlpha}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("SetLayeredWindowAttributes", {layered, 0, 0, 0x80}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("GetLayeredWindowAttributes", {plain, key, alpha, flags}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    // LWA_COLORKEY needs a keying stage the window surface lacks: it fails
    // loudly instead of pretending.
    EXPECT_EQ(call("SetLayeredWindowAttributes", {layered, 0xFF00, 0, kLwaColorKey}), 0u);
    EXPECT_EQ(get_last_error(), 50u);   // ERROR_NOT_SUPPORTED
    // Flags 0 succeeds; the key is kept only with LWA_COLORKEY.
    EXPECT_EQ(call("SetLayeredWindowAttributes", {layered, 0x112233, 0, 0}), 1u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, key, alpha, flags}), 1u);
    EXPECT_EQ(memory_.read_u32(key).value(), 0u);
    EXPECT_EQ(memory_.read_u32(flags).value(), 0u);
    EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, 0, 0, 0}), 1u);
    // After SetLayeredWindowAttributes, UpdateLayeredWindow fails until
    // WS_EX_LAYERED is cleared and set again (Learn; measured 87).
    EXPECT_EQ(call("UpdateLayeredWindow", {layered, 0, 0, 0, 0, 0, 0, 0, 2}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("UpdateLayeredWindow", {plain, 0, 0, 0, 0, 0, 0, 0, 2}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("UpdateLayeredWindow", {0x7FFF0, 0, 0, 0, 0, 0, 0, 0, 2}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
    EXPECT_EQ(call("SetWindowLongW", {layered, static_cast<u64>(-20), 0}), kWsExLayered);
    EXPECT_EQ(call("SetWindowLongW", {layered, static_cast<u64>(-20), kWsExLayered}), 0u);
    EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, key, alpha, flags}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    // A fresh layering accepts UpdateLayeredWindow (move-only call) and then
    // reports no attributes; undefined ULW flags are rejected.
    EXPECT_EQ(call("UpdateLayeredWindow", {layered, 0, 0, 0, 0, 0, 0, 0, 0x80}), 0u);
    EXPECT_EQ(get_last_error(), kErrInvalidParameter);
    EXPECT_EQ(call("UpdateLayeredWindow", {layered, 0, 0, 0, 0, 0, 0, 0, 2}), 1u);
    EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, key, alpha, flags}), 0u);
    EXPECT_EQ(get_last_error(), kUntouched);
    // SetLayeredWindowAttributes works after UpdateLayeredWindow. LWA_ALPHA
    // is applied to the native window; without one it fails loudly.
    const u64 applied = call("SetLayeredWindowAttributes", {layered, 0x112233, 128, kLwaAlpha});
    if (bridge_.has_native_window(layered)) {
        EXPECT_EQ(applied, 1u);
        EXPECT_EQ(call("GetLayeredWindowAttributes", {layered, key, alpha, flags}), 1u);
        EXPECT_EQ(memory_.read_u8(alpha).value(), 128u);
        EXPECT_EQ(memory_.read_u32(flags).value(), kLwaAlpha);
        EXPECT_EQ(memory_.read_u32(key).value(), 0u);   // key not given
    } else {
        EXPECT_EQ(applied, 0u);
        EXPECT_EQ(get_last_error(), 50u);
    }
}

// ---------------------------------------------------------------------------
// SumatraPDF-install imports (tools/oracle_probe/u32_char_dde_probe)
// ---------------------------------------------------------------------------

TEST_F(S1WindowingTest, DdeLParamPackingMeasured) {
    VirtualMemoryManager vmm(memory_);
    HandleTable handles;
    HeapManager heap(vmm, memory_, handles);
    GlobalLocalManager globals(heap);
    mgr_->set_global_local_manager(&globals);
    for (const bool x86 : {false, true}) {
        if (x86) use_x86();
        const u64 slot = x86 ? 4 : 8;
        // pack msg=3e2/3e4/3e5/3e7 packed: a FIXED object of two UINT_PTRs
        // (packed_global size=8/16, fixed=1, v=51,52).
        for (u32 msg : {0x3E2u, 0x3E4u, 0x3E5u, 0x3E7u}) {
            const u64 lp = call("PackDDElParam", {msg, 0x1234, 0xABCD});
            ASSERT_NE(lp, 0u) << msg;
            EXPECT_EQ(globals.global_size(lp), 2 * slot) << msg;
            EXPECT_EQ(globals.global_lock(lp), lp) << msg;
            (void)globals.global_unlock(lp);
            const GuestAddr lo = buffer(8, 0x77), hi = buffer(8, 0x77);
            EXPECT_EQ(call("UnpackDDElParam", {msg, lp, lo, hi}), 1u) << msg;
            EXPECT_EQ(memory_.read_u32(lo).value(), 0x1234u);
            EXPECT_EQ(memory_.read_u32(hi).value(), 0xABCDu);
            EXPECT_EQ(call("FreeDDElParam", {msg, lp}), 1u) << msg;
            EXPECT_EQ(get_last_error(), kUntouched);
        }
        // pack msg=3e0/3e1/3e3/3e6 and WM_USER: MAKELONG; 3e8: uiHi.
        for (u32 msg : {0x3E0u, 0x3E1u, 0x3E3u, 0x3E6u, 0x400u}) {
            EXPECT_EQ(call("PackDDElParam", {msg, 0x11, 0x22}), 0x00220011u) << msg;
        }
        EXPECT_EQ(call("PackDDElParam", {0x3E8, 0x1234, 0xABCD}), 0xABCDu);
        // pack_initiate_big ffffffff:def05678 (sign-extended on x64).
        EXPECT_EQ(call("PackDDElParam", {0x3E0, 0x12345678, 0x9abcdef0}),
                  x86 ? 0xdef05678ull : 0xffffffffdef05678ull);
        // unpack_initiate_big lo=def0 hi=9abc; unpack_execute_big lo=0 hi=lParam.
        const GuestAddr lo = buffer(8, 0x77), hi = buffer(8, 0x77);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E0, 0x9abcdef0, lo, hi}), 1u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 0xdef0u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 0x9abcu);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E8, 0x9abcdef0, lo, hi}), 1u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 0u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 0x9abcdef0u);
        // unpack_ack_zero 0, outputs 0, error untouched; unpack_ack_bogus 0,
        // outputs 0, ERROR_INVALID_HANDLE.
        (void)memory_.write_u32(lo, 0x77);
        (void)memory_.write_u32(hi, 0x77);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E4, 0, lo, hi}), 0u);
        EXPECT_EQ(get_last_error(), kUntouched);
        EXPECT_EQ(memory_.read_u32(lo).value(), 0u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 0u);
        (void)memory_.write_u32(lo, 0x77);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E4, 0x12340000, lo, hi}), 0u);
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 0u);
        // unpack_null_out 1; unpack_cross_msg 1 lo=5 hi=6.
        const u64 lp = call("PackDDElParam", {0x3E5, 5, 6});
        EXPECT_EQ(call("UnpackDDElParam", {0x3E5, lp, 0, 0}), 1u);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E7, lp, lo, hi}), 1u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 5u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 6u);
        // free_zero 1, free_unpacked 1.
        EXPECT_EQ(call("FreeDDElParam", {0x3E4, 0}), 1u);
        EXPECT_EQ(call("FreeDDElParam", {0x3E0, 0x1234}), 1u);
        // reuse_packed_packed same=1 lo=3 hi=4.
        const u64 r = call("ReuseDDElParam", {lp, 0x3E5, 0x3E4, 3, 4});
        EXPECT_EQ(r, lp);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E4, r, lo, hi}), 1u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 3u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 4u);
        // reuse_packed_unpacked 8 (the object is freed).
        EXPECT_EQ(call("ReuseDDElParam", {r, 0x3E4, 0x3E8, 7, 8}), 8u);
        EXPECT_EQ(globals.global_size(r), 0u);
        // reuse_unpacked_packed packed lo=9 hi=a; reuse_unpacked_unpacked c000b.
        const u64 r3 = call("ReuseDDElParam", {8, 0x3E8, 0x3E2, 9, 10});
        EXPECT_NE(r3, 0x000a0009u);
        EXPECT_EQ(call("UnpackDDElParam", {0x3E2, r3, lo, hi}), 1u);
        EXPECT_EQ(memory_.read_u32(lo).value(), 9u);
        EXPECT_EQ(memory_.read_u32(hi).value(), 10u);
        EXPECT_EQ(call("FreeDDElParam", {0x3E2, r3}), 1u);
        EXPECT_EQ(call("ReuseDDElParam", {0x55, 0x3E0, 0x3E1, 11, 12}), 0x000c000bu);
        // reuse_null_packed: 0.
        EXPECT_EQ(call("ReuseDDElParam", {0, 0x3E4, 0x3E5, 13, 14}), 0u);
    }
}

TEST_F(S1WindowingTest, DdeDataHandlesMeasured) {
    constexpr u32 kDmlInvalidParameter = 0x4006;
    VirtualMemoryManager vmm(memory_);
    HandleTable handles;
    HeapManager heap(vmm, memory_, handles);
    GlobalLocalManager globals(heap);
    mgr_->set_global_local_manager(&globals);
    const GuestAddr inst_at = buffer(8);
    ASSERT_EQ(call("DdeInitializeW", {inst_at, 0x401000, 0x10, 0}), 0u);
    const u32 inst = memory_.read_u32(inst_at).value();
    const GuestAddr src = buffer(8);
    (void)memory_.write_bytes(src, reinterpret_cast<const u8*>("hello\0ZZ"), 8);
    const u64 item = call("DdeCreateStringHandleW", {inst, wstr(L"item"), 1200});
    const u64 h = call("DdeCreateDataHandle", {inst, src, 6, 0, item, 1, 0});
    ASSERT_NE(h, 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);
    // getdata_size 6, getdata_size_off 4, getdata_3_1 3 "ell", getdata_16_0 6.
    EXPECT_EQ(call("DdeGetData", {h, 0, 0, 0}), 6u);
    EXPECT_EQ(call("DdeGetData", {h, 0, 0, 2}), 4u);
    const GuestAddr b = buffer(16, 0x55);
    EXPECT_EQ(call("DdeGetData", {h, b, 3, 1}), 3u);
    EXPECT_EQ(memory_.read_u32(b).value(), 0x556c6c65u);
    EXPECT_EQ(call("DdeGetData", {h, b, 16, 0}), 6u);
    // getdata_off_end / _off_past / _null_off_past: 0, DMLERR_INVALIDPARAMETER.
    for (const auto& [dst, off] : {std::pair<u64, u64>{b, 6}, {b, 7}, {0, 7}}) {
        EXPECT_EQ(call("DdeGetData", {h, dst, 4, off}), 0u);
        EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    }
    // access ok cb=6; access_again same pointer; unaccess x3 TRUE.
    const GuestAddr cb = buffer(4, 0x77);
    const u64 p = call("DdeAccessData", {h, cb});
    ASSERT_NE(p, 0u);
    EXPECT_EQ(memory_.read_u32(cb).value(), 6u);
    EXPECT_EQ(memory_.read_u8(p).value(), 'h');
    (void)memory_.write_u8(p, 'J');
    EXPECT_EQ(call("DdeAccessData", {h, 0}), p);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(call("DdeUnaccessData", {h}), 1u);
    EXPECT_EQ(call("DdeGetData", {h, b, 1, 0}), 1u);
    EXPECT_EQ(memory_.read_u8(b).value(), 'J');
    // adddata same handle, size 11, gap zero: 4a 65 6c 6c 6f 00 00 00 58 59 5a.
    const GuestAddr xyz = buffer(4);
    (void)memory_.write_bytes(xyz, reinterpret_cast<const u8*>("XYZ"), 3);
    EXPECT_EQ(call("DdeAddData", {h, xyz, 3, 8}), h);
    EXPECT_EQ(call("DdeGetData", {h, 0, 0, 0}), 11u);
    EXPECT_EQ(call("DdeGetData", {h, b, 16, 0}), 11u);
    const std::array<u8, 11> grown{0x4a, 0x65, 0x6c, 0x6c, 0x6f, 0, 0, 0, 0x58, 0x59, 0x5a};
    for (size_t i = 0; i < grown.size(); ++i) {
        EXPECT_EQ(memory_.read_u8(b + i).value(), grown[i]) << i;
    }
    EXPECT_EQ(call("DdeAddData", {h, xyz, 1, 1}), h);         // adddata_inside
    EXPECT_EQ(call("DdeGetData", {h, 0, 0, 0}), 11u);
    EXPECT_EQ(call("DdeAddData", {h, xyz, 0, 0}), h);         // adddata_zero_cb
    EXPECT_EQ(call("DdeAddData", {h, 0, 2, 0}), h);           // adddata_null_src
    EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);
    // free 1; then the handle is dead: free/access/unaccess/adddata 0 with no
    // DDEML error, getdata 0 with DMLERR_INVALIDPARAMETER.
    EXPECT_EQ(call("DdeFreeDataHandle", {h}), 1u);
    for (const u64 dead : {h, u64{0}, u64{0x1234560}}) {
        EXPECT_EQ(call("DdeFreeDataHandle", {dead}), 0u);
        EXPECT_EQ(call("DdeAccessData", {dead, 0}), 0u);
        EXPECT_EQ(call("DdeUnaccessData", {dead}), 0u);
        EXPECT_EQ(call("DdeAddData", {dead, src, 1, 0}), 0u);
        EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);
        EXPECT_EQ(call("DdeGetData", {dead, 0, 0, 0}), 0u);
        EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    }
    // create_null_src size 4; create_zero_cb size query 0 + error;
    // create_off places the data at object offset cbOff: size 5, 00 00 68 65 6c.
    u64 hz = call("DdeCreateDataHandle", {inst, 0, 4, 0, 0, 1, 0});
    EXPECT_EQ(call("DdeGetData", {hz, 0, 0, 0}), 4u);
    EXPECT_EQ(call("DdeFreeDataHandle", {hz}), 1u);
    hz = call("DdeCreateDataHandle", {inst, src, 0, 0, 0, 1, 0});
    EXPECT_NE(hz, 0u);
    EXPECT_EQ(call("DdeGetData", {hz, 0, 0, 0}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    (void)call("DdeFreeDataHandle", {hz});
    hz = call("DdeCreateDataHandle", {inst, src, 3, 2, 0, 1, 0});
    EXPECT_EQ(call("DdeGetData", {hz, b, 16, 0}), 5u);
    EXPECT_EQ(memory_.read_u32(b).value(), 0x65680000u);   // 00 00 68 65
    EXPECT_EQ(memory_.read_u8(b + 4).value(), 0x6cu);
    (void)call("DdeFreeDataHandle", {hz});
    // create_appowned / free_appowned; create_badflag and create_badinst 0
    // with DMLERR_INVALIDPARAMETER; create_badhsz accepted.
    hz = call("DdeCreateDataHandle", {inst, src, 6, 0, 0, 1, 1});
    EXPECT_EQ(call("DdeGetData", {hz, 0, 0, 0}), 6u);
    EXPECT_EQ(call("DdeFreeDataHandle", {hz}), 1u);
    EXPECT_EQ(call("DdeCreateDataHandle", {inst, src, 6, 0, 0, 1, 0x80}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    EXPECT_EQ(call("DdeCreateDataHandle", {0x5555, src, 6, 0, 0, 1, 0}), 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), kDmlInvalidParameter);
    hz = call("DdeCreateDataHandle", {inst, src, 6, 0, 0x9999, 1, 0});
    EXPECT_NE(hz, 0u);
    EXPECT_EQ(call("DdeGetLastError", {inst}), 0u);
    // transact / free_after_transact: not consumed without a conversation.
    EXPECT_EQ(call("DdeClientTransaction", {hz, 0xFFFFFFFF, 0, 0, 1, 0x4090, 1000, 0}), 0u);
    EXPECT_EQ(call("DdeGetData", {hz, 0, 0, 0}), 6u);
    EXPECT_EQ(call("DdeFreeDataHandle", {hz}), 1u);
    // after_uninit: the instance's objects die with it, owned or not.
    hz = call("DdeCreateDataHandle", {inst, src, 6, 0, 0, 1, 1});
    EXPECT_EQ(call("DdeUninitialize", {inst}), 1u);
    EXPECT_EQ(call("DdeGetData", {hz, 0, 0, 0}), 0u);
    EXPECT_EQ(call("DdeFreeDataHandle", {hz}), 0u);
}

TEST_F(S1WindowingTest, ShowWindowAsyncIsDeferredToTheOwningThread) {
    register_class(L"S1Swa", kInstance);
    const HWND w = create(L"S1Swa", WS::POPUP);
    const u32 owner = hwnd_mgr_.owner_thread_id(w).value();
    sent_.clear();
    // swa_own ret=1 visible_now=0 showmsgs_now=0; after_peek visible=1.
    EXPECT_EQ(call("ShowWindowAsync", {w, 8 /*SW_SHOWNA*/}), 1u);
    EXPECT_EQ(get_last_error(), kUntouched);
    EXPECT_EQ(hwnd_mgr_.find(w)->style & WS::VISIBLE, 0u);
    EXPECT_TRUE(sent_.empty());
    EXPECT_TRUE(queue_.dispatch_sent_messages(owner));
    EXPECT_NE(hwnd_mgr_.find(w)->style & WS::VISIBLE, 0u);
    // swa_visible ret=1; swa_hide ret=1, still visible until the queue runs.
    EXPECT_EQ(call("ShowWindowAsync", {w, 8}), 1u);
    EXPECT_EQ(call("ShowWindowAsync", {w, 0 /*SW_HIDE*/}), 1u);
    EXPECT_NE(hwnd_mgr_.find(w)->style & WS::VISIBLE, 0u);
    EXPECT_TRUE(queue_.dispatch_sent_messages(owner));
    EXPECT_EQ(hwnd_mgr_.find(w)->style & WS::VISIBLE, 0u);
    // swa_invalid / swa_null 1400; swa_badcmd 87 and nothing queued.
    for (const bool x86 : {false, true}) {
        if (x86) use_x86();
        EXPECT_EQ(call("ShowWindowAsync", {0x7FFF0, 5}), 0u);
        EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
        EXPECT_EQ(call("ShowWindowAsync", {0, 5}), 0u);
        EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
        EXPECT_EQ(call("ShowWindowAsync", {w, 99}), 0u);
        EXPECT_EQ(get_last_error(), kErrInvalidParameter);
        EXPECT_FALSE(queue_.dispatch_sent_messages(owner));
    }
}

TEST_F(S1WindowingTest, GetComboBoxInfoMeasured) {
    constexpr u32 kErrNoAccess = 998, kErrCallNotImplemented = 120;
    register_class(L"S1Cbi", kInstance);
    const HWND parent = create(L"S1Cbi", WS::OVERLAPPEDWINDOW);
    const HWND combo = create(L"ComboBox", WS::CHILD | 2 /*CBS_DROPDOWN*/, 0, parent, 100);
    ASSERT_NE(combo, 0u);
    for (const bool x86 : {false, true}) {
        if (x86) use_x86();
        const u32 size = x86 ? 52 : 64;   // sizeof_comboboxinfo
        const GuestAddr ci = buffer(80);
        // cbi_invalid / cbi_nullhwnd 1400; cbi_null 998.
        (void)memory_.write_u32(ci, size);
        EXPECT_EQ(call("GetComboBoxInfo", {0x7FFF0, ci}), 0u);
        EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
        EXPECT_EQ(call("GetComboBoxInfo", {0, ci}), 0u);
        EXPECT_EQ(get_last_error(), kErrInvalidWindowHandle);
        EXPECT_EQ(call("GetComboBoxInfo", {combo, 0}), 0u);
        EXPECT_EQ(get_last_error(), kErrNoAccess);
        // cbi_size0 / _minus1 / _plus4: 87.
        for (const u32 bad : {0u, size - 1, size + 4}) {
            (void)memory_.write_u32(ci, bad);
            EXPECT_EQ(call("GetComboBoxInfo", {combo, ci}), 0u) << bad;
            EXPECT_EQ(get_last_error(), kErrInvalidParameter) << bad;
        }
        // A native combo box has no Edit/ComboLBox child windows: loud failure.
        (void)memory_.write_u32(ci, size);
        EXPECT_EQ(call("GetComboBoxInfo", {combo, ci}), 0u);
        EXPECT_EQ(get_last_error(), kErrCallNotImplemented);
        // cbi_notcombo: CB_GETCOMBOBOXINFO is sent and its result returned
        // (DefWindowProc: 0, error untouched); cbi_custom_badsize: x64 sends
        // whatever cbSize says, x86 refuses it first.
        sent_.clear();
        EXPECT_EQ(call("GetComboBoxInfo", {parent, ci}), 0u);
        EXPECT_EQ(get_last_error(), kUntouched);
        ASSERT_EQ(sent_.size(), 1u);
        EXPECT_EQ(sent_[0].msg, 0x164u);
        EXPECT_EQ(sent_[0].lparam, ci);
        sent_.clear();
        (void)memory_.write_u32(ci, 3);
        EXPECT_EQ(call("GetComboBoxInfo", {parent, ci}), 0u);
        EXPECT_EQ(get_last_error(), x86 ? kErrInvalidParameter : kUntouched);
        EXPECT_EQ(sent_.size(), x86 ? 0u : 1u);
        EXPECT_EQ(call("GetComboBoxInfo", {parent, 0}), 0u);   // cbi_custom_null
        EXPECT_EQ(get_last_error(), kErrNoAccess);
    }
}
