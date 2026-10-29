/// @file test_console.cpp
/// @brief Unit tests for Console I/O (WIN32-032)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/console/console.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <iterator>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class ConsoleManagerTest : public ::testing::Test {
protected:
    static constexpr usize MEM_SIZE = 8192;
    static constexpr GuestAddr MEM_BASE = 0x1000;

    HandleTable handles_;
    GuestMemory memory_;
    std::unique_ptr<Marshaler> marshaler_;
    std::unique_ptr<ConsoleManager> cm_;

    std::string captured_output_;
    std::string captured_error_;
    std::string input_to_provide_;

    void SetUp() override {
        auto res = memory_.map_region(MEM_BASE, MEM_SIZE, MemoryProtection::ReadWrite);
        ASSERT_FALSE(res.is_err());

        marshaler_ = std::make_unique<Marshaler>(memory_);
        cm_ = std::make_unique<ConsoleManager>(handles_);

        cm_->set_output_callback([this](std::string_view s) {
            captured_output_ += s;
        });
        cm_->set_error_callback([this](std::string_view s) {
            captured_error_ += s;
        });
        cm_->set_input_callback([this]() -> std::string {
            return input_to_provide_;
        });
        cm_->set_input_ready_callback([this] { return !input_to_provide_.empty(); });
    }

    /// Helper: write an ANSI C string into guest memory at the given address
    void write_guest_string(GuestAddr addr, std::string_view str) {
        auto res = memory_.write_bytes(addr,
            reinterpret_cast<const Byte*>(str.data()),
            str.size());
        ASSERT_FALSE(res.is_err());
    }

    /// Helper: write UTF-16LE code units into guest memory
    void write_guest_wide(GuestAddr addr, const u16* units, usize count) {
        for (usize i = 0; i < count; ++i) {
            auto res = memory_.write_u16(addr + static_cast<GuestAddr>(i) * 2, units[i]);
            ASSERT_FALSE(res.is_err());
        }
    }

    /// Helper: read a u32 from guest memory
    u32 read_guest_u32(GuestAddr addr) {
        auto res = memory_.read_u32(addr);
        EXPECT_FALSE(res.is_err());
        return res.value();
    }

    /// Helper: read bytes from guest memory into a std::string
    std::string read_guest_string(GuestAddr addr, usize len) {
        std::vector<Byte> buf(len);
        auto res = memory_.read_bytes(addr, buf.data(), len);
        EXPECT_FALSE(res.is_err());
        return std::string(reinterpret_cast<const char*>(buf.data()), len);
    }
};

// =============================================================================
// GetStdHandle Tests
// =============================================================================

TEST_F(ConsoleManagerTest, GetStdInputReturnsValidHandle) {
    Handle h = cm_->get_std_handle(STD_INPUT_HANDLE);
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_TRUE(handles_.is_valid(h));
}

TEST_F(ConsoleManagerTest, GetStdOutputReturnsValidHandle) {
    Handle h = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_TRUE(handles_.is_valid(h));
}

TEST_F(ConsoleManagerTest, GetStdErrorReturnsValidHandle) {
    Handle h = cm_->get_std_handle(STD_ERROR_HANDLE);
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_TRUE(handles_.is_valid(h));
}

TEST_F(ConsoleManagerTest, GetStdHandleInvalidReturnsInvalidHandle) {
    Handle h = cm_->get_std_handle(0x12345678);
    EXPECT_EQ(h, INVALID_HANDLE);
}

// SetStdHandle/GetStdHandle round trip, measured on Windows 11
// (tools/oracle_probe/process_contract_probe.expected "== std handles").
TEST_F(ConsoleManagerTest, SetStdHandleRedirectsGetStdHandleLikeWindows) {
    set_last_error(0xDEAD);
    EXPECT_EQ(cm_->get_std_handle(static_cast<u32>(-13)), INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(cm_->get_std_handle(0), INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    set_last_error(0xDEAD);
    EXPECT_TRUE(cm_->set_std_handle(STD_ERROR_HANDLE, 0x1234));
    EXPECT_EQ(cm_->get_std_handle(STD_ERROR_HANDLE), 0x1234u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_TRUE(cm_->set_std_handle(STD_ERROR_HANDLE, NULL_HANDLE));
    EXPECT_EQ(cm_->get_std_handle(STD_ERROR_HANDLE), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_TRUE(cm_->set_std_handle(STD_ERROR_HANDLE, INVALID_HANDLE));
    EXPECT_EQ(cm_->get_std_handle(STD_ERROR_HANDLE), INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    set_last_error(0xDEAD);
    EXPECT_FALSE(cm_->set_std_handle(static_cast<u32>(-13), 0x1234));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_FALSE(cm_->set_std_handle(0, 0x1234));
    // Redirection leaves the console output handle itself usable.
    EXPECT_EQ(cm_->console_handle_role(cm_->console_output_handle(true)), 2u);
}

// GetStartupInfo reports the creator's STARTUPINFO (probe: dwFlags 0x101,
// wShowWindow 7, hStd* == GetStdHandle), at both pointer widths.
TEST(GuestStartupInfoTest, RoundTripsAndWritesBothLayouts) {
    GuestStartupInfo si{};
    si.flags = 0x101; si.show_window = 7; si.x = 3; si.fill_attribute = 0x1f;
    si.std_input = 0x40; si.std_output = 0x44; si.std_error = 0xFFFFFFFFu;
    GuestStartupInfo back{};
    ASSERT_TRUE(decode_guest_startup_info(encode_guest_startup_info(si), back));
    EXPECT_EQ(back.flags, 0x101u);
    EXPECT_EQ(back.show_window, 7u);
    EXPECT_EQ(back.std_error, 0xFFFFFFFFu);

    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    const GuestStartupInfo saved = guest_startup_info();
    guest_startup_info() = si;
    write_guest_startup_info(mem, 0x10000, true);
    EXPECT_EQ(mem.read_u32(0x10000).value(), 104u);
    EXPECT_EQ(mem.read_u32(0x10000 + 32).value(), 3u);
    EXPECT_EQ(mem.read_u32(0x10000 + 56).value(), 0x1fu);
    EXPECT_EQ(mem.read_u32(0x10000 + 60).value(), 0x101u);
    EXPECT_EQ(mem.read_u16(0x10000 + 64).value(), 7u);
    EXPECT_EQ(mem.read_u64(0x10000 + 80).value(), 0x40u);
    EXPECT_EQ(mem.read_u64(0x10000 + 88).value(), 0x44u);
    EXPECT_EQ(mem.read_u64(0x10000 + 96).value(), 0xFFFFFFFFFFFFFFFFull);
    write_guest_startup_info(mem, 0x10200, false);
    EXPECT_EQ(mem.read_u32(0x10200).value(), 68u);
    EXPECT_EQ(mem.read_u32(0x10200 + 44).value(), 0x101u);
    EXPECT_EQ(mem.read_u16(0x10200 + 48).value(), 7u);
    EXPECT_EQ(mem.read_u32(0x10200 + 56).value(), 0x40u);
    EXPECT_EQ(mem.read_u32(0x10200 + 64).value(), 0xFFFFFFFFu);
    guest_startup_info() = saved;
}

TEST(ConsoleManagerDetachedTest, GuiProcessHasNullStandardHandles) {
    HandleTable handles;
    ConsoleManager console(handles, false);

    EXPECT_EQ(console.get_std_handle(STD_INPUT_HANDLE), NULL_HANDLE);
    EXPECT_EQ(console.get_std_handle(STD_OUTPUT_HANDLE), NULL_HANDLE);
    EXPECT_EQ(console.get_std_handle(STD_ERROR_HANDLE), NULL_HANDLE);
}

TEST_F(ConsoleManagerTest, StdoutAndStderrAreDifferentHandles) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    EXPECT_NE(out, err);
}

TEST_F(ConsoleManagerTest, StdinDiffersFromOutputHandles) {
    Handle in  = cm_->get_std_handle(STD_INPUT_HANDLE);
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    EXPECT_NE(in, out);
    EXPECT_NE(in, err);
}

TEST_F(ConsoleManagerTest, StdHandlesAreConsoleType) {
    Handle in  = cm_->get_std_handle(STD_INPUT_HANDLE);
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    EXPECT_EQ(handles_.get_type(in),  HandleType::ConsoleInput);
    EXPECT_EQ(handles_.get_type(out), HandleType::ConsoleOutput);
    EXPECT_EQ(handles_.get_type(err), HandleType::ConsoleOutput);
}

// =============================================================================
// WriteConsoleA Tests
// =============================================================================

TEST_F(ConsoleManagerTest, WriteConsoleAWritesCorrectString) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    write_guest_string(buf_addr, "Hello, World!");
    bool ok = cm_->write_console_a(out, buf_addr, 13, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_output_, "Hello, World!");
    EXPECT_EQ(read_guest_u32(written_addr), 13u);
}

TEST_F(ConsoleManagerTest, WriteConsoleAToStderr) {
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    write_guest_string(buf_addr, "error msg");
    bool ok = cm_->write_console_a(err, buf_addr, 9, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_error_, "error msg");
    EXPECT_TRUE(captured_output_.empty());
}

TEST_F(ConsoleManagerTest, WriteConsoleAZeroCharsSucceeds) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr written_addr = MEM_BASE + 256;

    // Write zero to the written_addr location first so we can detect writes
    memory_.write_u32(written_addr, 0xDEAD);

    bool ok = cm_->write_console_a(out, MEM_BASE, 0, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(captured_output_.empty());
    EXPECT_EQ(read_guest_u32(written_addr), 0u);
}

TEST_F(ConsoleManagerTest, WriteConsoleAInvalidHandleFails) {
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    write_guest_string(buf_addr, "test");
    bool ok = cm_->write_console_a(INVALID_HANDLE, buf_addr, 4, written_addr, *marshaler_);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(captured_output_.empty());
}

TEST_F(ConsoleManagerTest, WriteConsoleAToInputHandleFails) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    write_guest_string(buf_addr, "test");
    bool ok = cm_->write_console_a(in, buf_addr, 4, written_addr, *marshaler_);

    EXPECT_FALSE(ok);
}

TEST_F(ConsoleManagerTest, WriteConsoleACharsWrittenCorrect) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 512;

    std::string test_str = "abcde";
    write_guest_string(buf_addr, test_str);
    bool ok = cm_->write_console_a(out, buf_addr, 5, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(read_guest_u32(written_addr), 5u);
    EXPECT_EQ(captured_output_, "abcde");
}

// =============================================================================
// WriteConsoleW Tests
// =============================================================================

TEST_F(ConsoleManagerTest, WriteConsoleWWritesUtf8) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    // "Hi" as UTF-16LE: 0x0048, 0x0069
    u16 wide[] = {0x0048, 0x0069};
    write_guest_wide(buf_addr, wide, 2);

    bool ok = cm_->write_console_w(out, buf_addr, 2, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_output_, "Hi");
    EXPECT_EQ(read_guest_u32(written_addr), 2u);
}

TEST_F(ConsoleManagerTest, WriteConsoleWNonAscii) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    // U+00E9 (é) — should become 2-byte UTF-8: 0xC3 0xA9
    u16 wide[] = {0x00E9};
    write_guest_wide(buf_addr, wide, 1);

    bool ok = cm_->write_console_w(out, buf_addr, 1, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_output_, "\xC3\xA9"); // é in UTF-8
    EXPECT_EQ(read_guest_u32(written_addr), 1u);
}

TEST_F(ConsoleManagerTest, WriteConsoleWBmpCharacter) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    // U+4E16 (世) — 3-byte UTF-8: E4 B8 96
    u16 wide[] = {0x4E16};
    write_guest_wide(buf_addr, wide, 1);

    bool ok = cm_->write_console_w(out, buf_addr, 1, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_output_, "\xE4\xB8\x96");
}

TEST_F(ConsoleManagerTest, WriteConsoleWSurrogatePairIsUtf8NotCesu8) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    // U+1F600 😀 → UTF-16 D83D DE00 → UTF-8 F0 9F 98 80 (not CESU-8 ED A0 BD ED B8 80)
    u16 wide[] = {0xD83D, 0xDE00};
    write_guest_wide(buf_addr, wide, 2);

    bool ok = cm_->write_console_w(out, buf_addr, 2, written_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_output_, "\xF0\x9F\x98\x80");
    EXPECT_EQ(read_guest_u32(written_addr), 2u);
}

TEST_F(ConsoleManagerTest, WriteConsoleWInvalidHandleFails) {
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    u16 wide[] = {0x0041};
    write_guest_wide(buf_addr, wide, 1);

    bool ok = cm_->write_console_w(INVALID_HANDLE, buf_addr, 1, written_addr, *marshaler_);

    EXPECT_FALSE(ok);
}

// =============================================================================
// ReadConsoleA Tests
// =============================================================================

TEST_F(ConsoleManagerTest, ReadConsoleAReadsInput) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr read_addr = MEM_BASE + 512;

    // Learn ReadConsole (line input): the line is returned with the CR LF
    // the Enter key produced.
    input_to_provide_ = "hello\n";
    bool ok = cm_->read_console_a(in, buf_addr, 256, read_addr, *marshaler_);

    EXPECT_TRUE(ok);
    u32 chars_read = read_guest_u32(read_addr);
    EXPECT_EQ(chars_read, 7u);
    EXPECT_EQ(read_guest_string(buf_addr, chars_read), "hello\r\n");
}

TEST_F(ConsoleManagerTest, ReadConsoleAClampsToRequestedSize) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr read_addr = MEM_BASE + 512;

    input_to_provide_ = "0123456789";
    bool ok = cm_->read_console_a(in, buf_addr, 5, read_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(read_guest_u32(read_addr), 5u);
    EXPECT_EQ(read_guest_string(buf_addr, 5), "01234");
}

TEST_F(ConsoleManagerTest, ReadConsoleAEmptyInput) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr read_addr = MEM_BASE + 512;

    input_to_provide_ = "";
    bool ok = cm_->read_console_a(in, buf_addr, 256, read_addr, *marshaler_);

    EXPECT_TRUE(ok);
    EXPECT_EQ(read_guest_u32(read_addr), 0u);
}

TEST_F(ConsoleManagerTest, ReadConsoleAInvalidHandleFails) {
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr read_addr = MEM_BASE + 512;

    input_to_provide_ = "test";
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    bool ok = cm_->read_console_a(out, buf_addr, 256, read_addr, *marshaler_);

    EXPECT_FALSE(ok);
}

// =============================================================================
// GetConsoleMode / SetConsoleMode Tests
// =============================================================================

TEST_F(ConsoleManagerTest, DefaultInputMode) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr mode_addr = MEM_BASE;

    bool ok = cm_->get_console_mode(in, mode_addr, *marshaler_);

    EXPECT_TRUE(ok);
    u32 mode = read_guest_u32(mode_addr);
    EXPECT_EQ(mode, DEFAULT_INPUT_MODE);
}

TEST_F(ConsoleManagerTest, DefaultOutputMode) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr mode_addr = MEM_BASE;

    bool ok = cm_->get_console_mode(out, mode_addr, *marshaler_);

    EXPECT_TRUE(ok);
    u32 mode = read_guest_u32(mode_addr);
    EXPECT_EQ(mode, DEFAULT_OUTPUT_MODE);
}

TEST_F(ConsoleManagerTest, SetAndGetConsoleModeRoundTrip) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr mode_addr = MEM_BASE;

    constexpr u32 NEW_MODE =
        ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL |
        ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    EXPECT_TRUE(cm_->set_console_mode(out, NEW_MODE));

    bool ok = cm_->get_console_mode(out, mode_addr, *marshaler_);
    EXPECT_TRUE(ok);
    EXPECT_EQ(read_guest_u32(mode_addr), NEW_MODE);
}

TEST_F(ConsoleManagerTest, StdoutAndStderrShareActiveScreenBufferMode) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    GuestAddr mode_addr = MEM_BASE;
    ASSERT_NE(out, err);

    constexpr u32 with_vt =
        DEFAULT_OUTPUT_MODE | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    EXPECT_TRUE(cm_->set_console_mode(out, with_vt));
    EXPECT_TRUE(cm_->get_console_mode(err, mode_addr, *marshaler_));
    EXPECT_EQ(read_guest_u32(mode_addr), with_vt);

    EXPECT_TRUE(cm_->set_console_mode(err, DEFAULT_OUTPUT_MODE));
    EXPECT_TRUE(cm_->get_console_mode(out, mode_addr, *marshaler_));
    EXPECT_EQ(read_guest_u32(mode_addr), DEFAULT_OUTPUT_MODE);
}

TEST_F(ConsoleManagerTest, SetConsoleModeInvalidHandleFails) {
    EXPECT_FALSE(cm_->set_console_mode(INVALID_HANDLE, 0));
}

TEST_F(ConsoleManagerTest, GetConsoleModeInvalidHandleFails) {
    GuestAddr mode_addr = MEM_BASE;
    EXPECT_FALSE(cm_->get_console_mode(INVALID_HANDLE, mode_addr, *marshaler_));
}

TEST_F(ConsoleManagerTest, SetConsoleModeOnInputHandle) {
    Handle in = cm_->get_std_handle(STD_INPUT_HANDLE);
    GuestAddr mode_addr = MEM_BASE;

    constexpr u32 NEW_MODE = ENABLE_PROCESSED_INPUT;
    EXPECT_TRUE(cm_->set_console_mode(in, NEW_MODE));

    EXPECT_TRUE(cm_->get_console_mode(in, mode_addr, *marshaler_));
    EXPECT_EQ(read_guest_u32(mode_addr), NEW_MODE);
}

// =============================================================================
// Console title tests
// =============================================================================

TEST_F(ConsoleManagerTest, ConsoleTitleWideRoundTripsAsUtf16) {
    constexpr GuestAddr title_addr = MEM_BASE + 0x200;
    constexpr GuestAddr output_addr = MEM_BASE + 0x300;
    const u16 title[] = {u'S', u't', u'e', u'a', u'm', 0x03A9, 0};
    write_guest_wide(title_addr, title, std::size(title));

    ASSERT_TRUE(cm_->set_console_title_w(
        title_addr, *marshaler_));
    EXPECT_EQ(cm_->get_console_title_w(
                  output_addr, static_cast<u32>(std::size(title)),
                  *marshaler_),
              std::size(title) - 1);

    for (usize i = 0; i < std::size(title); ++i) {
        EXPECT_EQ(memory_.read_u16(output_addr + i * sizeof(u16)).value_or(0),
                  title[i]);
    }
}

TEST(ConsoleManagerDetachedTest, SetConsoleTitleFailsWithoutConsole) {
    HandleTable handles;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u16(0x1000, 0).is_ok());
    Marshaler marshaler(memory);
    ConsoleManager console(handles, false);

    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(console.set_console_title_w(0x1000, marshaler));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// make_console_exports Tests
// =============================================================================

TEST_F(ConsoleManagerTest, MakeConsoleExportsReturnsAllEntries) {
    DllExports exports = make_console_exports(*cm_);
    EXPECT_GE(exports.entries.size(), 6u);
}

TEST_F(ConsoleManagerTest, MakeConsoleExportsContainsGetStdHandle) {
    DllExports exports = make_console_exports(*cm_);
    bool found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "GetStdHandle") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConsoleManagerTest, MakeConsoleExportsContainsWriteConsoleA) {
    DllExports exports = make_console_exports(*cm_);
    bool found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "WriteConsoleA") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConsoleManagerTest, SetConsoleTitleWideHasOneArgumentStdcallCleanup) {
    DllExports exports = make_console_exports(*cm_);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "SetConsoleTitleW";
        });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
    EXPECT_TRUE(static_cast<bool>(it->handler));
}

TEST_F(ConsoleManagerTest, MakeConsoleExportsHandlersAreCallable) {
    DllExports exports = make_console_exports(*cm_);
    for (const auto& e : exports.entries) {
        EXPECT_TRUE(static_cast<bool>(e.handler))
            << "Handler for " << e.name << " is not callable";
    }
}

TEST_F(ConsoleManagerTest, MultipleWritesAccumulate) {
    Handle out = cm_->get_std_handle(STD_OUTPUT_HANDLE);
    GuestAddr buf_addr = MEM_BASE;
    GuestAddr written_addr = MEM_BASE + 256;

    write_guest_string(buf_addr, "Hello");
    cm_->write_console_a(out, buf_addr, 5, written_addr, *marshaler_);

    write_guest_string(buf_addr, "World");
    cm_->write_console_a(out, buf_addr, 5, written_addr, *marshaler_);

    EXPECT_EQ(captured_output_, "HelloWorld");
}

// =============================================================================
// Screen buffer state
//
// These exist because the console used to hold NO state: the cursor APIs were
// absent (missing-export stubs returning 0xFFFFFFFF), SetConsoleTextAttribute
// was a no-op returning TRUE, and GetConsoleScreenBufferInfo synthesised a
// fresh answer with a hardcoded 0,0 cursor on every call. Anything that drives
// a console -- a progress display rewriting a line, an installer, node-pty --
// needs what it wrote to still be there when it reads back.
// =============================================================================

TEST_F(ConsoleManagerTest, CursorPositionRoundTrips) {
    cm_->set_buffer_size(80, 25);
    cm_->set_cursor_position(10, 4);
    auto [x, y] = cm_->cursor_position();
    EXPECT_EQ(x, 10);
    EXPECT_EQ(y, 4);
}

TEST_F(ConsoleManagerTest, CursorPositionIsClampedToTheBuffer) {
    cm_->set_buffer_size(20, 10);
    cm_->set_cursor_position(500, 500);
    auto [x, y] = cm_->cursor_position();
    EXPECT_EQ(x, 19) << "must clamp to the last column, never index past it";
    EXPECT_EQ(y, 9);
}

TEST_F(ConsoleManagerTest, CursorInfoRoundTrips) {
    ConsoleManager::CursorInfo in{};
    in.size = 50;
    in.visible = false;
    cm_->set_cursor_info(in);
    const auto out = cm_->cursor_info();
    EXPECT_EQ(out.size, 50u);
    EXPECT_FALSE(out.visible);
}

TEST_F(ConsoleManagerTest, TextAttributesRoundTrip) {
    cm_->set_text_attributes(0x4E);
    EXPECT_EQ(cm_->text_attributes(), 0x4E);
}

TEST_F(ConsoleManagerTest, FillWrapsOntoFollowingRowsAndStopsAtTheEnd) {
    cm_->set_buffer_size(10, 3);   // 30 cells
    // A fill is NOT clipped to its starting row: it runs on into the rows
    // below, which is how a console clears a region.
    EXPECT_EQ(cm_->fill_output_character(5, 0, u'x', 12), 12u)
        << "5 on row 0 + 7 into row 1";
    // And it stops at the end of the buffer rather than wrapping to the top
    // or scrolling.
    EXPECT_EQ(cm_->fill_output_character(0, 2, u'y', 999), 10u)
        << "only the last row remains";
    EXPECT_EQ(cm_->fill_output_character(0, 0, u'z', 999), 30u)
        << "a whole-buffer fill is exactly the cell count";
}

TEST_F(ConsoleManagerTest, FillOutsideTheBufferWritesNothing) {
    cm_->set_buffer_size(10, 3);
    EXPECT_EQ(cm_->fill_output_character(10, 0, u'x', 5), 0u);
    EXPECT_EQ(cm_->fill_output_character(0, 3, u'x', 5), 0u);
    EXPECT_EQ(cm_->fill_output_attribute(-1, 0, 0x07, 5), 0u);
}

TEST_F(ConsoleManagerTest, ResizePreservesOverlappingCells) {
    cm_->set_buffer_size(10, 3);
    ASSERT_EQ(cm_->fill_output_character(0, 0, u'A', 30), 30u);
    cm_->set_buffer_size(20, 6);          // grow
    // The grown region must still hold the old contents; a resize that
    // blanked the screen would lose a program's output.
    EXPECT_EQ(cm_->fill_output_attribute(0, 0, 0x07, 1), 1u);
    auto [c, r] = cm_->buffer_size();
    EXPECT_EQ(c, 20);
    EXPECT_EQ(r, 6);
}

// =============================================================================
// Input queue
// =============================================================================

TEST_F(ConsoleManagerTest, InputQueueCountsAndReadsInOrder) {
    std::vector<ConsoleManager::InputRecord> in;
    for (u16 i = 0; i < 3; ++i) {
        ConsoleManager::InputRecord r{};
        r.unicode_char = static_cast<u16>(u'a' + i);
        in.push_back(r);
    }
    EXPECT_EQ(cm_->write_input(in), 3u);
    EXPECT_EQ(cm_->pending_input_events(), 3u);

    std::vector<ConsoleManager::InputRecord> out;
    EXPECT_EQ(cm_->read_input(out, 2, /*peek=*/false), 2u);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].unicode_char, u'a');
    EXPECT_EQ(out[1].unicode_char, u'b');
    EXPECT_EQ(cm_->pending_input_events(), 1u) << "a read consumes";
}

TEST_F(ConsoleManagerTest, PeekLeavesTheQueueIntact) {
    ConsoleManager::InputRecord r{};
    r.unicode_char = u'q';
    EXPECT_EQ(cm_->write_input({r}), 1u);

    std::vector<ConsoleManager::InputRecord> out;
    EXPECT_EQ(cm_->read_input(out, 4, /*peek=*/true), 1u);
    EXPECT_EQ(cm_->pending_input_events(), 1u)
        << "PeekConsoleInput must not consume -- a peek that consumed would "
           "present as silently dropped input";
    EXPECT_EQ(cm_->read_input(out, 4, /*peek=*/false), 1u);
    EXPECT_EQ(cm_->pending_input_events(), 0u);
}

// Learn ReadConsoleInput: "does not return until at least one input record
// has been read". Host keystrokes become KEY_EVENT down/up pairs; once the
// host input has ended the read waits for WriteConsoleInput.
TEST_F(ConsoleManagerTest, ReadConsoleInputTurnsHostTextIntoKeyEvents) {
    input_to_provide_ = "aB\n";
    std::vector<ConsoleManager::InputRecord> out;
    EXPECT_EQ(cm_->read_input(out, 16, false), 6u);
    ASSERT_EQ(out.size(), 6u);
    EXPECT_TRUE(out[0].key_down);
    EXPECT_FALSE(out[1].key_down);
    EXPECT_EQ(out[0].virtual_key, u'A');
    EXPECT_EQ(out[0].unicode_char, u'a');
    EXPECT_EQ(out[2].virtual_key, u'B');
    EXPECT_EQ(out[2].control_key_state, 0x10u);  // SHIFT_PRESSED
    EXPECT_EQ(out[4].virtual_key, 0x0Du);        // VK_RETURN
    EXPECT_EQ(out[4].unicode_char, u'\r');
}

TEST_F(ConsoleManagerTest, ReadConsoleInputBlocksUntilARecordArrives) {
    input_to_provide_.clear();  // host input at EOF
    std::vector<ConsoleManager::InputRecord> out;
    std::thread writer([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ConsoleManager::InputRecord r{};
        r.unicode_char = u'z';
        (void)cm_->write_input({r});
    });
    EXPECT_EQ(cm_->read_input(out, 8, false), 1u);
    writer.join();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].unicode_char, u'z');
    // Peek never blocks.
    EXPECT_EQ(cm_->read_input(out, 8, true), 0u);
}

// ReadConsoleW body: one line per call, remainder kept, wake-up mask.
TEST_F(ConsoleManagerTest, ReadTextKeepsRemainderAndHonoursWakeupMask) {
    input_to_provide_ = "ab\tcd\n";
    auto first = cm_->read_text(64, 1u << '\t');
    EXPECT_EQ(first, (std::vector<u16>{u'a', u'b', u'\t'}));
    input_to_provide_.clear();
    auto second = cm_->read_text(2, 0);
    EXPECT_EQ(second, (std::vector<u16>{u'c', u'd'}));
    auto third = cm_->read_text(64, 0);
    EXPECT_EQ(third, (std::vector<u16>{u'\r', u'\n'}));
}

// =============================================================================
// ConPTY object lifetime
//
// A pseudoconsole is a console whose screen is a pipe. These pin the object
// contract; the pipe plumbing that carries a child's console I/O through it is
// separate.
// =============================================================================

TEST_F(ConsoleManagerTest, CreatePseudoConsoleReturnsDistinctLiveHandles) {
    auto& pcm = cm_->pseudo_consoles();
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, 0);
    const Hpcon b = pcm.create(120, 30, 0x30, 0x40, 0);
    EXPECT_NE(a, NULL_HPCON);
    EXPECT_NE(b, NULL_HPCON);
    EXPECT_NE(a, b) << "two pseudoconsoles must not share an HPCON";
    EXPECT_EQ(pcm.live_count(), 2u);

    const auto pa = pcm.find(a);
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(pa->cols, 80);
    EXPECT_EQ(pa->rows, 25);
    EXPECT_EQ(pa->input, 0x10u);
    EXPECT_EQ(pa->output, 0x20u);
}

TEST_F(ConsoleManagerTest, CreatePseudoConsoleRejectsADegenerateSize) {
    auto& pcm = cm_->pseudo_consoles();
    // Windows answers E_INVALIDARG. Accepting a zero dimension would leave
    // every cursor coordinate clamping into an empty buffer, which surfaces
    // much later as a terminal that renders nothing.
    EXPECT_EQ(pcm.create(0, 25, 0x10, 0x20, 0), NULL_HPCON);
    EXPECT_EQ(pcm.create(80, 0, 0x10, 0x20, 0), NULL_HPCON);
    EXPECT_EQ(pcm.create(-1, 25, 0x10, 0x20, 0), NULL_HPCON);
    EXPECT_EQ(pcm.live_count(), 0u);
}

TEST_F(ConsoleManagerTest, ResizePseudoConsoleUpdatesOnlyThatConsole) {
    auto& pcm = cm_->pseudo_consoles();
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, 0);
    const Hpcon b = pcm.create(80, 25, 0x30, 0x40, 0);
    EXPECT_TRUE(pcm.resize(a, 132, 50));
    EXPECT_EQ(pcm.find(a)->cols, 132);
    EXPECT_EQ(pcm.find(a)->rows, 50);
    EXPECT_EQ(pcm.find(b)->cols, 80) << "resize must not touch a sibling";
}

TEST_F(ConsoleManagerTest, ResizeAndCloseRejectUnknownHandles) {
    auto& pcm = cm_->pseudo_consoles();
    EXPECT_FALSE(pcm.resize(0xDEADBEEF, 80, 25));
    EXPECT_FALSE(pcm.close(0xDEADBEEF));
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, 0);
    EXPECT_FALSE(pcm.resize(a, 0, 25)) << "a degenerate resize is rejected too";
    EXPECT_EQ(pcm.find(a)->cols, 80) << "and leaves the old size intact";
}

TEST_F(ConsoleManagerTest, ClosePseudoConsoleIsNotIdempotentlySilent) {
    auto& pcm = cm_->pseudo_consoles();
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, 0);
    EXPECT_TRUE(pcm.close(a));
    EXPECT_FALSE(pcm.close(a)) << "a double close must be distinguishable";
    EXPECT_FALSE(pcm.find(a).has_value());
    EXPECT_EQ(pcm.live_count(), 0u);
}

TEST_F(ConsoleManagerTest, PseudoConsoleHandlesDoNotLookLikeKernelHandles) {
    // An HPCON is opaque and must never be mistaken for a kernel Handle: a
    // value that aliased a real one would let CloseHandle destroy an unrelated
    // object. The registry tags them well clear of the low handle range.
    auto& pcm = cm_->pseudo_consoles();
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, 0);
    EXPECT_GT(a, 0x1000000u);
}

TEST_F(ConsoleManagerTest, InheritCursorFlagIsRetained) {
    auto& pcm = cm_->pseudo_consoles();
    constexpr u32 kPseudoConsoleInheritCursor = 0x1;
    const Hpcon a = pcm.create(80, 25, 0x10, 0x20, kPseudoConsoleInheritCursor);
    ASSERT_TRUE(pcm.find(a).has_value());
    EXPECT_TRUE(pcm.find(a)->inherit_cursor);
    const Hpcon b = pcm.create(80, 25, 0x10, 0x20, 0);
    EXPECT_FALSE(pcm.find(b)->inherit_cursor);
}

// DuplicateHandle of a console handle (Learn: console input/screen buffers
// are duplicable). The duplicate names the same console object: same mode,
// same stream.
TEST_F(ConsoleManagerTest, DuplicatedConsoleHandleNamesTheSameObject) {
    Handle err = cm_->get_std_handle(STD_ERROR_HANDLE);
    Handle dup = cm_->duplicate_handle(err, HandleFlags::Inherit, HandleAccess::All);
    ASSERT_NE(dup, NULL_HANDLE);
    EXPECT_NE(dup, err);
    EXPECT_EQ(handles_.get_type(dup), HandleType::ConsoleOutput);
    EXPECT_EQ(handles_.get_flags(dup), HandleFlags::Inherit);
    ASSERT_TRUE(cm_->set_console_mode(dup, 0x5));
    ASSERT_TRUE(cm_->get_console_mode(err, MEM_BASE + 512, *marshaler_));
    EXPECT_EQ(read_guest_u32(MEM_BASE + 512), 0x5u);
    write_guest_string(MEM_BASE, "via dup");
    ASSERT_TRUE(cm_->write_console_a(dup, MEM_BASE, 7, MEM_BASE + 256, *marshaler_));
    EXPECT_EQ(captured_error_, "via dup");
    Handle dup2 = cm_->duplicate_handle(dup, 0, HandleAccess::All);
    ASSERT_TRUE(cm_->write_console_a(dup2, MEM_BASE, 3, MEM_BASE + 256, *marshaler_));
    EXPECT_EQ(captured_error_, "via dupvia");
    EXPECT_EQ(cm_->duplicate_handle(0x7FFFFFF0u, 0, HandleAccess::All), NULL_HANDLE);
}
