/// @file test_input_contracts.cpp
/// @brief Keyboard layout, key translation, injected input, hot keys, focus
///        queues, clipboard, caret, raw input, character and IME contracts
///        (Steam audit S2, 2026-09-30).
///
/// Every expected value is a line of tools/oracle_probe/s2msg_input_probe
/// .expected (Windows 11, x64; the .x86.expected file is identical for these
/// lines), and each test names the lines it pins.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <winapi/user32/resources/resources.hpp>
#include <winapi/ui/imm32/imm32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <tuple>
#include <functional>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

namespace {

constexpr u64 kBogusHwnd = 0x1234;

class InputContractTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr BUF = 0x6000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    void SetUp() override {
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        ext_ = std::make_unique<User32ExtManager>(
            memory_, vfs_, hwnd_mgr_, class_mgr_, bridge_, queue_);
        ext_->set_process_manager(&process_mgr_);
        mlm_ = std::make_unique<MessageLoopManager>(hwnd_mgr_, queue_, bridge_);
        mlm_->set_user32_ext_manager(ext_.get());
        ext_->set_message_loop_manager(mlm_.get());
        msm_ = std::make_unique<MessageSendManager>(hwnd_mgr_, queue_, bridge_);
        msm_->set_wndproc_callback([](HWND, u32, u64, u64, u64) { return 0u; });
        ext_->set_message_send_manager(msm_.get());
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        previous_tid_ = ThreadManager::get_current_thread_id_static();
        ThreadManager::set_current_thread_id(kSelf);
    }

    void TearDown() override {
        queue_.begin_shutdown();
        ThreadManager::set_current_thread_id(previous_tid_);
    }

    HWND make_window(u32 owner) {
        WindowData data{};
        data.title = L"w";
        data.wndproc = 0x401000;
        data.owner_thread_id = owner;
        data.style = WS::VISIBLE;
        data.visible = true;
        return hwnd_mgr_.create(data);
    }

    /// Call an x64 thunk; @p stack are the arguments from index 4 on.
    u64 call(const ExportEntry& e, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0,
             u64 a3 = 0, std::vector<u64> stack = {}) {
        cpu_.gpr.rcx = a0;
        cpu_.gpr.rdx = a1;
        cpu_.gpr.r8 = a2;
        cpu_.gpr.r9 = a3;
        for (size_t i = 0; i < stack.size(); ++i) {
            (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + 8 * i, stack[i]);
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        e.handler(m);
        return cpu_.gpr.rax;
    }

    static ExportEntry find(const DllExports& exports, const std::string& name) {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name;
        return {};
    }
    ExportEntry ext(const std::string& name) {
        return find(create_user32_ext_thunks(*ext_), name);
    }

    u32 err() const { return get_last_error(); }
    void seed_err() { set_last_error(0xDEAD); }

    void set_keys(std::initializer_list<std::pair<u8, u8>> keys) {
        std::array<u8, 256> state{};
        for (const auto& [vk, value] : keys) state[vk] = value;
        ASSERT_TRUE(memory_.write_bytes(BUF + 0x800, state.data(), state.size()).is_ok());
        ASSERT_EQ(call(ext("SetKeyboardState"), BUF + 0x800), 1u);
    }

    static constexpr u32 kSelf = 5;
    static constexpr u32 kOther = 6;

    GuestMemory memory_;
    capsule::Vfs vfs_;
    HwndManager hwnd_mgr_;
    WindowClassManager class_mgr_;
    CocoaBridge bridge_;
    MessageQueue queue_;
    ProcessManager process_mgr_;
    std::unique_ptr<User32ExtManager> ext_;
    std::unique_ptr<MessageLoopManager> mlm_;
    std::unique_ptr<MessageSendManager> msm_;
    cpu::CpuState cpu_;
    u32 previous_tid_ = 0;
};

// -----------------------------------------------------------------------------
// US keyboard layout
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, MapVirtualKeyMatchesTheUsLayout) {
    // mapvk: 41:1e/41/1e/1e a3:1d/0/e01d/1d 6f:35/2f/e035/35 13:0/0/e11d/0
    //        ba:27/3b/27/27 ff:0/0/0/0 (VK_TO_VSC / VK_TO_CHAR / VK_TO_VSC_EX)
    const auto map = ext("MapVirtualKeyW");
    struct Row { u32 vk, vsc, chr, vsc_ex; };
    for (const Row r : {Row{0x41, 0x1E, 0x41, 0x1E}, Row{0xA3, 0x1D, 0, 0xE01D},
                        Row{0x6F, 0x35, 0x2F, 0xE035}, Row{0x13, 0, 0, 0xE11D},
                        Row{0xBA, 0x27, 0x3B, 0x27}, Row{0xFF, 0, 0, 0}}) {
        EXPECT_EQ(call(map, r.vk, 0), r.vsc) << std::hex << r.vk;
        EXPECT_EQ(call(map, r.vk, 2), r.chr) << std::hex << r.vk;
        EXPECT_EQ(call(map, r.vk, 4), r.vsc_ex) << std::hex << r.vk;
    }
    // mapvsc: 2a:10/a0 36:10/a1 5b:f1/f1 (VSC_TO_VK / VSC_TO_VK_EX)
    EXPECT_EQ(call(map, 0x2A, 1), 0x10u);
    EXPECT_EQ(call(map, 0x2A, 3), 0xA0u);
    EXPECT_EQ(call(map, 0x36, 3), 0xA1u);
    EXPECT_EQ(call(map, 0x5B, 1), 0xF1u);
    // mapvk_ex_e01d = a3, mapvk_ex_e038 = a5
    EXPECT_EQ(call(ext("MapVirtualKeyExW"), 0xE01D, 3, 0), 0xA3u);
    EXPECT_EQ(call(ext("MapVirtualKeyExW"), 0xE038, 3, 0), 0xA5u);
    // mapvk_type5 / mapvk_type7: 0 / ERROR_INVALID_PARAMETER
    seed_err();
    EXPECT_EQ(call(map, 'A', 5), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    seed_err();
    EXPECT_EQ(call(map, 'A', 7), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(InputContractTest, VkKeyScanReportsShiftStateAndUnmappableCharacters) {
    // vkkeyscanexw_a 0041, vkkeyscanexa_A 0141, vkkeyscanexa_bang 0131,
    // vkkeyscanexa_ctrl_a 0241, vkkeyscanexw_euro ffff, vkkeyscanexa_e9 ffff
    const auto scan = ext("VkKeyScanW");
    EXPECT_EQ(call(scan, 'a') & 0xFFFF, 0x0041u);
    EXPECT_EQ(call(scan, 'A') & 0xFFFF, 0x0141u);
    EXPECT_EQ(call(scan, '!') & 0xFFFF, 0x0131u);
    EXPECT_EQ(call(scan, 1) & 0xFFFF, 0x0241u);
    EXPECT_EQ(call(scan, 0x20AC) & 0xFFFF, 0xFFFFu);
    EXPECT_EQ(call(ext("VkKeyScanA"), 0xE9) & 0xFFFF, 0xFFFFu);
}

TEST_F(InputContractTest, ToUnicodeTranslatesWithTheGivenKeyState) {
    const auto to_unicode = ext("ToUnicode");
    const GuestAddr ks = BUF;
    const GuestAddr out = BUF + 0x200;
    const auto run = [&](u32 vk, u32 scan, std::initializer_list<std::pair<u8, u8>> keys,
                         u32 cch = 8) {
        std::array<u8, 256> state{};
        for (const auto& [k, v] : keys) state[k] = v;
        (void)memory_.write_bytes(ks, state.data(), state.size());
        (void)memory_.write_u16(out, 0);
        return static_cast<i32>(call(to_unicode, vk, scan, ks, out, {cch, 0}));
    };
    // tounicode_a 1 char=61; _A_shift 41; _ctrl_a 1; _caps_a 41; _return d
    EXPECT_EQ(run('A', 0x1E, {}), 1);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x61u);
    EXPECT_EQ(run('A', 0x1E, {{0x10, 0x80}}), 1);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x41u);
    EXPECT_EQ(run('A', 0x1E, {{0x11, 0x80}}), 1);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x01u);
    EXPECT_EQ(run('A', 0x1E, {{0x14, 0x01}}), 1);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x41u);
    EXPECT_EQ(run(0x0D, 0x1C, {}), 1);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x0Du);
    // tounicode_f1 0; tounicode_keyup_bit 0; tounicode_ctrl_alt_a 0
    EXPECT_EQ(run(0x70, 0x3B, {}), 0);
    EXPECT_EQ(run('A', 0x801E, {}), 0);
    EXPECT_EQ(run('A', 0x1E, {{0x11, 0x80}, {0x12, 0x80}}), 0);
    // tounicode_zero_buffer 0 / ERROR_INVALID_PARAMETER
    seed_err();
    EXPECT_EQ(run('A', 0x1E, {}, 0), 0);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(InputContractTest, TranslateMessagePostsCharactersForKeyMessagesOnly) {
    const HWND w = make_window(kSelf);
    const auto translate = find(make_message_loop_exports(*mlm_), "TranslateMessage");
    const GuestAddr msg = BUF + 0x400;
    const auto run = [&](u32 message, u64 wparam, u64 lparam,
                         std::initializer_list<std::pair<u8, u8>> keys) {
        set_keys(keys);
        MSG drained{};
        while (mlm_->peek_message(drained, 0, 0, 0, PM::REMOVE)) {}
        (void)memory_.write_u64(msg + 0, w);
        (void)memory_.write_u32(msg + 8, message);
        (void)memory_.write_u64(msg + 16, wparam);
        (void)memory_.write_u64(msg + 24, lparam);
        const u64 ret = call(translate, msg);
        std::vector<std::array<u64, 3>> posted;
        MSG m{};
        while (mlm_->peek_message(m, 0, 0, 0, PM::REMOVE)) {
            posted.push_back({m.message, m.wParam, m.lParam});
        }
        return std::make_pair(ret, posted);
    };
    using Posted = std::vector<std::array<u64, 3>>;
    // translate_keydown_a ret=1 posted: 102/61/1e0001
    EXPECT_EQ(run(WM::KEYDOWN, 'A', 0x1E0001, {}),
              std::make_pair(u64{1}, Posted{{0x102, 0x61, 0x1E0001}}));
    // translate_keyup_a ret=1 posted: (none); translate_keydown_f1 ret=1
    EXPECT_EQ(run(WM::KEYUP, 'A', 0xC01E0001, {}), std::make_pair(u64{1}, Posted{}));
    EXPECT_EQ(run(WM::KEYDOWN, 0x70, 0x3B0001, {}), std::make_pair(u64{1}, Posted{}));
    // translate_syskeydown_a_alt ret=1 posted: 106/61/201e0001
    EXPECT_EQ(run(WM::SYSKEYDOWN, 'A', 0x201E0001, {{0x12, 0x80}}),
              std::make_pair(u64{1}, Posted{{0x106, 0x61, 0x201E0001}}));
    // translate_ctrl_shift_2 posted: 102/0/30001; translate_ctrl_2 nothing
    EXPECT_EQ(run(WM::KEYDOWN, '2', 0x30001, {{0x10, 0x80}, {0x11, 0x80}}),
              std::make_pair(u64{1}, Posted{{0x102, 0, 0x30001}}));
    EXPECT_EQ(run(WM::KEYDOWN, '2', 0x30001, {{0x11, 0x80}}), std::make_pair(u64{1}, Posted{}));
    // translate_ctrl_alt_a ret=1 nothing; translate_char_msg ret=0;
    // translate_lbuttondown ret=0
    EXPECT_EQ(run(WM::KEYDOWN, 'A', 0x1E0001, {{0x11, 0x80}, {0x12, 0x80}}),
              std::make_pair(u64{1}, Posted{}));
    EXPECT_EQ(run(WM::CHAR_MSG, 'a', 0x1E0001, {}), std::make_pair(u64{0}, Posted{}));
    EXPECT_EQ(run(WM::LBUTTONDOWN, 0, 0, {}), std::make_pair(u64{0}, Posted{}));
}

TEST_F(InputContractTest, GetKeyStateIsSignExtendedFromTheThreadTable) {
    // getkeystate_A_down ffff80 (SHORT 0xFF80), _capital_toggled 1, _B_up 0;
    // getasync_A_not_set_by_setkeyboardstate 0.
    set_keys({{'A', 0x80}, {0x14, 0x01}});
    EXPECT_EQ(call(ext("GetKeyState"), 'A') & 0xFFFF, 0xFF80u);
    EXPECT_EQ(call(ext("GetKeyState"), 0x14) & 0xFFFF, 0x0001u);
    EXPECT_EQ(call(ext("GetKeyState"), 'B') & 0xFFFF, 0u);
}

// -----------------------------------------------------------------------------
// SendInput / hot keys
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, SendInputValidatesThenDeliversKeystrokesToTheFocusWindow) {
    const auto send = ext("SendInput");
    const GuestAddr in = BUF + 0x100;
    std::array<u8, 40> zero{};
    (void)memory_.write_bytes(in, zero.data(), zero.size());
    (void)memory_.write_u32(in, 1);  // INPUT_KEYBOARD
    // sendinput_zero / _bad_size: 0 / ERROR_INVALID_PARAMETER; _null: 0 /
    // ERROR_NOACCESS (x64); _bad_type: counted (1).
    seed_err();
    EXPECT_EQ(call(send, 0, in, 40), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    seed_err();
    EXPECT_EQ(call(send, 1, in, 36), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    seed_err();
    EXPECT_EQ(call(send, 1, 0, 40), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_NOACCESS);
    (void)memory_.write_u32(in, 7);
    EXPECT_EQ(call(send, 1, in, 40), 1u);

    // A keystroke reaches the focus window as hardware input, with the scan
    // code MapVirtualKey gives when wScan is 0 (KEYBDINPUT, Learn).
    const HWND w = make_window(kSelf);
    (void)call(ext("SetFocus"), w);
    MSG drained{};
    while (mlm_->peek_message(drained, 0, 0, 0, PM::REMOVE)) {}
    (void)memory_.write_u32(in, 1);
    (void)memory_.write_u16(in + 8, 'Q');
    EXPECT_EQ(call(send, 1, in, 40), 1u);
    MSG m{};
    ASSERT_TRUE(mlm_->peek_message(m, 0, WM::KEYDOWN, WM::KEYDOWN, PM::REMOVE));
    EXPECT_EQ(m.hwnd, w);
    EXPECT_EQ(m.wParam, u64{'Q'});
    EXPECT_EQ(m.lParam, 0x100001u);  // repeat 1, scan 0x10
    EXPECT_NE(call(ext("GetAsyncKeyState"), 'Q') & 0x8000, 0u);
    (void)memory_.write_u32(in + 12, 2);  // KEYEVENTF_KEYUP
    EXPECT_EQ(call(send, 1, in, 40), 1u);
    ASSERT_TRUE(mlm_->peek_message(m, 0, WM::KEYUP, WM::KEYUP, PM::REMOVE));
    EXPECT_EQ(m.lParam, 0xC0100001u);
    EXPECT_EQ(call(ext("GetAsyncKeyState"), 'Q') & 0x8000, 0u);
}

TEST_F(InputContractTest, MessageExtraInfoFollowsTheLastRetrievedMessage) {
    const HWND w = make_window(kSelf);
    (void)call(ext("SetFocus"), w);
    MSG drained{};
    while (mlm_->peek_message(drained, 0, 0, 0, PM::REMOVE)) {}
    const auto get = ext("GetMessageExtraInfo");
    const auto set = ext("SetMessageExtraInfo");
    // set_55 prev=0 err=dead now=55
    (void)call(set, 0);
    seed_err();
    EXPECT_EQ(call(set, 0x55), 0u);
    EXPECT_EQ(err(), 0xDEADu);
    EXPECT_EQ(call(get), 0x55u);
    // after_empty_peek value=66: a peek that finds nothing leaves it
    (void)call(set, 0x66);
    EXPECT_FALSE(mlm_->peek_message(drained, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(call(get), 0x66u);
    // after_noremove_peek=0: retrieving a posted message, even PM_NOREMOVE
    MSG posted{};
    posted.hwnd = w;
    posted.message = 0x8000;
    posted.target_thread_id = kSelf;
    ASSERT_TRUE(queue_.post(posted));
    ASSERT_TRUE(mlm_->peek_message(drained, 0, 0, 0, PM::NOREMOVE));
    EXPECT_EQ(call(get), 0u);
    ASSERT_TRUE(mlm_->peek_message(drained, 0, 0, 0, PM::REMOVE));
    // Hardware input carries its event's dwExtraInfo (KEYBDINPUT, Learn).
    const GuestAddr in = BUF + 0x100;
    std::array<u8, 40> zero{};
    (void)memory_.write_bytes(in, zero.data(), zero.size());
    (void)memory_.write_u32(in, 1);           // INPUT_KEYBOARD
    (void)memory_.write_u16(in + 8, 'Q');
    (void)memory_.write_u64(in + 8 + 16, 0xFF515700ull);  // dwExtraInfo
    ASSERT_EQ(call(ext("SendInput"), 1, in, 40), 1u);
    ASSERT_TRUE(mlm_->peek_message(drained, 0, WM::KEYDOWN, WM::KEYDOWN, PM::REMOVE));
    EXPECT_EQ(call(get), 0xFF515700u);
}

TEST_F(InputContractTest, RegisterHotKeyErrorsMatchWindows) {
    const HWND w = make_window(kSelf);
    const auto reg = ext("RegisterHotKey");
    const auto unreg = ext("UnregisterHotKey");
    // registerhotkey 1, registerhotkey_same_id 1, _invalid_hwnd 0/1400,
    // _bad_mods 0/1004; unregisterhotkey 1, _again 0/1419, _invalid_hwnd
    // 0/1400.
    EXPECT_EQ(call(reg, w, 7, 0x2 | 0x4, 0x7A), 1u);
    EXPECT_EQ(call(reg, w, 7, 0x2 | 0x4, 0x7A), 1u);
    seed_err();
    EXPECT_EQ(call(reg, kBogusHwnd, 7, 0x2, 0x7A), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    seed_err();
    EXPECT_EQ(call(reg, w, 8, 0x80, 0x7A), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(call(unreg, w, 7), 1u);
    seed_err();
    EXPECT_EQ(call(unreg, w, 7), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_HOTKEY_NOT_REGISTERED);
    seed_err();
    EXPECT_EQ(call(unreg, kBogusHwnd, 7), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

// -----------------------------------------------------------------------------
// Focus and capture follow the input queue
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, AttachThreadInputSharesFocusBetweenQueues) {
    ext_->set_thread_alive_query([](u32 tid) { return tid == kSelf || tid == kOther; });
    const HWND other = make_window(kOther);
    const auto attach = ext("AttachThreadInput");
    // The other thread pumps its queue, so the WM_SETFOCUS that focusing its
    // window sends is answered (s2msg_input_probe runs a pumping thread).
    std::atomic<bool> stop{false};
    std::thread pump([&] {
        ThreadManager::set_current_thread_id(kOther);
        MSG m{};
        while (!stop.load()) {
            (void)mlm_->peek_message(m, 0, 0, 0, PM::REMOVE);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    struct Join {
        std::atomic<bool>& stop;
        std::thread& t;
        ~Join() { stop.store(true); t.join(); }
    } join{stop, pump};
    // setfocus_other_thread_null: NULL / ERROR_ACCESS_DENIED
    seed_err();
    EXPECT_EQ(call(ext("SetFocus"), other), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_ACCESS_DENIED);
    // attach_self_self, attach_bogus, attach_zero, detach_not_attached:
    // 0 / ERROR_INVALID_PARAMETER
    for (const auto& [from, to, flag] :
         {std::tuple<u32, u32, u32>{kSelf, kSelf, 1}, {0x7FFFFFF0, kSelf, 1},
          {0, kSelf, 1}, {kOther, kSelf, 0}}) {
        seed_err();
        EXPECT_EQ(call(attach, from, to, flag), 0u);
        EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    }
    // attach_other 1; attached_focus_other_thread_ok err=0;
    // attached_getfocus_other; detach_other 1; detach_again 0 / 87
    EXPECT_EQ(call(attach, kOther, kSelf, 1), 1u);
    seed_err();
    (void)call(ext("SetFocus"), other);
    EXPECT_EQ(err(), 0u);
    EXPECT_EQ(call(ext("GetFocus")), other);
    EXPECT_EQ(call(attach, kOther, kSelf, 0), 1u);
    seed_err();
    EXPECT_EQ(call(attach, kOther, kSelf, 0), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(InputContractTest, CaptureCallsSetTheLastErrorLikeWindows) {
    const HWND w = make_window(kSelf);
    // setcapture_prev_none: NULL / err 0; releasecapture 1 / err 0;
    // releasecapture_none 1 / untouched; setcapture_invalid NULL / 1400;
    // setcapture_null NULL / untouched.
    seed_err();
    EXPECT_EQ(call(ext("SetCapture"), w), 0u);
    EXPECT_EQ(err(), 0u);
    EXPECT_EQ(call(ext("GetCapture")), w);
    seed_err();
    EXPECT_EQ(call(ext("ReleaseCapture")), 1u);
    EXPECT_EQ(err(), 0u);
    seed_err();
    EXPECT_EQ(call(ext("ReleaseCapture")), 1u);
    EXPECT_EQ(err(), 0xDEADu);
    seed_err();
    EXPECT_EQ(call(ext("SetCapture"), kBogusHwnd), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    seed_err();
    EXPECT_EQ(call(ext("SetCapture"), 0), 0u);
    EXPECT_EQ(err(), 0xDEADu);
}

// -----------------------------------------------------------------------------
// Clipboard
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, ClipboardOwnershipSynthesisAndSequenceMatchWindows) {
    VirtualMemoryManager vmm(memory_);
    HandleTable handles;
    HeapManager heap(vmm, memory_, handles);
    GlobalLocalManager globals(heap);
    ext_->set_global_local_manager(&globals);
    const std::string previous = bridge_.clipboard_get_text();
    const HWND w = make_window(kSelf);

    // *_not_open: 0 / ERROR_CLIPBOARD_NOT_OPEN
    for (const char* name : {"CloseClipboard", "EmptyClipboard"}) {
        seed_err();
        EXPECT_EQ(call(ext(name)), 0u) << name;
        EXPECT_EQ(err(), error_codes::ERROR_CLIPBOARD_NOT_OPEN) << name;
    }
    seed_err();
    EXPECT_EQ(call(ext("GetClipboardData"), 13), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_CLIPBOARD_NOT_OPEN);

    // openclipboard 1, openclipboard_again_same_thread 1; emptyclipboard
    // advances the sequence number.
    EXPECT_EQ(call(ext("OpenClipboard"), w), 1u);
    EXPECT_EQ(call(ext("OpenClipboard"), w), 1u);
    const u64 seq0 = call(ext("GetClipboardSequenceNumber"));
    EXPECT_EQ(call(ext("EmptyClipboard")), 1u);
    EXPECT_GT(call(ext("GetClipboardSequenceNumber")), seq0);

    // openclipboard_other_thread 0 / ERROR_ACCESS_DENIED
    ThreadManager::set_current_thread_id(kOther);
    seed_err();
    EXPECT_EQ(call(ext("OpenClipboard"), 0), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_ACCESS_DENIED);
    ThreadManager::set_current_thread_id(kSelf);

    // setclipboarddata_unicode: the handle / err 0
    const u64 h = globals.global_alloc(GHND, 16);
    Marshaler m(memory_);
    (void)m.write_wide_string(globals.global_lock(h), "hi");
    (void)globals.global_unlock(h);
    seed_err();
    EXPECT_EQ(call(ext("SetClipboardData"), 13, h), h);
    EXPECT_EQ(err(), 0u);
    // Synthesized formats appear only after CloseClipboard:
    // isformat_text_synth 0, enumformats: d end_err=0.
    EXPECT_EQ(call(ext("IsClipboardFormatAvailable"), 1), 0u);
    std::vector<u64> formats;
    u64 f = 0;
    while ((f = call(ext("EnumClipboardFormats"), f)) != 0) formats.push_back(f);
    EXPECT_EQ(formats, (std::vector<u64>{13}));
    EXPECT_EQ(err(), 0u);
    // getclipboarddata_absent: NULL / ERROR_NOT_FOUND
    seed_err();
    EXPECT_EQ(call(ext("GetClipboardData"), 2), 0u);
    EXPECT_EQ(err(), 1168u);
    EXPECT_EQ(call(ext("CloseClipboard")), 1u);
    seed_err();
    EXPECT_EQ(call(ext("CloseClipboard")), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_CLIPBOARD_NOT_OPEN);

    // isformat_text_after_close 1, isformat_locale_after_close 1;
    // enumformats_after_close: d 10 1 7; getclipboarddata_text_after_close
    // text=hi.
    EXPECT_EQ(call(ext("IsClipboardFormatAvailable"), 1), 1u);
    EXPECT_EQ(call(ext("IsClipboardFormatAvailable"), 16), 1u);
    EXPECT_EQ(call(ext("OpenClipboard"), 0), 1u);
    formats.clear();
    f = 0;
    while ((f = call(ext("EnumClipboardFormats"), f)) != 0) formats.push_back(f);
    EXPECT_EQ(formats, (std::vector<u64>{13, 16, 1, 7}));
    const u64 text = call(ext("GetClipboardData"), 1);
    ASSERT_NE(text, 0u);
    const GuestAddr p = globals.global_lock(text);
    EXPECT_EQ(memory_.read_u8(p).value(), 'h');
    EXPECT_EQ(memory_.read_u8(p + 1).value(), 'i');
    EXPECT_EQ(memory_.read_u8(p + 2).value(), 0u);
    (void)globals.global_unlock(text);
    EXPECT_EQ(call(ext("CloseClipboard")), 1u);
    // enumformats_closed: 0 / ERROR_CLIPBOARD_NOT_OPEN
    seed_err();
    EXPECT_EQ(call(ext("EnumClipboardFormats"), 0), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_CLIPBOARD_NOT_OPEN);

    // GetUpdatedClipboardFormats needs no open clipboard
    // (clipformats_gesture_probe{32,64}.expected "== clipboard"):
    // after-close -> 1 out=4 list=d 10 1 7; cap1 / cap0 -> 0 err=122 out=4
    // with the buffer untouched; null-out -> 0 err=998.
    {
        const GuestAddr list = MEM_BASE + 0xE000;
        const GuestAddr out = MEM_BASE + 0xE100;
        for (u32 i = 0; i < 8; ++i) (void)memory_.write_u32(list + i * 4, 0xEEEE);
        EXPECT_EQ(call(ext("GetUpdatedClipboardFormats"), list, 16, out), 1u);
        EXPECT_EQ(memory_.read_u32(out).value(), 4u);
        std::vector<u32> got;
        for (u32 i = 0; i < 5; ++i) got.push_back(memory_.read_u32(list + i * 4).value());
        EXPECT_EQ(got, (std::vector<u32>{13, 16, 1, 7, 0xEEEE}));
        for (u32 i = 0; i < 8; ++i) (void)memory_.write_u32(list + i * 4, 0xEEEE);
        seed_err();
        EXPECT_EQ(call(ext("GetUpdatedClipboardFormats"), list, 1, out), 0u);
        EXPECT_EQ(err(), error_codes::ERROR_INSUFFICIENT_BUFFER);
        EXPECT_EQ(memory_.read_u32(out).value(), 4u);
        EXPECT_EQ(memory_.read_u32(list).value(), 0xEEEEu);
        seed_err();
        EXPECT_EQ(call(ext("GetUpdatedClipboardFormats"), 0, 0, out), 0u);
        EXPECT_EQ(err(), error_codes::ERROR_INSUFFICIENT_BUFFER);
        seed_err();
        EXPECT_EQ(call(ext("GetUpdatedClipboardFormats"), list, 16, 0), 0u);
        EXPECT_EQ(err(), error_codes::ERROR_NOACCESS);
    }

    (void)bridge_.clipboard_set_text(previous);
    ext_->set_global_local_manager(nullptr);
}

// -----------------------------------------------------------------------------
// Caret
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, CaretCallsMatchWindows) {
    const HWND w = make_window(kSelf);
    const HWND w2 = make_window(kSelf);
    const HWND other = make_window(kOther);
    // *_no_caret: 0 / ERROR_ACCESS_DENIED
    for (const char* name : {"HideCaret", "ShowCaret", "DestroyCaret"}) {
        seed_err();
        EXPECT_EQ(call(ext(name), 0), 0u) << name;
        EXPECT_EQ(err(), error_codes::ERROR_ACCESS_DENIED) << name;
    }
    seed_err();
    EXPECT_EQ(call(ext("SetCaretPos"), 3, 4), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_ACCESS_DENIED);
    // createcaret_invalid 0 / 1400; createcaret_other_thread 0 / untouched
    seed_err();
    EXPECT_EQ(call(ext("CreateCaret"), kBogusHwnd, 0, 2, 10), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    seed_err();
    EXPECT_EQ(call(ext("CreateCaret"), other, 0, 2, 10), 0u);
    EXPECT_EQ(err(), 0xDEADu);
    // createcaret, setcaretpos, getcaretpos pt=5,6, showcaret x2, hidecaret
    EXPECT_EQ(call(ext("CreateCaret"), w, 0, 2, 10), 1u);
    EXPECT_EQ(call(ext("SetCaretPos"), 5, 6), 1u);
    EXPECT_EQ(call(ext("GetCaretPos"), BUF), 1u);
    EXPECT_EQ(memory_.read_u32(BUF).value(), 5u);
    EXPECT_EQ(memory_.read_u32(BUF + 4).value(), 6u);
    EXPECT_EQ(call(ext("ShowCaret"), w), 1u);
    EXPECT_EQ(call(ext("ShowCaret"), w), 1u);
    EXPECT_EQ(call(ext("HideCaret"), w), 1u);
    // hidecaret_other_window 0 / untouched; hidecaret_null_window 1
    seed_err();
    EXPECT_EQ(call(ext("HideCaret"), w2), 0u);
    EXPECT_EQ(err(), 0xDEADu);
    EXPECT_EQ(call(ext("HideCaret"), 0), 1u);
    // destroycaret 1; destroycaret_again 0 / ERROR_ACCESS_DENIED
    EXPECT_EQ(call(ext("DestroyCaret")), 1u);
    seed_err();
    EXPECT_EQ(call(ext("DestroyCaret")), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_ACCESS_DENIED);
}

// -----------------------------------------------------------------------------
// Raw input and mouse tracking
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, RawInputRegistrationErrorsMatchWindows) {
    const HWND w = make_window(kSelf);
    const auto reg = ext("RegisterRawInputDevices");
    const GuestAddr rid = BUF + 0x100;  // RAWINPUTDEVICE x64: 16 bytes
    const auto device = [&](u32 flags, u64 target) {
        (void)memory_.write_u16(rid + 0, 1);  // usage page
        (void)memory_.write_u16(rid + 2, 2);  // mouse
        (void)memory_.write_u32(rid + 4, flags);
        (void)memory_.write_u64(rid + 8, target);
    };
    device(0, 0);
    // rid_register_bad_size / _zero / _null: 0 / ERROR_INVALID_PARAMETER
    for (const auto& [ptr, count, size] :
         {std::tuple<u64, u64, u64>{rid, 1, 8}, {rid, 0, 16}, {0, 1, 16}}) {
        seed_err();
        EXPECT_EQ(call(reg, ptr, count, size), 0u);
        EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    }
    // rid_register_mouse 1; rid_inputsink_no_hwnd 0/87; rid_inputsink_hwnd 1;
    // rid_remove_with_hwnd 0/87; rid_remove 1; rid_bad_flags 0/1004.
    EXPECT_EQ(call(reg, rid, 1, 16), 1u);
    device(0x100, 0);
    seed_err();
    EXPECT_EQ(call(reg, rid, 1, 16), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    device(0x100, w);
    EXPECT_EQ(call(reg, rid, 1, 16), 1u);
    device(0x1, w);
    seed_err();
    EXPECT_EQ(call(reg, rid, 1, 16), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    device(0x1, 0);
    EXPECT_EQ(call(reg, rid, 1, 16), 1u);
    device(0x12345, 0);
    seed_err();
    EXPECT_EQ(call(reg, rid, 1, 16), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_FLAGS);
}

TEST_F(InputContractTest, TrackMouseEventValidatesSizeThenWindow) {
    const auto track = ext("TrackMouseEvent");
    const GuestAddr tme = BUF + 0x100;  // TRACKMOUSEEVENT x64: 24 bytes
    (void)memory_.write_u32(tme + 0, 4);
    (void)memory_.write_u32(tme + 4, 2);  // TME_LEAVE
    (void)memory_.write_u64(tme + 8, kBogusHwnd);
    // trackmouse_bad_size 0/87; trackmouse_invalid_hwnd 0/1400
    seed_err();
    EXPECT_EQ(call(track, tme), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    (void)memory_.write_u32(tme + 0, 24);
    seed_err();
    EXPECT_EQ(call(track, tme), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

// -----------------------------------------------------------------------------
// Characters
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, CharCaseAndNavigationUseTheUserTables) {
    ResourceManager resources(memory_);
    const DllExports res = create_resource_thunks(resources);
    // charupperw_char 41, charupperw_char_e_acute c9 (a character in the low
    // word, not a pointer)
    EXPECT_EQ(call(find(res, "CharUpperW"), 'a'), 0x41u);
    EXPECT_EQ(call(find(res, "CharUpperW"), 0xE9), 0xC9u);
    // charupperw_str 41 c9 391 5a
    // Above 0xFFFF: a lower value is a character (IS_INTRESOURCE), not a string.
    const GuestAddr s = STACK_BASE + 0x4000;
    const std::array<u16, 5> up{'a', 0xE9, 0x3B1, 'z', 0};
    (void)memory_.write_bytes(s, reinterpret_cast<const u8*>(up.data()), 10);
    EXPECT_EQ(call(find(res, "CharUpperW"), s), s);
    EXPECT_EQ(memory_.read_u16(s + 0).value(), 0x41u);
    EXPECT_EQ(memory_.read_u16(s + 2).value(), 0xC9u);
    EXPECT_EQ(memory_.read_u16(s + 4).value(), 0x391u);
    EXPECT_EQ(memory_.read_u16(s + 6).value(), 0x5Au);
    // charlowerbuffw_ret 5, charlowerbuffw 61 e9 3b1 130 7a (U+0130 has no
    // one-to-one lower case in USER's table), charlowerbuffw_zero 0
    const std::array<u16, 6> low{'A', 0xC9, 0x391, 0x130, 'Z', 0};
    (void)memory_.write_bytes(s, reinterpret_cast<const u8*>(low.data()), 12);
    EXPECT_EQ(call(find(res, "CharLowerBuffW"), s, 5), 5u);
    const std::array<u16, 5> want{0x61, 0xE9, 0x3B1, 0x130, 0x7A};
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(memory_.read_u16(s + 2 * i).value(), want[i]) << i;
    }
    EXPECT_EQ(call(find(res, "CharLowerBuffW"), s, 0), 0u);

    // charnextw_combining 2, _surrogate 4, _at_end 6; charprevw_start 0,
    // _after_combining 0, _after_surrogate 4 (units from the start).
    const std::array<u16, 7> nav{'a', 0x0301, 'b', 0xD83D, 0xDE00, 'c', 0};
    (void)memory_.write_bytes(s, reinterpret_cast<const u8*>(nav.data()), 14);
    const auto next = ext("CharNextW");
    const auto prev = ext("CharPrevW");
    EXPECT_EQ((call(next, s) - s) / 2, 2u);
    EXPECT_EQ((call(next, s + 6) - s) / 2, 4u);
    EXPECT_EQ((call(next, s + 12) - s) / 2, 6u);
    EXPECT_EQ((call(prev, s, s) - s) / 2, 0u);
    EXPECT_EQ((call(prev, s, s + 4) - s) / 2, 0u);
    EXPECT_EQ((call(prev, s, s + 10) - s) / 2, 4u);
}

TEST_F(InputContractTest, CharToOemUsesUnderscoreForUnmappableCharacters) {
    // chartooembuffw oem=82 41 5f b0: U+20AC has no CP437 form and becomes
    // '_' (not WideCharToMultiByte's '?'); U+2591 is 0xB0.
    const GuestAddr w = BUF + 0x100;
    const GuestAddr o = BUF + 0x200;
    const std::array<u16, 5> text{0xE9, 'A', 0x20AC, 0x2591, 0};
    (void)memory_.write_bytes(w, reinterpret_cast<const u8*>(text.data()), 10);
    EXPECT_EQ(call(ext("CharToOemBuffW"), w, o, 4), 1u);
    EXPECT_EQ(memory_.read_u8(o + 0).value(), 0x82u);
    EXPECT_EQ(memory_.read_u8(o + 1).value(), 0x41u);
    EXPECT_EQ(memory_.read_u8(o + 2).value(), 0x5Fu);
    EXPECT_EQ(memory_.read_u8(o + 3).value(), 0xB0u);
}

// -----------------------------------------------------------------------------
// ANSI / Unicode character messages (tools/oracle_probe/s2msg_ansi_msg_probe)
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, CharacterMessagesCrossTheAnsiBoundaryInTheQueue) {
    const HWND w = make_window(kSelf);
    const DllExports loop = make_message_loop_exports(*mlm_);
    const DllExports send = make_message_send_exports(*msm_);
    const GuestAddr msg = BUF + 0x400;
    const auto post = [&](const char* api, u32 message, u64 wparam) {
        EXPECT_EQ(call(find(send, api), w, message, wparam, 0x11), 1u) << api;
    };
    const auto get = [&](const char* api) {
        (void)memory_.write_u64(msg + 16, 0xDEAD);
        EXPECT_EQ(call(find(loop, api), msg, w, 0, 0), 1u) << api;
        return std::make_pair(memory_.read_u32(msg + 8).value(),
                              memory_.read_u64(msg + 16).value());
    };
    // postw_geta_char_euro w=80, _alpha w=61 (best fit), _surrogate w=3f,
    // _high_bits w=3f80 (x64: two UTF-16 units), _imechar_euro w=80
    post("PostMessageW", WM::CHAR_MSG, 0x20AC);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x102}, u64{0x80}));
    post("PostMessageW", WM::CHAR_MSG, 0x3B1);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x102}, u64{0x61}));
    post("PostMessageW", WM::CHAR_MSG, 0xD83D);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x102}, u64{0x3F}));
    post("PostMessageW", WM::CHAR_MSG, 0x100020AC);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x102}, u64{0x3F80}));
    post("PostMessageW", 0x286, 0x20AC);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x286}, u64{0x80}));
    // postw_geta_keydown / postw_geta_app: untouched
    post("PostMessageW", WM::KEYDOWN, 0x20AC);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x100}, u64{0x20AC}));
    post("PostMessageW", 0x8000, 0x20AC);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x8000}, u64{0x20AC}));
    // posta_getw_char_80 w=20ac, _81 w=81, _high_bits (0x12340080) w=20ac,
    // posta_getw_app w=80, posta_geta_char_80 w=80
    post("PostMessageA", WM::CHAR_MSG, 0x80);
    EXPECT_EQ(get("GetMessageW"), std::make_pair(u32{0x102}, u64{0x20AC}));
    post("PostMessageA", WM::CHAR_MSG, 0x81);
    EXPECT_EQ(get("GetMessageW"), std::make_pair(u32{0x102}, u64{0x81}));
    post("PostMessageA", WM::CHAR_MSG, 0x12340080);
    EXPECT_EQ(get("GetMessageW"), std::make_pair(u32{0x102}, u64{0x20AC}));
    post("PostMessageA", 0x8000, 0x80);
    EXPECT_EQ(get("GetMessageW"), std::make_pair(u32{0x8000}, u64{0x80}));
    post("PostMessageA", WM::CHAR_MSG, 0x80);
    EXPECT_EQ(get("GetMessageA"), std::make_pair(u32{0x102}, u64{0x80}));
    // postw_peek_noremove_a: PeekMessageA(PM_NOREMOVE) sees 80, the queued
    // message is still 20ac.
    post("PostMessageW", WM::CHAR_MSG, 0x20AC);
    EXPECT_EQ(call(find(loop, "PeekMessageA"), msg, w, 0, 0, {PM::NOREMOVE}), 1u);
    EXPECT_EQ(memory_.read_u64(msg + 16).value(), 0x80u);
    EXPECT_EQ(call(find(loop, "PeekMessageW"), msg, w, 0, 0, {PM::REMOVE}), 1u);
    EXPECT_EQ(memory_.read_u64(msg + 16).value(), 0x20ACu);
    // postthreada_getw_char_80 w=20ac
    EXPECT_EQ(call(find(send, "PostThreadMessageA"), kSelf, WM::CHAR_MSG, 0x80, 0), 1u);
    EXPECT_EQ(call(find(loop, "GetMessageW"), msg, static_cast<u64>(-1), 0, 0), 1u);
    EXPECT_EQ(memory_.read_u64(msg + 16).value(), 0x20ACu);
}

TEST_F(InputContractTest, DispatchMessageAHandsAUnicodeProcedureTheUnicodeCharacter) {
    const HWND w = make_window(kSelf);
    std::vector<u64> seen;
    mlm_->set_wndproc_callback([&](HWND, u32, u64 wparam, u64, u64) -> u64 {
        seen.push_back(wparam);
        return 0;
    });
    const auto dispatch = find(make_message_loop_exports(*mlm_), "DispatchMessageA");
    const GuestAddr msg = BUF + 0x400;
    const auto run = [&](u32 message, u64 wparam) {
        (void)memory_.write_u64(msg + 0, w);
        (void)memory_.write_u32(msg + 8, message);
        (void)memory_.write_u64(msg + 16, wparam);
        (void)memory_.write_u64(msg + 24, 0);
        (void)call(dispatch, msg);
    };
    // disp_postw_geta_dispa_to_w proc_w=20ac; disp_postw_getw_dispa_to_w
    // (a Unicode value dispatched as ANSI) proc_w=2000ac; application
    // messages untouched.
    run(WM::CHAR_MSG, 0x80);
    run(WM::CHAR_MSG, 0x20AC);
    run(0x8000, 0x80);
    EXPECT_EQ(seen, (std::vector<u64>{0x20AC, 0x2000AC, 0x80}));
}

// -----------------------------------------------------------------------------
// MessageBox (tools/oracle_probe/s2msg_msgbox_probe)
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, MessageBoxValidatesOwnerAndStyleAndIndirectReadsTheX64Layout) {
    // No host is attached here, so a shown box answers its default button.
    ResourceManager resources(memory_);
    resources.set_hwnd_manager(&hwnd_mgr_);
    ext_->set_resource_manager(&resources);
    // msgboxw_bogus_owner 0 / 1400; msgboxw_bad_button_type (7) 0 / 1438;
    // msgboxw_yesno_defbutton2 7 / err 0; msgboxw_bad_icon (0x50) shown.
    seed_err();
    EXPECT_EQ(resources.message_box(static_cast<HWND>(kBogusHwnd), "c", "t", 0), 0);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    seed_err();
    EXPECT_EQ(resources.message_box(0, "c", "t", 0x7), 0);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_MSGBOX_STYLE);
    seed_err();
    EXPECT_EQ(resources.message_box(0, "c", "t", 0x4 | 0x100), 7);
    EXPECT_EQ(err(), 0u);
    EXPECT_EQ(resources.message_box(0, "c", "t", 0x50), 1);

    // MSGBOXPARAMSW on x64: hwndOwner@8 lpszText@24 lpszCaption@32
    // dwStyle@40. indirect_yesno 6; indirect_bogus_owner 0 / 1400;
    // indirect_cbsize_zero shown (cbSize is not checked).
    const GuestAddr p = STACK_BASE + 0x4000;
    const GuestAddr text = STACK_BASE + 0x4100;
    Marshaler m(memory_);
    (void)m.write_wide_string(text, "t");
    std::array<u8, 80> zero{};
    (void)memory_.write_bytes(p, zero.data(), zero.size());
    (void)memory_.write_u32(p + 0, 80);
    (void)memory_.write_u64(p + 24, text);
    (void)memory_.write_u64(p + 32, text);
    (void)memory_.write_u32(p + 40, 0x4);  // MB_YESNO
    const auto indirect = ext("MessageBoxIndirectW");
    EXPECT_EQ(call(indirect, p), 6u);
    (void)memory_.write_u32(p + 0, 0);
    EXPECT_EQ(call(indirect, p), 6u);
    (void)memory_.write_u64(p + 8, kBogusHwnd);
    seed_err();
    EXPECT_EQ(call(indirect, p), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    ext_->set_resource_manager(nullptr);
}

TEST_F(InputContractTest, OpenInputDesktopSucceedsWithoutTouchingTheLastError) {
    // s2msg_inputdesktop_probe open_switch / open_bad_flag / open_no_access:
    // a handle, the last error untouched, whatever dwFlags and access say.
    const auto open = ext("OpenInputDesktop");
    for (const auto& [flags, access] :
         {std::pair<u64, u64>{0, 0x100}, {0x2, 0x1}, {0, 0}}) {
        seed_err();
        const u64 desk = call(open, flags, 0, access);
        EXPECT_NE(desk, 0u);
        EXPECT_EQ(err(), 0xDEADu);
        EXPECT_EQ(call(ext("CloseDesktop"), desk), 1u);
    }
}

// -----------------------------------------------------------------------------
// IMM32
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, ImmDefaultContextStateAndPropertiesMatchWindows) {
    Imm32Manager imm;
    const DllExports exports = create_imm32_thunks(imm);
    const auto imm_call = [&](const char* name, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0,
                              u64 a3 = 0) { return call(find(exports, name), a0, a1, a2, a3); };
    const HWND w = make_window(kSelf);
    const u64 ctx = imm_call("ImmGetContext", w);
    ASSERT_NE(ctx, 0u);
    // immgetconversionstatus 1 / err 0, conv=1 sent=8
    seed_err();
    EXPECT_EQ(imm_call("ImmGetConversionStatus", ctx, BUF, BUF + 4), 1u);
    EXPECT_EQ(err(), 0u);
    EXPECT_EQ(memory_.read_u32(BUF).value(), 1u);
    EXPECT_EQ(memory_.read_u32(BUF + 4).value(), 8u);
    // immgetopenstatus 0 / err 0; immsetopenstatus then _after 1
    seed_err();
    EXPECT_EQ(imm_call("ImmGetOpenStatus", ctx), 0u);
    EXPECT_EQ(err(), 0u);
    EXPECT_EQ(imm_call("ImmSetOpenStatus", ctx, 1), 1u);
    EXPECT_EQ(imm_call("ImmGetOpenStatus", ctx), 1u);
    // immgetcompositionstringw_bad_index fffffffe (IMM_ERROR_GENERAL)
    EXPECT_EQ(static_cast<u32>(imm_call("ImmGetCompositionStringW", ctx, 0x12345, 0, 0)),
              0xFFFFFFFEu);
    // immgetcandidatelistcountw 0x90, lists=0
    (void)memory_.write_u32(BUF, 0xFFFF);
    EXPECT_EQ(imm_call("ImmGetCandidateListCountW", ctx, BUF), 0x90u);
    EXPECT_EQ(memory_.read_u32(BUF).value(), 0u);
    // immgetproperty_property 00090000, _getimeversion 00040000, every other
    // index 0.
    const u64 hkl = 0x04090409;
    EXPECT_EQ(imm_call("ImmGetProperty", hkl, 0x4), 0x00090000u);
    EXPECT_EQ(imm_call("ImmGetProperty", hkl, 0xFFFFFFFCull), 0x00040000u);
    for (u64 index : {0x7777ull, 0x8ull, 0xCull, 0x10ull, 0x14ull, 0x18ull, 0ull}) {
        EXPECT_EQ(imm_call("ImmGetProperty", hkl, index), 0u) << index;
    }
    // immreleasecontext 1; immdestroycontext of a created context 1 / err 0,
    // again 0 / ERROR_INVALID_HANDLE.
    EXPECT_EQ(imm_call("ImmReleaseContext", w, ctx), 1u);
    const u64 mine = imm_call("ImmCreateContext");
    ASSERT_NE(mine, 0u);
    seed_err();
    EXPECT_EQ(imm_call("ImmDestroyContext", mine), 1u);
    EXPECT_EQ(err(), 0u);
    seed_err();
    EXPECT_EQ(imm_call("ImmDestroyContext", mine), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_HANDLE);
}

// -----------------------------------------------------------------------------
// IsChar* and the OEM translators (tools/oracle_probe/u32_char_dde_probe)
// -----------------------------------------------------------------------------

TEST_F(InputContractTest, IsCharClassesFollowTheCtype1Table) {
    ResourceManager resources(memory_);
    const DllExports res = create_resource_thunks(resources);
    // ischarw: one hex digit per code point, bit0 alpha, bit1 alphanumeric,
    // bit2 upper, bit3 lower.
    static constexpr u16 kPoints[] = {
        0x0041, 0x0061, 0x0030, 0x0039, 0x005F, 0x0020, 0x0000, 0x007F,
        0x00AA, 0x00B2, 0x00B5, 0x00BA, 0x00BD, 0x00C0, 0x00D7, 0x00DF,
        0x00E9, 0x00F7, 0x00FF, 0x0130, 0x0131, 0x01C5, 0x02B0, 0x0300,
        0x0391, 0x03B1, 0x0410, 0x0430, 0x0660, 0x0966, 0x2160, 0x2170,
        0x24B6, 0x24D0, 0x3007, 0x4E00, 0xFF10, 0xFF21, 0xFF41, 0xD800,
        0xFFFF, 0x2155, 0x00B9, 0x0345, 0x1D2C, 0x2071, 0x212B, 0x00AD,
    };
    static constexpr char kIsCharW[] = "7b220000b2bb070bb0b7bf307b7b2233003327b000203b70";
    const auto alpha_w = find(res, "IsCharAlphaW");
    const auto alnum_w = find(res, "IsCharAlphaNumericW");
    const auto upper_w = find(res, "IsCharUpperW");
    const auto lower_w = ext("IsCharLowerW");
    const auto digit = [](char c) { return static_cast<u32>(c <= '9' ? c - '0' : c - 'a' + 10); };
    // Known deviation (.docs/contracts/user32.md, SumatraPDF-install imports):
    // Windows' CT_CTYPE1 table predates Unicode 6.0 for U+2071 (then Ll, now
    // Lm) -- not derivable from current Unicode data.
    for (size_t i = 0; i < std::size(kPoints); ++i) {
        if (kPoints[i] == 0x2071) continue;
        const u32 want = digit(kIsCharW[i]);
        const u32 got = call(alpha_w, kPoints[i]) | (call(alnum_w, kPoints[i]) << 1) |
                        (call(upper_w, kPoints[i]) << 2) | (call(lower_w, kPoints[i]) << 3);
        EXPECT_EQ(got, want) << std::hex << kPoints[i];
    }
    // ischara0..3: every byte through CP_ACP (1252).
    static constexpr const char* kIsCharA[] = {
        "0000000000000000000000000000000000000000000000002222222222000000",
        "077777777777777777777777777000000bbbbbbbbbbbbbbbbbbbbbbbbbb00000",
        "000b0000007070700000000000b0b0b70000000000b0000000220b0002b00000",
        "7777777777777777777777707777777bbbbbbbbbbbbbbbbbbbbbbbb0bbbbbbbb",
    };
    const auto alpha_a = find(res, "IsCharAlphaA");
    const auto alnum_a = find(res, "IsCharAlphaNumericA");
    const auto upper_a = find(res, "IsCharUpperA");
    const auto lower_a = find(res, "IsCharLowerA");
    // Known deviation: byte 88 (U+02C6, Lm) is not alpha on Windows.
    for (u32 b = 0; b < 256; ++b) {
        if (b == 0x88) continue;
        const u32 want = digit(kIsCharA[b / 64][b % 64]);
        const u32 got = call(alpha_a, b) | (call(alnum_a, b) << 1) |
                        (call(upper_a, b) << 2) | (call(lower_a, b) << 3);
        EXPECT_EQ(got, want) << std::hex << b;
    }
    // ischaralphaw_hiword / ischaralphaa_hibyte: only the low unit counts;
    // TRUE is 1 and the last error is untouched.
    seed_err();
    EXPECT_EQ(call(alpha_w, 0x10041), 1u);
    EXPECT_EQ(call(alpha_a, 0x141), 1u);
    EXPECT_EQ(err(), 0xDEADu);
}

TEST_F(InputContractTest, OemTranslatorsMatchWindowsIncludingEdgeCases) {
    const GuestAddr src = BUF + 0x100;
    const GuestAddr dst = BUF + 0x300;
    const auto fill = [&](GuestAddr at, u32 n) {
        std::vector<u8> f(n, 0x55);
        (void)memory_.write_bytes(at, f.data(), n);
    };
    const auto put8 = [&](GuestAddr at, std::vector<u8> v) {
        (void)memory_.write_bytes(at, v.data(), v.size());
    };
    const auto put16 = [&](GuestAddr at, std::vector<u16> v) {
        for (size_t i = 0; i < v.size(); ++i) (void)memory_.write_u16(at + 2 * i, v[i]);
    };
    const auto bytes = [&](GuestAddr at, u32 n) {
        std::vector<u8> v(n);
        for (u32 i = 0; i < n; ++i) v[i] = memory_.read_u8(at + i).value();
        return v;
    };
    const auto units = [&](GuestAddr at, u32 n) {
        std::vector<u16> v(n);
        for (u32 i = 0; i < n; ++i) v[i] = memory_.read_u16(at + 2 * i).value();
        return v;
    };
    using B = std::vector<u8>;
    using W = std::vector<u16>;
    const B ansi{0xe9, 'A', 0xa6, 0x80, 0xff, 0};

    // chartooema 1: oem=82 41 dd 5f 98 00; chartooema_same identical.
    fill(dst, 8);
    put8(src, ansi);
    seed_err();
    EXPECT_EQ(call(ext("CharToOemA"), src, dst), 1u);
    EXPECT_EQ(bytes(dst, 8), (B{0x82, 0x41, 0xdd, 0x5f, 0x98, 0, 0x55, 0x55}));
    EXPECT_EQ(call(ext("CharToOemA"), src, src), 1u);
    EXPECT_EQ(bytes(src, 6), (B{0x82, 0x41, 0xdd, 0x5f, 0x98, 0}));
    EXPECT_EQ(err(), 0xDEADu);
    // *_null_src / *_null_dst: 0, last error untouched, for all eight.
    for (const char* name : {"CharToOemA", "CharToOemW", "OemToCharA", "OemToCharW",
                             "CharToOemBuffA", "CharToOemBuffW", "OemToCharBuffA",
                             "OemToCharBuffW"}) {
        seed_err();
        EXPECT_EQ(call(ext(name), 0, dst, 2), 0u) << name;
        EXPECT_EQ(call(ext(name), src, 0, 2), 0u) << name;
        EXPECT_EQ(err(), 0xDEADu) << name;
    }
    // chartooembuffa: 82 41 dd then untouched; _zero 1, nothing written.
    put8(src, ansi);
    fill(dst, 8);
    EXPECT_EQ(call(ext("CharToOemBuffA"), src, dst, 3), 1u);
    EXPECT_EQ(bytes(dst, 5), (B{0x82, 0x41, 0xdd, 0x55, 0x55}));
    fill(dst, 8);
    seed_err();
    EXPECT_EQ(call(ext("CharToOemBuffA"), src, dst, 0), 1u);
    EXPECT_EQ(err(), 0xDEADu);
    EXPECT_EQ(bytes(dst, 2), (B{0x55, 0x55}));
    // chartooembuffa_same: 82 41 dd 00.
    EXPECT_EQ(call(ext("CharToOemBuffA"), src, src, 3), 1u);
    EXPECT_EQ(bytes(src, 3), (B{0x82, 0x41, 0xdd}));

    // chartooemw: e9 41 20ac 2591 4e00 -> 82 41 5f b0 5f 00.
    put16(src, {0xe9, 'A', 0x20ac, 0x2591, 0x4e00, 0});
    fill(dst, 8);
    EXPECT_EQ(call(ext("CharToOemW"), src, dst), 1u);
    EXPECT_EQ(bytes(dst, 8), (B{0x82, 0x41, 0x5f, 0xb0, 0x5f, 0, 0x55, 0x55}));
    // chartooemw_same / chartooembuffw_same: 0, buffer untouched.
    put16(src, {0xe9, 'A', 0x2591, 0});
    seed_err();
    EXPECT_EQ(call(ext("CharToOemW"), src, src), 0u);
    EXPECT_EQ(call(ext("CharToOemBuffW"), src, src, 3), 0u);
    EXPECT_EQ(err(), 0xDEADu);
    EXPECT_EQ(units(src, 4), (W{0xe9, 'A', 0x2591, 0}));
    // chartooembuffw_zero: 1 with ERROR_INVALID_PARAMETER, nothing written.
    fill(dst, 4);
    seed_err();
    EXPECT_EQ(call(ext("CharToOemBuffW"), src, dst, 0), 1u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(bytes(dst, 4), (B{0x55, 0x55, 0x55, 0x55}));

    // oemtocharw: 82 41 b0 9c ff -> e9 41 2591 a3 a0 0; oemtocharbuffw 3.
    put8(src, {0x82, 'A', 0xb0, 0x9c, 0xff, 0});
    fill(dst, 16);
    EXPECT_EQ(call(ext("OemToCharW"), src, dst), 1u);
    EXPECT_EQ(units(dst, 8), (W{0xe9, 0x41, 0x2591, 0xa3, 0xa0, 0, 0x5555, 0x5555}));
    fill(dst, 16);
    EXPECT_EQ(call(ext("OemToCharBuffW"), src, dst, 3), 1u);
    EXPECT_EQ(units(dst, 4), (W{0xe9, 0x41, 0x2591, 0x5555}));
    // oemtocharbuffw_zero: 0 / ERROR_INVALID_PARAMETER; oemtocharbuffa_zero 1.
    seed_err();
    EXPECT_EQ(call(ext("OemToCharBuffW"), src, dst, 0), 0u);
    EXPECT_EQ(err(), error_codes::ERROR_INVALID_PARAMETER);
    fill(dst, 4);
    seed_err();
    EXPECT_EQ(call(ext("OemToCharBuffA"), src, dst, 0), 1u);
    EXPECT_EQ(err(), 0xDEADu);
    EXPECT_EQ(bytes(dst, 4), (B{0x55, 0x55, 0x55, 0x55}));
    // oemtocharw_same / oemtocharbuffw_same: 0, untouched;
    // oemtocharbuffa_same translates in place (e9 41 a6).
    put8(src, {0x82, 'A', 0xb0, 0});
    EXPECT_EQ(call(ext("OemToCharW"), src, src), 0u);
    EXPECT_EQ(call(ext("OemToCharBuffW"), src, src, 3), 0u);
    EXPECT_EQ(bytes(src, 4), (B{0x82, 0x41, 0xb0, 0}));
    EXPECT_EQ(call(ext("OemToCharBuffA"), src, src, 3), 1u);
    EXPECT_EQ(bytes(src, 3), (B{0xe9, 0x41, 0xa6}));

    // Whole-page tables: ANSI 01..ff to OEM, OEM 01..ff to Unicode.
    B all;
    for (u32 i = 1; i < 256; ++i) all.push_back(static_cast<u8>(i));
    all.push_back(0);
    put8(src, all);
    EXPECT_EQ(call(ext("CharToOemBuffA"), src, dst, 255), 1u);
    EXPECT_EQ(bytes(dst, 31), (B{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f}));   // ansi01_oem
    EXPECT_EQ(bytes(dst + 127, 64), (B{0x5f, 0x5f, 0x2c, 0x9f, 0x2c, 0x2e, 0x2b, 0xd8, 0x5e, 0x25, 0x53, 0x3c, 0x4f, 0x5f, 0x5a, 0x5f, 0x5f, 0x60, 0x27, 0x22, 0x22, 0x07, 0x2d, 0x2d, 0x7e, 0x54, 0x73, 0x3e, 0x6f, 0x5f, 0x7a, 0x59, 0xff, 0xad, 0x9b, 0x9c, 0x0f, 0x9d, 0xdd, 0x15, 0x22, 0x63, 0xa6, 0xae, 0xaa, 0x2d, 0x72, 0x5f, 0xf8, 0xf1, 0xfd, 0x33, 0x27, 0xe6, 0x14, 0xfa, 0x2c, 0x31, 0xa7, 0xaf, 0xac, 0xab, 0x5f, 0xa8}));   // ansi80_oem
    EXPECT_EQ(bytes(dst + 191, 64), (B{0x41, 0x41, 0x41, 0x41, 0x8e, 0x8f, 0x92, 0x80, 0x45, 0x90, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49, 0x44, 0xa5, 0x4f, 0x4f, 0x4f, 0x4f, 0x99, 0x78, 0x4f, 0x55, 0x55, 0x55, 0x9a, 0x59, 0x5f, 0xe1, 0x85, 0xa0, 0x83, 0x61, 0x84, 0x86, 0x91, 0x87, 0x8a, 0x82, 0x88, 0x89, 0x8d, 0xa1, 0x8c, 0x8b, 0x64, 0xa4, 0x95, 0xa2, 0x93, 0x6f, 0x94, 0xf6, 0x6f, 0x97, 0xa3, 0x96, 0x81, 0x79, 0x5f, 0x98}));   // ansiC0_oem
    const GuestAddr wide = BUF + 0x600;
    EXPECT_EQ(call(ext("OemToCharBuffW"), src, wide, 255), 1u);
    EXPECT_EQ(units(wide, 31), (W{0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c, 0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc}));   // oem01_w
    EXPECT_EQ(units(wide + 2 * 126, 1), (W{0x2302}));                       // oem7f_w
    EXPECT_EQ(units(wide + 2 * 127, 32), (W{0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192}));   // oem80_w
    EXPECT_EQ(units(wide + 2 * 159, 32), (W{0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510}));   // oemA0_w
    EXPECT_EQ(units(wide + 2 * 191, 32), (W{0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580}));   // oemC0_w
    EXPECT_EQ(units(wide + 2 * 223, 32), (W{0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0}));   // oemE0_w
    EXPECT_EQ(call(ext("OemToCharBuffA"), src, dst, 31), 1u);
    EXPECT_EQ(bytes(dst, 31), (B{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0xa4, 0x10, 0x11, 0x12, 0x13, 0xb6, 0xa7, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f}));   // oem01_a
    // glyphs_oem: 263a 2302 25d8 -> 01 7f 08 (best fit back to the controls).
    put16(src, {0x263a, 0x2302, 0x25d8});
    EXPECT_EQ(call(ext("CharToOemBuffW"), src, dst, 3), 1u);
    EXPECT_EQ(bytes(dst, 3), (B{0x01, 0x7f, 0x08}));
}

}  // namespace
