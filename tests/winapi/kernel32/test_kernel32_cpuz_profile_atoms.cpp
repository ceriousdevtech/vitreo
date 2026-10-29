/// @file test_kernel32_cpuz_profile_atoms.cpp
/// @brief Unit tests for KERNEL32 legacy profile, locale, global atom table and GetDateFormat contracts

#include "kernel32_cpuz_fixture.hpp"
#include <atomic>
#include <chrono>
#include <capsule/vfs/host_filesystem.hpp>
#include <winapi/kernel32/error.hpp>
#include <filesystem>
#include <fstream>
#include <thread>

// =============================================================================
// Profile / Locale
// =============================================================================

TEST_F(Kernel32CpuzTest, GetProfileIntA_ReturnsDefault) {
    constexpr GuestAddr app_name = DATA_BASE;
    constexpr GuestAddr key_name = DATA_BASE + 0x20;
    ASSERT_TRUE(marshaler.write_ansi_string(app_name, "Smacker").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(key_name, "UseMMX").is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), app_name).is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), key_name).is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 7).is_ok());
    mgr.impl_GetProfileIntA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 7u);
}

TEST_F(Kernel32CpuzTest, GetProfileIntW_ReturnsDefault) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);    // lpAppName (NULL)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);    // lpKeyName (NULL)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 42);  // nDefault
    mgr.impl_GetProfileIntW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 42u);
}

TEST_F(Kernel32CpuzTest, ConvertDefaultLocale_ReturnsInputUnchanged) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);  // en-US LCID
    mgr.impl_ConvertDefaultLocale(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x0409u);
}

// =============================================================================
// Atom Table
// =============================================================================

TEST_F(Kernel32CpuzTest, GlobalAddAtomW_ReturnsNonZero) {
    // Write a wide string "TestAtom" at DATA_BASE
    write_guest_wide_string(DATA_BASE, "TestAtom");
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), DATA_BASE);  // lpString
    mgr.impl_GlobalAddAtomW(marshaler);
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, GlobalGetAtomNameW_RetrievesPreviouslyAdded) {
    // Add an atom first
    write_guest_wide_string(DATA_BASE, "Hello");
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), DATA_BASE);
    mgr.impl_GlobalAddAtomW(marshaler);
    u16 atom = static_cast<u16>(cpu.gpr.rax);
    ASSERT_NE(atom, 0);

    // Now retrieve it into a buffer at DATA_BASE + 0x200
    constexpr GuestAddr buf = DATA_BASE + 0x200;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), atom);  // nAtom
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf);   // lpBuffer
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 128);  // nSize
    mgr.impl_GlobalGetAtomNameW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 5u);  // length of "Hello"

    // Verify first char 'H' = 0x48
    auto r = memory.read_u16(buf);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), u'H');
}

// The global atom table is one kernel object for every thread: GlobalAddAtom
// on one thread inserts (rehashing m_atoms) while GlobalGetAtomName on
// another walks it. 8 threads x 1500 rounds, half the atoms kept alive so the
// table keeps growing under the lookups. Rounds stay below the u16 atom
// range (0xC001..0xFFFF) so no two threads ever share an atom value.
TEST_F(Kernel32CpuzTest, AtomTableSurvivesConcurrentAddGetDelete) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 1500;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            cpu::CpuState tcpu;
            tcpu.init_protected_mode();
            tcpu.gpr.rsp = STACK_BASE + 0x100 + static_cast<u64>(t) * 0x100;
            Marshaler tm(memory);
            tm.attach_cpu(tcpu);
            const u32 rsp = static_cast<u32>(tcpu.gpr.rsp);
            const GuestAddr str = DATA_BASE + 0x1000 * static_cast<GuestAddr>(t);
            const GuestAddr buf = str + 0x200;
            const std::string text = "atom_" + std::to_string(t);
            write_guest_wide_string(str, text.c_str());
            std::vector<u16> kept;
            for (int i = 0; i < kRounds; ++i) {
                (void)memory.write_u32(rsp + 4, static_cast<u32>(str));
                mgr.impl_GlobalAddAtomW(tm);
                const u16 atom = static_cast<u16>(tcpu.gpr.rax);
                if (atom == 0) { ++failures; continue; }
                (void)memory.write_u32(rsp + 4, atom);
                (void)memory.write_u32(rsp + 8, static_cast<u32>(buf));
                (void)memory.write_u32(rsp + 12, 128);
                mgr.impl_GlobalGetAtomNameW(tm);
                if (tcpu.gpr.rax != text.size()) ++failures;
                if (i % 2 == 0) {
                    kept.push_back(atom);
                } else {
                    (void)memory.write_u32(rsp + 4, atom);
                    mgr.impl_GlobalDeleteAtom(tm);
                }
            }
            for (u16 atom : kept) {
                (void)memory.write_u32(rsp + 4, atom);
                mgr.impl_GlobalDeleteAtom(tm);
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST_F(Kernel32CpuzTest, GlobalDeleteAtom_ReturnsZero) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0xC001);  // nAtom
    mgr.impl_GlobalDeleteAtom(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, GlobalFindAtomW_ReturnsZeroForNotFound) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);  // lpString (NULL)
    mgr.impl_GlobalFindAtomW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// GetProfileStringA/W were MISSING (Miles mss32/mss64 import them). Learn
// GetProfileString: reads win.ini, i.e. GetPrivateProfileString on
// %windir%\win.ini. Windows 11's win.ini ends "[Mail]\r\nMAPI=1" (Windows).
TEST_F(Kernel32CpuzTest, GetProfileStringReadsWinIni) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_profile_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Windows"));
    {
        std::ofstream ini(root / "Windows" / "win.ini", std::ios::binary);
        ini << "; for 16-bit app support\r\n[fonts]\r\n[extensions]\r\n"
               "[mci extensions]\r\n[files]\r\n[Mail]\r\nMAPI=1\r\n";
    }
    ASSERT_TRUE(vfs.mount_drive(
                       'C', std::make_unique<capsule::HostFileSystem>(root.string()))
                    .is_ok());
    constexpr GuestAddr app = DATA_BASE + 0x400;
    constexpr GuestAddr key = DATA_BASE + 0x440;
    constexpr GuestAddr missing = DATA_BASE + 0x480;
    constexpr GuestAddr def = DATA_BASE + 0x4C0;
    constexpr GuestAddr out = DATA_BASE + 0x600;
    const GuestAddr stack = cpu.sp();
    const auto put_a = [&](GuestAddr at, const char* text) {
        ASSERT_TRUE(memory.write_bytes(at, reinterpret_cast<const u8*>(text),
                                       std::strlen(text) + 1).is_ok());
    };
    const auto call_a = [&](GuestAddr key_addr) {
        ASSERT_TRUE(memory.write_u32(stack + 4, app).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, key_addr).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 12, def).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 16, out).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 20, 16).is_ok());
        mgr.impl_GetProfileStringA(marshaler);
    };
    put_a(app, "Mail");
    put_a(key, "MAPI");
    put_a(missing, "NoSuchKey");
    put_a(def, "dflt");
    call_a(key);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(read_guest_ansi_string(out), "1");
    call_a(missing);
    EXPECT_EQ(cpu.gpr.rax, 4u);
    EXPECT_EQ(read_guest_ansi_string(out), "dflt");

    write_guest_wide_string(app, "Mail");
    write_guest_wide_string(key, "MAPI");
    write_guest_wide_string(def, "dflt");
    ASSERT_TRUE(memory.write_u32(stack + 4, app).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, key).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, def).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 16).is_ok());
    mgr.impl_GetProfileStringW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(read_guest_wide_string(out), "1");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

// =============================================================================
// GetDateFormatA / GetDateFormatW
// =============================================================================

namespace {

/// Fill a guest SYSTEMTIME. Only the date fields matter to GetDateFormat.
void write_systemtime(GuestMemory& memory, GuestAddr addr, u16 year, u16 month,
                      u16 day_of_week, u16 day) {
    (void)memory.write_u16(addr + 0, year);
    (void)memory.write_u16(addr + 2, month);
    (void)memory.write_u16(addr + 4, day_of_week);
    (void)memory.write_u16(addr + 6, day);
    for (u32 offset = 8; offset < 16; offset += 2) {
        (void)memory.write_u16(addr + offset, 0);
    }
}

} // namespace

TEST_F(Kernel32CpuzTest, GetDateFormatA_DefaultShortDateFormatsTheSuppliedDate) {
    constexpr GuestAddr date = DATA_BASE;
    constexpr GuestAddr out = DATA_BASE + 0x40;
    // Friday, 20 July 2035.
    write_systemtime(memory, date, 2035, 7, 5, 20);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409); // Locale
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);      // dwFlags
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);     // lpFormat
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), out);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 64);

    mgr.impl_GetDateFormatA(marshaler);

    // The date itself must be formatted, not the picture string echoed back.
    EXPECT_EQ(read_guest_ansi_string(out), "7/20/2035");
    EXPECT_EQ(cpu.gpr.rax, 10u); // includes the null terminator
}

TEST_F(Kernel32CpuzTest, GetDateFormatA_LongDateUsesDayAndMonthNames) {
    constexpr GuestAddr date = DATA_BASE;
    constexpr GuestAddr out = DATA_BASE + 0x40;
    write_systemtime(memory, date, 2035, 7, 5, 20);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0x2); // DATE_LONGDATE
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), out);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 64);

    mgr.impl_GetDateFormatA(marshaler);

    EXPECT_EQ(read_guest_ansi_string(out), "Friday, July 20, 2035");
}

TEST_F(Kernel32CpuzTest, GetDateFormatA_HonoursExplicitPictureAndQuotedLiterals) {
    constexpr GuestAddr date = DATA_BASE;
    constexpr GuestAddr fmt = DATA_BASE + 0x40;
    constexpr GuestAddr out = DATA_BASE + 0x80;
    write_systemtime(memory, date, 2035, 7, 5, 20);
    ASSERT_TRUE(marshaler.write_ansi_string(fmt, "dd'-'MMM'-'yy").is_ok());

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), fmt);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), out);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 64);

    mgr.impl_GetDateFormatA(marshaler);

    EXPECT_EQ(read_guest_ansi_string(out), "20-Jul-35");
}

TEST_F(Kernel32CpuzTest, GetDateFormatA_ZeroBufferSizeReturnsRequiredLength) {
    constexpr GuestAddr date = DATA_BASE;
    write_systemtime(memory, date, 2035, 7, 5, 20);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 0); // cchDate == 0

    mgr.impl_GetDateFormatA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 10u); // "7/20/2035" + null
}

TEST_F(Kernel32CpuzTest, GetDateFormatA_TooSmallBufferFailsWithInsufficientBuffer) {
    constexpr GuestAddr date = DATA_BASE;
    constexpr GuestAddr out = DATA_BASE + 0x40;
    write_systemtime(memory, date, 2035, 7, 5, 20);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), out);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 4);

    mgr.impl_GetDateFormatA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(Kernel32CpuzTest, GetDateFormatA_RejectsOutOfRangeMonth) {
    constexpr GuestAddr date = DATA_BASE;
    constexpr GuestAddr out = DATA_BASE + 0x40;
    write_systemtime(memory, date, 2035, 13, 5, 20); // month 13 is invalid

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x0409);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), date);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), out);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 64);

    mgr.impl_GetDateFormatA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}
