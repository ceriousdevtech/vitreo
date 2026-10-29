/// @file test_user32_ext_desktop_station.cpp
/// @brief Window stations, desktops and user-object information: startup desktop,
///        GetUserObjectInformation, station/desktop lifecycle and process/thread association.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, StartupDesktopAttachesProcessToNamedStationAndDesktop) {
    // CreateProcess STARTUPINFO.lpDesktop ("winsta\\desk") must attach the
    // new process — Chromium sandbox launches children on alternate desktops.
    mgr_->apply_startup_desktop("VitreoSandboxSta\\sbox_alternate_desktop_test");

    auto get_station = find_export("GetProcessWindowStation");
    auto get_desktop = find_export("GetThreadDesktop");
    auto info_entry = find_export("GetUserObjectInformationW");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    get_station.handler(m);
    const u64 station = cpu_.gpr.rax;
    ASSERT_NE(station, 0u);
    get_desktop.handler(m);
    const u64 desktop = cpu_.gpr.rax;
    ASSERT_NE(desktop, 0u);
    EXPECT_NE(station, desktop);

    constexpr GuestAddr name_out = BUF_ADDR;
    cpu_.gpr.rcx = station;
    cpu_.gpr.rdx = 2; // UOI_NAME
    cpu_.gpr.r8 = name_out;
    cpu_.gpr.r9 = 128;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, 0).is_ok());
    info_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u16(name_out).value(), static_cast<u16>('V'));

    cpu_.gpr.rcx = desktop;
    cpu_.gpr.rdx = 2;
    cpu_.gpr.r8 = name_out;
    cpu_.gpr.r9 = 128;
    info_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u16(name_out).value(), static_cast<u16>('s'));
}

// Desktops and window stations are kernel objects: CreateDesktop /
// OpenDesktop / OpenInputDesktop insert into m_desktops on one thread while
// GetUserObjectInformation / CloseDesktop on another look it up (every
// Chromium process probes its desktop from several threads at startup).
// 8 threads x 2000 rounds through the export table, a quarter of the
// desktops kept open so the table keeps rehashing under the lookups.
TEST_F(User32ExtThunkTest, DesktopTablesSurviveConcurrentCreateOpenQueryClose) {
    auto create_entry = find_export("CreateDesktopW");
    auto open_entry = find_export("OpenDesktopW");
    auto input_entry = find_export("OpenInputDesktop");
    auto info_entry = find_export("GetUserObjectInformationW");
    auto close_entry = find_export("CloseDesktop");
    constexpr int kThreads = 8;
    constexpr int kRounds = 2000;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            cpu::CpuState tcpu;
            tcpu.mode = cpu::CpuMode::Long;
            tcpu.gpr.rsp = STACK_BASE + 0x800 + static_cast<u64>(t) * 0x1000;
            Marshaler tm(memory_);
            tm.attach_cpu(tcpu);
            const GuestAddr name = MEM_BASE + 0x3000 + static_cast<GuestAddr>(t) * 0x400;
            const GuestAddr flags_out = name + 0x100;
            const GuestAddr needed_out = name + 0x140;
            if (tm.write_wide_string(name, "RaceDesk" + std::to_string(t)).is_err()) {
                ++failures;
                return;
            }
            auto stack_args = [&](u64 arg4, u64 arg5) {
                (void)memory_.write_u64(tcpu.gpr.rsp + 0x28, arg4);
                (void)memory_.write_u64(tcpu.gpr.rsp + 0x30, arg5);
            };
            auto close = [&](u64 handle) {
                tcpu.gpr.rcx = handle;
                close_entry.handler(tm);
                if (tcpu.gpr.rax != 1u) ++failures;
            };
            std::vector<u64> kept;
            for (int i = 0; i < kRounds; ++i) {
                // CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL)
                tcpu.gpr.rcx = name;
                tcpu.gpr.rdx = 0;
                tcpu.gpr.r8 = 0;
                tcpu.gpr.r9 = 0;
                stack_args(0x10000000, 0);
                create_entry.handler(tm);
                const u64 created = tcpu.gpr.rax;
                if (created == 0) { ++failures; continue; }
                // OpenDesktopW(name, 0, FALSE, GENERIC_ALL): a new handle to
                // the same desktop.
                tcpu.gpr.rcx = name;
                tcpu.gpr.rdx = 0;
                tcpu.gpr.r8 = 0;
                tcpu.gpr.r9 = 0x10000000;
                open_entry.handler(tm);
                const u64 opened = tcpu.gpr.rax;
                if (opened == 0) ++failures;
                // GetUserObjectInformationW(created, UOI_FLAGS, out, 12, needed)
                tcpu.gpr.rcx = created;
                tcpu.gpr.rdx = 1;
                tcpu.gpr.r8 = flags_out;
                tcpu.gpr.r9 = 12;
                stack_args(needed_out, 0);
                info_entry.handler(tm);
                if (tcpu.gpr.rax != 1u) ++failures;
                // OpenInputDesktop(0, FALSE, GENERIC_ALL) + CloseDesktop
                tcpu.gpr.rcx = 0;
                tcpu.gpr.rdx = 0;
                tcpu.gpr.r8 = 0x10000000;
                input_entry.handler(tm);
                if (tcpu.gpr.rax == 0) ++failures; else close(tcpu.gpr.rax);
                if (opened != 0) close(opened);
                if (i % 4 == 0) kept.push_back(created); else close(created);
            }
            for (u64 handle : kept) close(handle);
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST_F(User32ExtThunkTest, GetUserObjectInformationReportsVisibleWinSta0Flags) {
    // MS Learn: UOI_FLAGS returns USEROBJECTFLAGS; interactive WinSta0 has
    // WSF_VISIBLE; CreateWindowStation stations do not.
    auto get_entry = find_export("GetProcessWindowStation");
    auto create_entry = find_export("CreateWindowStationW");
    auto close_entry = find_export("CloseWindowStation");
    auto info_entry = find_export("GetUserObjectInformationW");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    get_entry.handler(m);
    const u64 winsta0 = cpu_.gpr.rax;
    ASSERT_NE(winsta0, 0u);

    constexpr GuestAddr flags_out = BUF_ADDR;
    constexpr GuestAddr needed_out = BUF_ADDR + 0x40;
    cpu_.gpr.rcx = winsta0;
    cpu_.gpr.rdx = 1; // UOI_FLAGS
    cpu_.gpr.r8 = flags_out;
    cpu_.gpr.r9 = 12;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(needed_out).value(), 12u);
    EXPECT_EQ(memory_.read_u32(flags_out + 0).value(), 0u); // fInherit
    EXPECT_EQ(memory_.read_u32(flags_out + 4).value(), 0u); // fReserved
    EXPECT_EQ(memory_.read_u32(flags_out + 8).value(), 0x1u); // WSF_VISIBLE

    ASSERT_TRUE(m.write_wide_string(BUF_ADDR + 0x100, "AltSta").is_ok());
    cpu_.gpr.rcx = BUF_ADDR + 0x100;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0xF037F;
    cpu_.gpr.r9 = 0;
    create_entry.handler(m);
    const u64 created = cpu_.gpr.rax;
    ASSERT_NE(created, 0u);

    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = flags_out;
    cpu_.gpr.r9 = 12;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(flags_out + 8).value(), 0u);

    cpu_.gpr.rcx = created;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

TEST_F(User32ExtThunkTest, GetUserObjectInformationAReturnsAnsiNameAndType) {
    auto get_entry = find_export("GetProcessWindowStation");
    auto info_a = find_export("GetUserObjectInformationA");
    ASSERT_EQ(info_a.name, "GetUserObjectInformationA");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    get_entry.handler(m);
    const u64 winsta0 = cpu_.gpr.rax;
    ASSERT_NE(winsta0, 0u);

    constexpr GuestAddr buf = BUF_ADDR;
    constexpr GuestAddr needed_out = BUF_ADDR + 0x80;
    cpu_.gpr.rcx = winsta0;
    cpu_.gpr.rdx = 2; // UOI_NAME
    cpu_.gpr.r8 = buf;
    cpu_.gpr.r9 = 64;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_a.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(needed_out).value(), 8u); // "WinSta0\0"
    EXPECT_EQ(memory_.read_u8(buf + 0).value(), static_cast<u8>('W'));
    EXPECT_EQ(memory_.read_u8(buf + 6).value(), static_cast<u8>('0'));
    EXPECT_EQ(memory_.read_u8(buf + 7).value(), 0u);

    cpu_.gpr.rcx = winsta0;
    cpu_.gpr.rdx = 3; // UOI_TYPE
    cpu_.gpr.r8 = buf;
    cpu_.gpr.r9 = 64;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_a.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u8(buf + 0).value(), static_cast<u8>('W')); // WindowStation
}

TEST_F(User32ExtThunkTest, WindowStationLifecycleTracksProcessAssociation) {
    auto get_entry = find_export("GetProcessWindowStation");
    auto create_entry = find_export("CreateWindowStationW");
    auto open_entry = find_export("OpenWindowStationW");
    auto set_entry = find_export("SetProcessWindowStation");
    auto close_entry = find_export("CloseWindowStation");
    auto info_entry = find_export("GetUserObjectInformationW");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    get_entry.handler(m);
    const u64 original = cpu_.gpr.rax;
    ASSERT_NE(original, 0u);

    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "VitreoSandbox").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0xF037F;
    cpu_.gpr.r9 = 0;
    create_entry.handler(m);
    const u64 created = cpu_.gpr.rax;
    ASSERT_NE(created, 0u);
    EXPECT_NE(created, original);

    // OpenWindowStationW reopens an existing station by name (case-insensitive).
    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "vitreosandbox").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0xF037F;
    open_entry.handler(m);
    const u64 opened_station = cpu_.gpr.rax;
    ASSERT_NE(opened_station, 0u);
    EXPECT_NE(opened_station, created);
    cpu_.gpr.rcx = opened_station;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    constexpr GuestAddr needed_out = BUF_ADDR + 0x100;
    constexpr GuestAddr name_out = BUF_ADDR + 0x200;
    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = 2; // UOI_NAME
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    const u32 needed = memory_.read_u32(needed_out).value();
    ASSERT_GT(needed, 2u);

    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = 2;
    cpu_.gpr.r8 = name_out;
    cpu_.gpr.r9 = needed;
    info_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u16(name_out).value(), static_cast<u16>('V'));

    cpu_.gpr.rcx = created;
    set_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    get_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, created);

    cpu_.gpr.rcx = created;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    cpu_.gpr.rcx = original;
    ASSERT_NO_FATAL_FAILURE(set_entry.handler(m));
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = created;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

// Desktop / window-station handles carry the inheritance they were opened
// with (s4_desktop_handle_probe, Windows 11 x64): OpenDesktopW and
// OpenWindowStationW fInherit=TRUE -> GetHandleInformation flags 1, FALSE -> 0;
// GetThreadDesktop -> 0; SetHandleInformation changes them; after
// CloseDesktop GetHandleInformation fails. GetHandleInformation consults the
// registry read here for handles HandleTable does not own. OpenInputDesktop's
// fInherit follows Learn (the probe cannot open the input desktop over SSH).
TEST_F(User32ExtThunkTest, DesktopHandlesReportTheirInheritFlag) {
    namespace sh = synthetic_handles;
    auto open_w = find_export("OpenDesktopW");
    auto open_input = find_export("OpenInputDesktop");
    auto open_winsta = find_export("OpenWindowStationW");
    auto get_thread = find_export("GetThreadDesktop");
    auto create_w = find_export("CreateDesktopW");
    auto close_desk = find_export("CloseDesktop");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    u32 flags = 0xFFu;

    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "Default").is_ok());
    // fInherit is a BOOL: only the low 32 bits of R8 count.
    for (const u64 inherit_reg : {u64{1}, u64{0xFFFFFFFF00000000ull}}) {
        cpu_.gpr.rcx = BUF_ADDR;
        cpu_.gpr.rdx = 0;
        cpu_.gpr.r8 = inherit_reg;
        cpu_.gpr.r9 = 0x0001; // DESKTOP_READOBJECTS
        open_w.handler(m);
        const Handle h = static_cast<Handle>(cpu_.gpr.rax);
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(sh::get_flags(h, flags));
        EXPECT_EQ(flags, inherit_reg == 1 ? HandleFlags::Inherit : 0u);
        ASSERT_TRUE(sh::set_flags(h, HandleFlags::Inherit | HandleFlags::ProtectFromClose,
                                  HandleFlags::ProtectFromClose));
        ASSERT_TRUE(sh::get_flags(h, flags));
        EXPECT_EQ(flags, HandleFlags::ProtectFromClose);
        cpu_.gpr.rcx = h;
        close_desk.handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 1u);
        EXPECT_FALSE(sh::get_flags(h, flags));
    }

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 1; // fInherit
    cpu_.gpr.r8 = 0x0001;
    open_input.handler(m);
    const Handle input = static_cast<Handle>(cpu_.gpr.rax);
    ASSERT_NE(input, 0u);
    ASSERT_TRUE(sh::get_flags(input, flags));
    EXPECT_EQ(flags, HandleFlags::Inherit);
    cpu_.gpr.rcx = input;
    close_desk.handler(m);

    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "WinSta0").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = 0x0002; // WINSTA_READATTRIBUTES
    open_winsta.handler(m);
    const Handle winsta = static_cast<Handle>(cpu_.gpr.rax);
    ASSERT_NE(winsta, 0u);
    ASSERT_TRUE(sh::get_flags(winsta, flags));
    EXPECT_EQ(flags, HandleFlags::Inherit);

    cpu_.gpr.rcx = 1;
    get_thread.handler(m);
    const Handle thread_desk = static_cast<Handle>(cpu_.gpr.rax);
    ASSERT_TRUE(sh::get_flags(thread_desk, flags));
    EXPECT_EQ(flags, 0u);

    // CreateDesktopW(..., lpsa): x64 SECURITY_ATTRIBUTES.bInheritHandle @16.
    constexpr GuestAddr sa = BUF_ADDR + 0x200;
    ASSERT_TRUE(memory_.write_u32(sa, 24).is_ok());
    ASSERT_TRUE(memory_.write_u64(sa + 8, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(sa + 16, 1).is_ok());
    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "S4InheritDesk").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, 0x0001).is_ok());
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x30, sa).is_ok());
    create_w.handler(m);
    const Handle created = static_cast<Handle>(cpu_.gpr.rax);
    ASSERT_NE(created, 0u);
    ASSERT_TRUE(sh::get_flags(created, flags));
    EXPECT_EQ(flags, HandleFlags::Inherit);
    cpu_.gpr.rcx = created;
    close_desk.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

TEST_F(User32ExtThunkTest, DesktopLifecycleTracksThreadAssociation) {
    auto get_entry = find_export("GetThreadDesktop");
    auto create_entry = find_export("CreateDesktopW");
    auto set_entry = find_export("SetThreadDesktop");
    auto close_entry = find_export("CloseDesktop");
    auto info_entry = find_export("GetUserObjectInformationW");
    auto open_input_entry = find_export("OpenInputDesktop");
    auto open_w_entry = find_export("OpenDesktopW");
    auto switch_entry = find_export("SwitchDesktop");
    auto get_sec_entry = find_export("GetUserObjectSecurity");
    auto set_sec_entry = find_export("SetUserObjectSecurity");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 1;
    get_entry.handler(m);
    const u64 original = cpu_.gpr.rax;
    ASSERT_NE(original, 0u);

    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "VitreoDesktop").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    set_fastcall_arg4_6(0xF01FF, 0, 0);
    create_entry.handler(m);
    const u64 created = cpu_.gpr.rax;
    ASSERT_NE(created, 0u);
    EXPECT_NE(created, original);

    cpu_.gpr.rcx = created;
    switch_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = 0xDEADBEEFu;
    switch_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    // GetUserObjectSecurity reports needed size then returns a self-relative SD.
    constexpr GuestAddr sec_needed = BUF_ADDR + 0x400;
    constexpr GuestAddr sec_buf = BUF_ADDR + 0x480;
    constexpr GuestAddr sec_info = BUF_ADDR + 0x440;
    ASSERT_TRUE(memory_.write_u32(sec_info, 0x00000007u).is_ok()); // O+G+DACL
    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = sec_info;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, sec_needed).is_ok());
    get_sec_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    const u32 sec_size = memory_.read_u32(sec_needed).value();
    ASSERT_GE(sec_size, 20u);
    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = sec_info;
    cpu_.gpr.r8 = sec_buf;
    cpu_.gpr.r9 = sec_size;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, sec_needed).is_ok());
    get_sec_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = sec_info;
    cpu_.gpr.r8 = sec_buf;
    set_sec_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    constexpr GuestAddr needed_out = BUF_ADDR + 0x100;
    cpu_.gpr.rcx = created;
    cpu_.gpr.rdx = 2;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, needed_out).is_ok());
    info_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_GT(memory_.read_u32(needed_out).value(), 2u);

    cpu_.gpr.rcx = created;
    set_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = 1;
    get_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, created);

    // OpenInputDesktop follows SwitchDesktop's input desktop and returns a
    // distinct handle that CloseDesktop can release.
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0x10000000; // GENERIC_READ-shaped access mask
    open_input_entry.handler(m);
    const u64 input_desk = cpu_.gpr.rax;
    ASSERT_NE(input_desk, 0u);
    EXPECT_NE(input_desk, created);
    constexpr GuestAddr input_name_out = BUF_ADDR + 0x300;
    cpu_.gpr.rcx = input_desk;
    cpu_.gpr.rdx = 2; // UOI_NAME
    cpu_.gpr.r8 = input_name_out;
    cpu_.gpr.r9 = 64;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 0x28, 0).is_ok());
    info_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u16(input_name_out).value(), static_cast<u16>('V'));
    cpu_.gpr.rcx = input_desk;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    // OpenDesktopW opens an existing desktop by name (case-insensitive).
    ASSERT_TRUE(m.write_wide_string(BUF_ADDR, "vitreodesktop").is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0x10000000;
    open_w_entry.handler(m);
    const u64 opened = cpu_.gpr.rax;
    ASSERT_NE(opened, 0u);
    EXPECT_NE(opened, created);
    cpu_.gpr.rcx = opened;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = created;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    cpu_.gpr.rcx = original;
    set_entry.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = created;
    close_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}
