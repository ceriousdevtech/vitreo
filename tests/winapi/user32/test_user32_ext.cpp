/// @file test_user32_ext.cpp
/// @brief Keyboard and text-input contracts: ToUnicode/CharNext/CharPrev, MapVirtualKey,
///        GetKeyNameText, GetLastInputInfo, accelerators, raw keyboard input.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, CharNextExAStepsOneByteOnAcp) {
    auto entry = find_export("CharNextExA");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    EXPECT_EQ(entry.stdcall_bytes, 12u);

    ASSERT_TRUE(memory_.write_u8(BUF_ADDR, 'A').is_ok());
    ASSERT_TRUE(memory_.write_u8(BUF_ADDR + 1, 'B').is_ok());
    ASSERT_TRUE(memory_.write_u8(BUF_ADDR + 2, 0).is_ok());

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0; // CP_ACP
    cpu_.gpr.rdx = BUF_ADDR;
    cpu_.gpr.r8 = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, BUF_ADDR + 1);

    cpu_.gpr.rdx = BUF_ADDR + 2;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, BUF_ADDR + 2);
}

TEST_F(User32ExtThunkTest, ToUnicodeStdcallPopsSixArgs) {
    // Microsoft Learn: ToUnicode(wVirtKey, wScanCode, lpKeyState, pwszBuff,
    // cchBuff, wFlags) is WINAPI. Six 32-bit parameters → 24-byte stdcall
    // cleanup. ToUnicodeEx adds HKL (7 × 4 = 28), already registered correctly.
    auto to_unicode = find_export("ToUnicode");
    ASSERT_TRUE(static_cast<bool>(to_unicode.handler));
    EXPECT_EQ(to_unicode.stdcall_bytes, 24u);

    auto to_unicode_ex = find_export("ToUnicodeEx");
    ASSERT_TRUE(static_cast<bool>(to_unicode_ex.handler));
    EXPECT_EQ(to_unicode_ex.stdcall_bytes, 28u);
}

TEST_F(User32ExtThunkTest, ToUnicodeExMapsUsLayoutLettersAndDigits) {
    // Chromium UI text input calls ToUnicodeEx; a permanent 0 return makes
    // omnibox/page typing appear to lose focus with no characters inserted.
    auto entry = find_export("ToUnicodeEx");
    ASSERT_TRUE(static_cast<bool>(entry.handler));

    constexpr GuestAddr key_state = BUF_ADDR;
    constexpr GuestAddr out_chars = BUF_ADDR + 0x200;
    std::array<u8, 256> state{};
    ASSERT_TRUE(memory_.write_bytes(key_state, state.data(), state.size()).is_ok());
    ASSERT_TRUE(memory_.write_u16(out_chars, 0).is_ok());

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    // ToUnicodeEx(VK_A, scan, keyState, buff, cch, flags, hkl)
    cpu_.gpr.rcx = 0x41;
    cpu_.gpr.rdx = 0x1E;
    cpu_.gpr.r8 = key_state;
    cpu_.gpr.r9 = out_chars;
    set_fastcall_arg4_6(2, 0, 0);
    entry.handler(m);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1);
    EXPECT_EQ(memory_.read_u16(out_chars).value_or(0), u'a');

    state[0x10] = 0x80; // VK_SHIFT down
    ASSERT_TRUE(memory_.write_bytes(key_state, state.data(), state.size()).is_ok());
    cpu_.gpr.rcx = 0x31; // VK_1
    entry.handler(m);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1);
    EXPECT_EQ(memory_.read_u16(out_chars).value_or(0), u'!');
}

TEST_F(User32ExtThunkTest, GetLastInputInfoReturnsSessionInputTick) {
    constexpr GuestAddr info = BUF_ADDR;
    ASSERT_TRUE(memory_.write_u32(info, 8).is_ok());
    ASSERT_TRUE(memory_.write_u32(info + 4, 0xDEADBEEFu).is_ok());

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = info;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    find_export("GetLastInputInfo").handler(marshaler);

    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_NE(memory_.read_u32(info + 4).value_or(0), 0xDEADBEEFu);
}

TEST_F(User32ExtThunkTest, GetLastInputInfoRejectsNullAndWrongSize) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto get_last_input = find_export("GetLastInputInfo");

    cpu_.gpr.rcx = 0;
    get_last_input.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    constexpr GuestAddr info = BUF_ADDR;
    ASSERT_TRUE(memory_.write_u32(info, 4).is_ok());
    ASSERT_TRUE(memory_.write_u32(info + 4, 0xA5A5A5A5u).is_ok());
    cpu_.gpr.rcx = info;
    get_last_input.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u32(info + 4).value_or(0), 0xA5A5A5A5u);
}

// Accelerator tables are implemented (LoadAccelerators parses RT_ACCELERATOR and
// TranslateAccelerator matches against it). With no such resource present these
// must still fail *honestly* — reporting the resource error Windows reports,
// not ERROR_CALL_NOT_IMPLEMENTED — and clean up their stdcall arguments.
TEST_F(User32ExtThunkTest, AcceleratorApisReportMissingResourceWithCorrectStdcallCleanup) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    for (const char* name : {"LoadAcceleratorsA", "LoadAcceleratorsW"}) {
        auto entry = find_export(name);
        EXPECT_EQ(entry.stdcall_bytes, 8u);
        cpu_.gpr.rcx = 0x400000;
        cpu_.gpr.rdx = 101;
        entry.handler(marshaler);
        EXPECT_EQ(cpu_.gpr.rax, 0u);
        // No RT_ACCELERATOR resource with this id exists in the test image.
        EXPECT_EQ(get_last_error(), error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
    }

    for (const char* name : {"CreateAcceleratorTableA", "CopyAcceleratorTableA"}) {
        auto entry = find_export(name);
        EXPECT_NE(entry.handler, nullptr);
    }

    for (const char* name : {"TranslateAcceleratorA", "TranslateAcceleratorW"}) {
        auto entry = find_export(name);
        EXPECT_EQ(entry.stdcall_bytes, 12u);
        cpu_.gpr.rcx = 0x10000;
        cpu_.gpr.rdx = 0;
        cpu_.gpr.r8 = BUF_ADDR;
        entry.handler(marshaler);
        EXPECT_EQ(cpu_.gpr.rax, 0u);
    }
}

TEST_F(User32ExtThunkTest, CreateAndCopyAcceleratorTableARoundTrip) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    // Two ACCEL rows: {FVIRTKEY|FCONTROL, '1', 101} and {FVIRTKEY, VK_F1, 102}.
    const u8 rows[] = {
        0x09, 0x00, 0x31, 0x00, 0x65, 0x00,
        0x01, 0x00, 0x70, 0x00, 0x66, 0x00,
    };
    ASSERT_TRUE(memory_.write_bytes(BUF_ADDR, rows, sizeof(rows)).is_ok());

    auto create = find_export("CreateAcceleratorTableA");
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 2;
    create.handler(marshaler);
    const u32 haccel = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(haccel, 0u);
    ASSERT_NE(haccel, 0xFFFFFFFFu);

    auto copy = find_export("CopyAcceleratorTableA");
    cpu_.gpr.rcx = haccel;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0;
    copy.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 2u);

    constexpr GuestAddr dest = BUF_ADDR + 0x40;
    cpu_.gpr.rcx = haccel;
    cpu_.gpr.rdx = dest;
    cpu_.gpr.r8 = 2;
    copy.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 2u);
    EXPECT_EQ(memory_.read_u8(dest).value(), 0x09u);
    EXPECT_EQ(memory_.read_u16(dest + 2).value(), 0x0031u);
    EXPECT_EQ(memory_.read_u16(dest + 4).value(), 0x0065u);
}

TEST_F(User32ExtThunkTest, AcceleratorUsesRetrievedControlStateAfterPhysicalRelease) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    const u8 row[] = {0x09, 0x00, 0x4E, 0x00, 0x65, 0x00};
    ASSERT_TRUE(memory_.write_bytes(BUF_ADDR, row, sizeof(row)).is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 1;
    find_export("CreateAcceleratorTableA").handler(m);
    const u32 table = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(table, 0u);
    MSG control{};
    control.message = WM::KEYDOWN;
    control.wParam = 0x11;
    mgr_->note_retrieved_keyboard_message(control);
    bridge_.note_native_key_transition(0x11, false);
    MSG key{};
    key.message = WM::KEYDOWN;
    key.wParam = 'N';
    EXPECT_TRUE(mgr_->translate_accelerator(0x1234, table, key));
    control.message = WM::KEYUP;
    mgr_->note_retrieved_keyboard_message(control);
    EXPECT_FALSE(mgr_->translate_accelerator(0x1234, table, key));
}

TEST_F(User32ExtThunkTest, RawKeyboardRegistrationDeliversWmInputPacket) {
    auto register_raw = find_export("RegisterRawInputDevices");
    auto get_raw = find_export("GetRawInputData");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    WindowData window{};
    const HWND hwnd = hwnd_mgr_.create(window);
    constexpr GuestAddr device = BUF_ADDR;
    ASSERT_TRUE(memory_.write_u16(device, 1u).is_ok());      // HID_USAGE_PAGE_GENERIC
    ASSERT_TRUE(memory_.write_u16(device + 2, 6u).is_ok()); // HID_USAGE_GENERIC_KEYBOARD
    ASSERT_TRUE(memory_.write_u32(device + 4, 0u).is_ok());
    ASSERT_TRUE(memory_.write_u64(device + 8, hwnd).is_ok());
    cpu_.gpr.rcx = device;
    cpu_.gpr.rdx = 1u;
    cpu_.gpr.r8 = 16u;
    register_raw.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);

    mgr_->note_raw_keyboard_transition('A', true);
    MSG message{};
    ASSERT_TRUE(queue_.get(message));
    ASSERT_EQ(message.hwnd, hwnd);
    ASSERT_EQ(message.message, WM::INPUT);
    ASSERT_EQ(message.wParam, 0u);

    constexpr GuestAddr packet = BUF_ADDR + 0x100;
    constexpr GuestAddr packet_size = BUF_ADDR + 0x200;
    ASSERT_TRUE(memory_.write_u32(packet_size, 40u).is_ok());
    cpu_.gpr.rcx = message.lParam;
    cpu_.gpr.rdx = 0x10000003u; // RID_INPUT
    cpu_.gpr.r8 = packet;
    cpu_.gpr.r9 = packet_size;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, 24u).is_ok());
    get_raw.handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 40u);
    EXPECT_EQ(memory_.read_u32(packet).value(), 1u);       // RIM_TYPEKEYBOARD
    EXPECT_EQ(memory_.read_u32(packet + 4).value(), 40u);  // RAWINPUT size
    EXPECT_EQ(memory_.read_u16(packet + 24).value(), 0x1Eu); // A make code
    EXPECT_EQ(memory_.read_u16(packet + 26).value(), 0u);    // key make
    EXPECT_EQ(memory_.read_u16(packet + 30).value(), 'A');
    EXPECT_EQ(memory_.read_u32(packet + 32).value(), WM::KEYDOWN);
}

// Windows rawinput_lifetime_probe.expected and .x86.expected: reads are
// repeatable until the next successful message removal on the owning thread.
TEST_F(User32ExtThunkTest, RawInputHandleSurvivesReadsUntilNextMessageRemoval) {
    const u32 original_tid = ThreadManager::get_current_thread_id_static();
    struct RestoreThread {
        u32 tid;
        ~RestoreThread() { ThreadManager::set_current_thread_id(tid); }
    } restore{original_tid};
    ThreadManager::set_current_thread_id(5);
    for (const bool wide : {false, true}) {
        cpu_.mode = wide ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        const auto args = [&](std::initializer_list<u64> values) {
            usize i = 0;
            for (const u64 value : values) {
                if (!wide) {
                    EXPECT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4,
                        static_cast<u32>(value)).is_ok());
                } else if (i == 0) cpu_.gpr.rcx = value;
                else if (i == 1) cpu_.gpr.rdx = value;
                else if (i == 2) cpu_.gpr.r8 = value;
                else if (i == 3) cpu_.gpr.r9 = value;
                else EXPECT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28 +
                    (i - 4) * 8, value).is_ok());
                ++i;
            }
        };
        WindowData window{};
        const HWND hwnd = hwnd_mgr_.create(window);
        ASSERT_TRUE(memory_.write_u16(BUF_ADDR, 1).is_ok());
        ASSERT_TRUE(memory_.write_u16(BUF_ADDR + 2, 2).is_ok());
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, 0).is_ok());
        ASSERT_TRUE(memory_.write_u64(BUF_ADDR + 8, hwnd).is_ok());
        args({BUF_ADDR, 1, wide ? 16u : 12u});
        find_export("RegisterRawInputDevices").handler(m);
        ASSERT_EQ(cpu_.gpr.rax, 1u);
        mgr_->note_raw_mouse_motion(9, -4, hwnd);
        MSG raw{};
        ASSERT_TRUE(queue_.get(raw));
        ASSERT_EQ(raw.message, WM::INPUT);
        mgr_->note_retrieved_queue_message(raw);
        constexpr GuestAddr output = BUF_ADDR + 0x100;
        constexpr GuestAddr size = BUF_ADDR + 0x200;
        const u32 header = wide ? 24u : 16u;
        const auto read = [&](bool valid) {
            ASSERT_TRUE(memory_.write_u32(size, 512).is_ok());
            args({raw.lParam, 0x10000003u, output, size, header});
            find_export("GetRawInputData").handler(m);
            EXPECT_EQ(cpu_.gpr.rax, valid ? header + 24u : 0xffffffffu);
            EXPECT_EQ(memory_.read_u32(size).value(),
                      valid || wide ? 512u : 520u);
            if (valid) {
                EXPECT_EQ(memory_.read_u32(output + header + 12).value(), 9u);
                EXPECT_EQ(static_cast<i32>(memory_.read_u32(output + header + 16).value()), -4);
            } else {
                EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
            }
        };
        read(true);
        read(true);
        ASSERT_TRUE(memory_.write_u32(size, 512).is_ok());
        args({raw.lParam, 0x10000005u, output, size, header});
        find_export("GetRawInputData").handler(m);
        EXPECT_EQ(cpu_.gpr.rax, header);
        EXPECT_EQ(memory_.read_u32(size).value(), wide ? 512u : 16u);
        ASSERT_TRUE(memory_.write_u32(size, 512).is_ok());
        args({output, size, header});
        find_export("GetRawInputBuffer").handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 0u) << "current packet is no longer buffered";
        read(true);
        mgr_->note_raw_mouse_motion(3, 4, hwnd);
        ASSERT_TRUE(memory_.write_u32(size, 512).is_ok());
        args({output, size, header});
        find_export("GetRawInputBuffer").handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 1u) << "only the additional queued packet";
        read(true);
        MSG other{};
        other.message = WM::PAINT;
        mgr_->note_retrieved_queue_message(other, false);
        read(true);
        ThreadManager::set_current_thread_id(7);
        mgr_->note_retrieved_queue_message(other);
        ThreadManager::set_current_thread_id(5);
        read(true);
        mgr_->note_retrieved_queue_message(other);
        read(false);
        mgr_->note_raw_mouse_motion(9, -4, hwnd);
        ASSERT_TRUE(queue_.get(raw));
        mgr_->note_retrieved_queue_message(raw);
        read(true);
        mgr_->discard_raw_input_for_thread(5);
        read(false);
    }
}

// winuser.h (SDK 10.0.26100): RI_MOUSE_WHEEL 0x0400, RI_MOUSE_HWHEEL 0x0800,
// "the wheel delta is stored in usButtonData" (signed SHORT, WHEEL_DELTA 120).
// RAWMOUSE: usFlags @0, usButtonFlags @4, usButtonData @6, lLastX @12,
// lLastY @16, after a 16-byte (x86) or 24-byte (x64) RAWINPUTHEADER.
TEST_F(User32ExtThunkTest, RawMouseWheelCarriesSignedDeltaInButtonData) {
    for (const bool wide : {false, true}) {
        cpu_.mode = wide ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        const auto args = [&](std::initializer_list<u64> values) {
            usize i = 0;
            for (const u64 value : values) {
                if (!wide) {
                    EXPECT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4,
                        static_cast<u32>(value)).is_ok());
                } else if (i == 0) cpu_.gpr.rcx = value;
                else if (i == 1) cpu_.gpr.rdx = value;
                else if (i == 2) cpu_.gpr.r8 = value;
                else if (i == 3) cpu_.gpr.r9 = value;
                else EXPECT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28 +
                    (i - 4) * 8, value).is_ok());
                ++i;
            }
        };
        WindowData window{};
        const HWND hwnd = hwnd_mgr_.create(window);
        MSG drained{};
        while (queue_.get(drained)) {}
        mgr_->note_raw_mouse_wheel(-120, false, hwnd);
        EXPECT_FALSE(queue_.get(drained)) << "no mouse registration, no WM_INPUT";

        ASSERT_TRUE(memory_.write_u16(BUF_ADDR, 1).is_ok());
        ASSERT_TRUE(memory_.write_u16(BUF_ADDR + 2, 2).is_ok());
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, 0).is_ok());
        ASSERT_TRUE(memory_.write_u64(BUF_ADDR + 8, hwnd).is_ok());
        args({BUF_ADDR, 1, wide ? 16u : 12u});
        find_export("RegisterRawInputDevices").handler(m);
        ASSERT_EQ(cpu_.gpr.rax, 1u);

        constexpr GuestAddr output = BUF_ADDR + 0x100;
        constexpr GuestAddr size = BUF_ADDR + 0x200;
        const u32 header = wide ? 24u : 16u;
        const auto expect_packet = [&](u16 flags, i16 data) {
            MSG raw{};
            ASSERT_TRUE(queue_.get(raw));
            ASSERT_EQ(raw.message, WM::INPUT);
            EXPECT_EQ(raw.hwnd, hwnd);
            ASSERT_TRUE(memory_.write_u32(size, 512).is_ok());
            args({raw.lParam, 0x10000003u, output, size, header});
            find_export("GetRawInputData").handler(m);
            ASSERT_EQ(cpu_.gpr.rax, header + 24u);
            EXPECT_EQ(memory_.read_u32(output).value(), 0u) << "RIM_TYPEMOUSE";
            EXPECT_EQ(memory_.read_u16(output + header + 0).value(), 0u);
            EXPECT_EQ(memory_.read_u16(output + header + 4).value(), flags);
            EXPECT_EQ(static_cast<i16>(memory_.read_u16(output + header + 6).value()),
                      data);
            EXPECT_EQ(memory_.read_u32(output + header + 12).value(), 0u);
            EXPECT_EQ(memory_.read_u32(output + header + 16).value(), 0u);
            mgr_->note_retrieved_queue_message(raw);
        };
        mgr_->note_raw_mouse_wheel(-120, false, hwnd);
        expect_packet(0x0400u, -120);
        mgr_->note_raw_mouse_wheel(240, true, hwnd);
        expect_packet(0x0800u, 240);
        mgr_->note_raw_mouse_wheel(30, false, hwnd);
        expect_packet(0x0400u, 30);
        // A delta beyond SHORT splits into several packets, like WM_MOUSEWHEEL.
        mgr_->note_raw_mouse_wheel(70000, false, hwnd);
        expect_packet(0x0400u, 32760);
        expect_packet(0x0400u, 32760);
        expect_packet(0x0400u, 4480);
        mgr_->note_raw_mouse_wheel(0, false, hwnd);
        EXPECT_FALSE(queue_.get(drained));

        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, 0x00000001u).is_ok());  // RIDEV_REMOVE
        ASSERT_TRUE(memory_.write_u64(BUF_ADDR + 8, 0).is_ok());
        args({BUF_ADDR, 1, wide ? 16u : 12u});
        find_export("RegisterRawInputDevices").handler(m);
        ASSERT_EQ(cpu_.gpr.rax, 1u);
    }
}

TEST_F(User32ExtThunkTest, RawKeyboardBufferConsumesPacketAndWmInput) {
    auto register_raw = find_export("RegisterRawInputDevices");
    auto get_buffer = find_export("GetRawInputBuffer");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    WindowData window{};
    const HWND hwnd = hwnd_mgr_.create(window);
    constexpr GuestAddr device = BUF_ADDR;
    ASSERT_TRUE(memory_.write_u16(device, 1u).is_ok());
    ASSERT_TRUE(memory_.write_u16(device + 2, 6u).is_ok());
    ASSERT_TRUE(memory_.write_u32(device + 4, 0u).is_ok());
    ASSERT_TRUE(memory_.write_u64(device + 8, hwnd).is_ok());
    cpu_.gpr.rcx = device;
    cpu_.gpr.rdx = 1u;
    cpu_.gpr.r8 = 16u;
    register_raw.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);

    mgr_->note_raw_keyboard_transition('D', true);
    constexpr GuestAddr packet = BUF_ADDR + 0x100;
    constexpr GuestAddr size = BUF_ADDR + 0x200;
    ASSERT_TRUE(memory_.write_u32(size, 40u).is_ok());
    cpu_.gpr.rcx = packet;
    cpu_.gpr.rdx = size;
    cpu_.gpr.r8 = 24u;
    get_buffer.handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(packet).value(), 1u);
    EXPECT_EQ(memory_.read_u16(packet + 24).value(), 0x20u);
    EXPECT_EQ(memory_.read_u16(packet + 30).value(), 'D');
    EXPECT_TRUE(queue_.empty());
}

// =============================================================================
// MapVirtualKeyA Tests
// =============================================================================

TEST_F(User32ExtThunkTest, MapVirtualKeyA_VkToScanCode) {
    auto entry = find_export("MapVirtualKeyA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // VK_RETURN (0x0D) → scan code 0x1C
    cpu_.gpr.rcx = 0x0D;  // uCode = VK_RETURN
    cpu_.gpr.rdx = 0;     // uMapType = MAPVK_VK_TO_VSC

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x1Cu);
}

TEST_F(User32ExtThunkTest, MapVirtualKeyA_ScanCodeToVk) {
    auto entry = find_export("MapVirtualKeyA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Scan code 0x1E → VK_A (0x41)
    cpu_.gpr.rcx = 0x1E;  // scan code for 'A'
    cpu_.gpr.rdx = 1;     // uMapType = MAPVK_VSC_TO_VK

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x41u);
}

TEST_F(User32ExtThunkTest, VkKeyScanExAMapsAsciiLetters) {
    auto entry = find_export("VkKeyScanExA");
    ASSERT_EQ(entry.stdcall_bytes, 8u);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = static_cast<u64>('A');
    cpu_.gpr.rdx = 0;
    entry.handler(m);
    EXPECT_EQ(static_cast<u16>(cpu_.gpr.rax), static_cast<u16>(0x0141));

    cpu_.gpr.rcx = static_cast<u64>('a');
    entry.handler(m);
    EXPECT_EQ(static_cast<u16>(cpu_.gpr.rax), static_cast<u16>(0x0041));
}


TEST_F(User32ExtThunkTest, MapVirtualKeyA_VkToChar) {
    auto entry = find_export("MapVirtualKeyA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // VK_A (0x41) → 'A' (0x41)
    cpu_.gpr.rcx = 0x41;
    cpu_.gpr.rdx = 2;  // MAPVK_VK_TO_CHAR

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x41u); // 'A'
}

TEST_F(User32ExtThunkTest, MapVirtualKeyA_UnknownReturnsZero) {
    auto entry = find_export("MapVirtualKeyA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0xFF;  // unknown VK
    cpu_.gpr.rdx = 0;     // VK_TO_VSC

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

// =============================================================================
// GetKeyNameTextA Tests
// =============================================================================

TEST_F(User32ExtThunkTest, GetKeyNameTextA_EscapeKey) {
    auto entry = find_export("GetKeyNameTextA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // lParam: scan code 0x01 in bits 16-23
    cpu_.gpr.rcx = (0x01u << 16);  // lParam with scan code 0x01 (Escape)
    cpu_.gpr.rdx = BUF_ADDR;      // lpString
    cpu_.gpr.r8  = 64;            // cchSize

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 3u); // "Esc" = 3 chars

    auto e = memory_.read_u8(BUF_ADDR);
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.value(), 'E');
}

TEST_F(User32ExtThunkTest, GetKeyNameTextA_F1Key) {
    auto entry = find_export("GetKeyNameTextA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = (0x3Bu << 16);  // scan code for F1
    cpu_.gpr.rdx = BUF_ADDR;
    cpu_.gpr.r8  = 64;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 2u); // "F1"
}

TEST_F(User32ExtThunkTest, CharPrevAStepsBackOneByte) {
    // InstallShield redistributable wrappers walk paths with CharPrevA.
    const char* path = "C:\\Temp\\a.exe";
    for (usize i = 0; path[i] != '\0'; ++i) {
        ASSERT_TRUE(memory_.write_u8(BUF_ADDR + i, static_cast<u8>(path[i])).is_ok());
    }
    ASSERT_TRUE(memory_.write_u8(BUF_ADDR + std::strlen(path), 0).is_ok());

    auto entry = find_export("CharPrevA");
    EXPECT_EQ(entry.stdcall_bytes, 8u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    const GuestAddr start = BUF_ADDR;
    const GuestAddr after_slash = BUF_ADDR + 8; // points at 'a' in "a.exe"
    cpu_.gpr.rcx = start;
    cpu_.gpr.rdx = after_slash;
    entry.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, after_slash - 1u);

    cpu_.gpr.rcx = start;
    cpu_.gpr.rdx = start;
    entry.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, start);
}
