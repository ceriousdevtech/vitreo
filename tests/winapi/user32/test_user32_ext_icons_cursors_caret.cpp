/// @file test_user32_ext_icons_cursors_caret.cpp
/// @brief Cursor, icon and caret objects: caret blink time and visibility, GetCursorPos,
///        ShowCursor, SetCursor/GetCursor, CreateIconIndirect, GetIconInfo, DrawIconEx.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, CaretBlinkTimeIsProcessWideAndMutable) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    auto get_blink_time = find_export("GetCaretBlinkTime");
    auto set_blink_time = find_export("SetCaretBlinkTime");

    get_blink_time.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 530u);

    cpu_.gpr.rcx = 725;
    set_blink_time.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    get_blink_time.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 725u);

    cpu_.gpr.rcx = 0xFFFFFFFFu;
    set_blink_time.handler(marshaler);
    get_blink_time.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0xFFFFFFFFu);
}

TEST_F(User32ExtThunkTest, HideCaretAndShowCaretHaveStdcallOneArg) {
    // Learn: BOOL HideCaret/ShowCaret(HWND) — x86 stdcall pops 4 bytes.
    auto hide = find_export("HideCaret");
    auto show = find_export("ShowCaret");
    EXPECT_EQ(hide.stdcall_bytes, 4u);
    EXPECT_EQ(show.stdcall_bytes, 4u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    // No caret yet: FALSE / ERROR_ACCESS_DENIED
    // (tools/oracle_probe/s2msg_input_probe.expected hidecaret_no_caret).
    cpu_.gpr.rcx = 0;
    hide.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);

    WindowData data{};
    data.owner_thread_id = ThreadManager::get_current_thread_id_static();
    const HWND hwnd = hwnd_mgr_.create(data);
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 2;
    cpu_.gpr.r9 = 10;
    find_export("CreateCaret").handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = 0;
    show.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = 0;
    hide.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    find_export("DestroyCaret").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

// =============================================================================
// GetCursorPos Tests
// =============================================================================

TEST_F(User32ExtThunkTest, GetCursorPosReturnsNativeBridgePosition) {
    auto entry = find_export("GetCursorPos");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = BUF_ADDR;  // lpPoint

    i32 expected_x = 0;
    i32 expected_y = 0;
    bridge_.get_native_cursor_pos(expected_x, expected_y);
    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE

    auto x = memory_.read_u32(BUF_ADDR);
    auto y = memory_.read_u32(BUF_ADDR + 4);
    ASSERT_TRUE(x.is_ok());
    ASSERT_TRUE(y.is_ok());
    EXPECT_EQ(static_cast<i32>(x.value()), expected_x);
    EXPECT_EQ(static_cast<i32>(y.value()), expected_y);
}

// No process is DPI-scaled under Vitreo, so GetPhysicalCursorPos reports the
// same point as GetCursorPos (both stdcall one argument).
TEST_F(User32ExtThunkTest, GetPhysicalCursorPosIsTheCursorPosition) {
    auto physical = find_export("GetPhysicalCursorPos");
    EXPECT_EQ(physical.stdcall_bytes, 4u);
    EXPECT_EQ(find_export("SetPhysicalCursorPos").stdcall_bytes, 8u);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = BUF_ADDR;
    i32 expected_x = 0;
    i32 expected_y = 0;
    bridge_.get_native_cursor_pos(expected_x, expected_y);
    physical.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR).value()), expected_x);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 4).value()), expected_y);
}

// physical_cursor_probe (x86 = x64): a NULL or unknown window is FALSE /
// ERROR_INVALID_WINDOW_HANDLE with the point untouched; the desktop window
// succeeds, the point unchanged (no scaling) and the last error untouched.
TEST_F(User32ExtThunkTest, PointConversionsCheckTheWindowLikeWindows) {
    for (const char* name : {"LogicalToPhysicalPoint", "PhysicalToLogicalPoint"}) {
        SCOPED_TRACE(name);
        auto entry = find_export(name);
        EXPECT_EQ(entry.stdcall_bytes, 8u);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR, 100).is_ok());
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, 200).is_ok());
        for (const u64 bad : {u64{0}, u64{0x12345678}}) {
            cpu_.gpr.rcx = bad;
            cpu_.gpr.rdx = BUF_ADDR;
            set_last_error(0xDEAD);
            entry.handler(m);
            EXPECT_EQ(cpu_.gpr.rax, 0u);
            EXPECT_EQ(get_last_error(), 1400u);  // ERROR_INVALID_WINDOW_HANDLE
        }
        cpu_.gpr.rcx = windowing::HWND_DESKTOP;
        cpu_.gpr.rdx = BUF_ADDR;
        set_last_error(0xDEAD);
        entry.handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 1u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 100u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR + 4).value(), 200u);
    }
}

// =============================================================================
// ShowCursor Tests
// =============================================================================

TEST_F(User32ExtThunkTest, CreateIconIndirectBuildsCursorFromColorDib) {
    HandleTable handles;
    GdiManager gdi(handles);
    ResourceManager resources(memory_);
    ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
                                   GdiManager::GDI_DIB_SCRATCH_SIZE,
                                   MemoryProtection::ReadWrite).is_ok());
    gdi.set_guest_memory(&memory_);
    mgr_->set_gdi_manager(&gdi);
    mgr_->set_resource_manager(&resources);

    constexpr GuestAddr bmi = BUF_ADDR + 0x100;
    constexpr GuestAddr ppv = BUF_ADDR + 0x180;
    constexpr GuestAddr info = BUF_ADDR + 0x200;
    (void)memory_.write_u32(bmi + 0, 40);
    (void)memory_.write_u32(bmi + 4, 8);
    (void)memory_.write_u32(bmi + 8, 8);
    (void)memory_.write_u16(bmi + 12, 1);
    (void)memory_.write_u16(bmi + 14, 32);
    (void)memory_.write_u32(bmi + 16, 0);
    const Handle color = gdi.create_dib_section(0, bmi, 0, ppv, false);
    ASSERT_NE(color, Handle{0});
    const u32 bits = memory_.read_u32(ppv).value();
    const std::array<u8, 4> bgra{0x10, 0x20, 0x30, 0xFF};
    ASSERT_TRUE(memory_.write_bytes(bits + 7 * 32, bgra.data(), bgra.size()).is_ok());

    const Handle mask = gdi.create_bitmap(8, 8, 1, 1, 0);
    ASSERT_NE(mask, Handle{0});

    (void)memory_.write_u32(info + 0, 0);   // fIcon = cursor
    (void)memory_.write_u32(info + 4, 2);   // xHotspot
    (void)memory_.write_u32(info + 8, 3);   // yHotspot
    (void)memory_.write_u64(info + 16, mask);
    (void)memory_.write_u64(info + 24, color);

    auto entry = find_export("CreateIconIndirect");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = info;
    entry.handler(m);
    const u32 handle = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(handle, 0u);
    EXPECT_NE(handle, 0xCAF1C003u);

    auto image = resources.cursor_image(handle);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 8u);
    EXPECT_EQ(image->height, 8u);
    EXPECT_EQ(image->hotspot_x, 2u);
    EXPECT_EQ(image->hotspot_y, 3u);
    ASSERT_GE(image->rgba.size(), 4u);
    EXPECT_EQ(image->rgba[0], 0x30);
    EXPECT_EQ(image->rgba[1], 0x20);
    EXPECT_EQ(image->rgba[2], 0x10);
    EXPECT_EQ(image->rgba[3], 0xFF);
}

TEST_F(User32ExtThunkTest, IconObjectsMatchTheMeasuredGetIconInfoAndDrawIconEx) {
    // tools/oracle_probe/s3gdi_icon_probe: a 4x4 32bpp colour plane whose
    // alpha runs 00/40/80/ff by column, with an opaque mask.
    HandleTable handles;
    GdiManager gdi(handles);
    ResourceManager resources(memory_);
    ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                   MemoryProtection::ReadWrite).is_ok());
    gdi.set_guest_memory(&memory_);
    mgr_->set_gdi_manager(&gdi);
    mgr_->set_resource_manager(&resources);
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    auto make_dib = [&](i32 w, i32 h, GuestAddr bmi, GuestAddr ppv) {
        (void)memory_.write_u32(bmi + 0, 40);
        (void)memory_.write_u32(bmi + 4, static_cast<u32>(w));
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(-h));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        (void)memory_.write_u32(bmi + 16, 0);
        const Handle dib = gdi.create_dib_section(0, bmi, 0, ppv, true);
        return std::pair{dib, static_cast<GuestAddr>(memory_.read_u64(ppv).value_or(0))};
    };
    auto [color, cbits] = make_dib(4, 4, BUF_ADDR + 0x100, BUF_ADDR + 0x180);
    const u8 alphas[4] = {0x00, 0x40, 0x80, 0xFF};
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x)
            (void)memory_.write_u32(cbits + (y * 4 + x) * 4,
                                    (u32{alphas[x]} << 24) | ((0x20 + x * 0x30) << 16) |
                                        ((0x10 + y * 0x30) << 8) | 0x44);
    const Handle mask = gdi.create_bitmap(4, 4, 1, 1, 0);
    constexpr GuestAddr info = BUF_ADDR + 0x200;
    (void)memory_.write_u32(info + 0, 1);
    (void)memory_.write_u32(info + 4, 9);
    (void)memory_.write_u32(info + 8, 9);
    (void)memory_.write_u64(info + 16, mask);
    (void)memory_.write_u64(info + 24, color);
    cpu_.gpr.rcx = info;
    find_export("CreateIconIndirect").handler(m);
    const u64 icon = cpu_.gpr.rax;
    ASSERT_NE(icon, 0u);

    // GetIconInfo: centre hotspot, new DDBs, the colour keeps its alpha.
    constexpr GuestAddr ii = BUF_ADDR + 0x300;
    cpu_.gpr.rcx = icon;
    cpu_.gpr.rdx = ii;
    find_export("GetIconInfo").handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(ii + 0).value(), 1u);
    EXPECT_EQ(memory_.read_u32(ii + 4).value(), 2u);
    EXPECT_EQ(memory_.read_u32(ii + 8).value(), 2u);
    const Handle got_mask = static_cast<Handle>(memory_.read_u64(ii + 16).value());
    const Handle got_color = static_cast<Handle>(memory_.read_u64(ii + 24).value());
    EXPECT_NE(got_mask, mask);
    EXPECT_NE(got_color, color);
    ASSERT_EQ(gdi.get_object(got_mask, 32, BUF_ADDR + 0x400, true), 32);
    EXPECT_EQ(memory_.read_u16(BUF_ADDR + 0x400 + 18).value(), 1u);
    ASSERT_EQ(gdi.get_object(got_color, 32, BUF_ADDR + 0x400, true), 32);
    EXPECT_EQ(memory_.read_u16(BUF_ADDR + 0x400 + 18).value(), 32u);
    EXPECT_EQ(memory_.read_u64(BUF_ADDR + 0x400 + 24).value(), 0u);  // a DDB, not a DIB
    GdiManager::BitmapRgbaCopy copy;
    ASSERT_TRUE(gdi.copy_bitmap_rgba(got_color, copy));
    EXPECT_EQ(copy.rgba[1 * 4 + 3], 0x40u);

    // DrawIconEx DI_NORMAL into a preset 8x8 DIB ("alpha DrawIconEx NORMAL").
    auto [dst, dbits] = make_dib(8, 8, BUF_ADDR + 0x500, BUF_ADDR + 0x580);
    const Handle dc = gdi.create_compatible_dc(0);
    (void)gdi.select_object(dc, dst);
    for (u32 i = 0; i < 64; ++i) (void)memory_.write_u32(dbits + i * 4, 0x80AABBCC);
    cpu_.gpr.rcx = dc;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = icon;
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, 0);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, 0);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x38, 0);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x40, 0);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x48, 3);  // DI_NORMAL
    find_export("DrawIconEx").handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    const u32 row1[4] = {0x80aabbcc, 0xa09390aa, 0xc0956588, 0xffb01044};
    for (u32 x = 0; x < 4; ++x) EXPECT_EQ(memory_.read_u32(dbits + (8 + 1 + x) * 4).value(), row1[x]) << x;

    // CopyIcon is a new handle; DestroyIcon twice: TRUE then FALSE/1402.
    cpu_.gpr.rcx = icon;
    find_export("CopyIcon").handler(m);
    const u64 copy_icon = cpu_.gpr.rax;
    EXPECT_NE(copy_icon, 0u);
    EXPECT_NE(copy_icon, icon);
    cpu_.gpr.rcx = copy_icon;
    find_export("DestroyIcon").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    set_last_error(57005);
    find_export("DestroyIcon").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1402u);
    // A mask/colour size mismatch is ERROR_INVALID_PARAMETER.
    (void)memory_.write_u64(info + 16, gdi.create_bitmap(8, 8, 1, 1, 0));
    cpu_.gpr.rcx = info;
    set_last_error(57005);
    find_export("CreateIconIndirect").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 87u);
}

TEST_F(User32ExtThunkTest, CreateIconFromResourceDecodesCursorDib) {
    ResourceManager resources(memory_);
    mgr_->set_resource_manager(&resources);

    constexpr GuestAddr blob = BUF_ADDR + 0x400;
    constexpr u32 width = 8;
    constexpr u32 height = 8;
    constexpr u32 xor_stride = 4;
    constexpr u32 and_stride = 4;
    constexpr u32 size = 4 + 40 + 8 + xor_stride * height + and_stride * height;
    std::vector<u8> cursor(size, 0);
    cursor[0] = 1;
    cursor[2] = 2;
    cursor[4] = 40;
    cursor[8] = static_cast<u8>(width);
    cursor[12] = static_cast<u8>(height * 2);
    cursor[16] = 1;
    cursor[18] = 1;
    cursor[4 + 40 + 4] = 255;
    cursor[4 + 40 + 5] = 255;
    cursor[4 + 40 + 6] = 255;
    cursor[4 + 40 + 8 + 7 * xor_stride] = 0x80;
    ASSERT_TRUE(memory_.write_bytes(blob, cursor.data(), cursor.size()).is_ok());

    auto entry = find_export("CreateIconFromResource");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = blob;
    cpu_.gpr.rdx = size;
    cpu_.gpr.r8 = 0; // fIcon = cursor
    cpu_.gpr.r9 = 0x00030000;
    entry.handler(m);
    const u32 handle = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(handle, 0u);
    EXPECT_NE(handle, 0xCAF1C001u);
    // CreateIconFromResource is CreateIconFromResourceEx(LR_DEFAULTSIZE):
    // the 8x8 cursor comes back 32x32 with its hotspot scaled (Windows:
    // "CreateIconFromResource cursor 8x8 fIcon=0 hot=4,8 mask 32x64").
    auto image = resources.cursor_image(handle);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 32u);
    EXPECT_EQ(image->hotspot_x, 4u);
    EXPECT_EQ(image->hotspot_y, 8u);
    ASSERT_GE(image->rgba.size(), 4u);
    EXPECT_EQ(image->rgba[3], 255);
}

TEST_F(User32ExtThunkTest, ShowCursor_IncrementAndDecrement) {
    auto entry = find_export("ShowCursor");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Show (increment) - starts at 0, becomes 1
    cpu_.gpr.rcx = 1; // TRUE
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    // Show again - becomes 2
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 1;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 2u);

    // Hide (decrement) - becomes 1
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0; // FALSE
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

TEST_F(User32ExtThunkTest, GetCursorReturnsCurrentSetCursorState) {
    auto entry = find_export("GetCursor");
    ASSERT_TRUE(static_cast<bool>(entry.handler));

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // User32ExtManager starts with its stock-arrow sentinel installed.
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    // GetCursor returns the actual SetCursor state, including an arbitrary
    // valid HCURSOR and NULL when the cursor has been removed.
    mgr_->m_current_cursor = 0x27F00u;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x27F00u);

    mgr_->m_current_cursor = 0u;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}
