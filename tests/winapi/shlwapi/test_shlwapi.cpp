/// @file test_shlwapi.cpp
/// @brief Unit tests for SHLWAPI path utilities

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/shlwapi/shlwapi_utils.hpp>
#include <winapi/system/shlwapi/shlwapi.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/advapi32/registry/registry.hpp>
#include <winapi/kernel32/handle.hpp>
#include <memory>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/locale/locale_string.hpp>
#include <optional>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class ShlwapiTest : public ::testing::Test {
protected:
    using Utils = ShlwapiPathUtils;
};

// =============================================================================
// PathCombineW Tests
// =============================================================================

TEST_F(ShlwapiTest, PathCombine_BasicDirAndFile) {
    // PathCombineW("C:\\Dir", "file.txt") -> "C:\\Dir\\file.txt"
    std::string result = Utils::path_combine("C:\\Dir", "file.txt");
    EXPECT_EQ(result, "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, PathCombine_HandlesTrailingBackslash) {
    // PathCombineW("C:\\Dir\\", "file.txt") -> "C:\\Dir\\file.txt"
    std::string result = Utils::path_combine("C:\\Dir\\", "file.txt");
    EXPECT_EQ(result, "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, PathCombine_MultipleTrailingSlashes) {
    std::string result = Utils::path_combine("C:\\Dir\\\\\\", "file.txt");
    EXPECT_EQ(result, "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, PathCombine_ForwardSlashNormalized) {
    std::string result = Utils::path_combine("C:/Dir", "file.txt");
    EXPECT_EQ(result, "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, PathCombine_EmptyFile) {
    std::string result = Utils::path_combine("C:\\Dir", "");
    EXPECT_EQ(result, "C:\\Dir");
}

TEST_F(ShlwapiTest, PathCombine_EmptyDir) {
    std::string result = Utils::path_combine("", "file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(ShlwapiTest, PathCombine_BothEmpty) {
    std::string result = Utils::path_combine("", "");
    EXPECT_EQ(result, "");
}

TEST_F(ShlwapiTest, PathCombine_AbsoluteFileOverridesDir) {
    // If file is absolute, it replaces dir
    std::string result = Utils::path_combine("C:\\Dir", "D:\\Other\\file.txt");
    EXPECT_EQ(result, "D:\\Other\\file.txt");
}

TEST_F(ShlwapiTest, PathCombine_UNCPathOverridesDir) {
    std::string result = Utils::path_combine("C:\\Dir", "\\\\server\\share");
    EXPECT_EQ(result, "\\\\server\\share");
}

TEST_F(ShlwapiTest, PathCombine_RootedPathOverridesDir) {
    std::string result = Utils::path_combine("C:\\Dir", "\\absolute");
    EXPECT_EQ(result, "\\absolute");
}

TEST_F(ShlwapiTest, PathCombine_SubdirectoryPath) {
    std::string result = Utils::path_combine("C:\\Program Files", "MyApp\\data");
    EXPECT_EQ(result, "C:\\Program Files\\MyApp\\data");
}

TEST_F(ShlwapiTest, PathCombine_RootDrive) {
    std::string result = Utils::path_combine("C:\\", "file.txt");
    EXPECT_EQ(result, "C:\\file.txt");
}

// =============================================================================
// PathAppendW Tests
// =============================================================================

TEST_F(ShlwapiTest, PathAppend_AppendsCorrectly) {
    // PathAppendW uses same logic as PathCombineW
    std::string path = "C:\\Dir";
    std::string result = Utils::path_combine(path, "subdir");
    EXPECT_EQ(result, "C:\\Dir\\subdir");
}

TEST_F(ShlwapiTest, PathAppend_WithTrailingSep) {
    std::string result = Utils::path_combine("C:\\Dir\\", "subdir");
    EXPECT_EQ(result, "C:\\Dir\\subdir");
}

TEST_F(ShlwapiTest, PathAppend_EmptyMore) {
    std::string result = Utils::path_combine("C:\\Dir", "");
    EXPECT_EQ(result, "C:\\Dir");
}

// =============================================================================
// PathFindFileNameW Tests
// =============================================================================

TEST_F(ShlwapiTest, PathFindFileName_BasicPath) {
    // PathFindFileNameW("C:\\Dir\\file.txt") -> "file.txt"
    auto result = Utils::path_find_filename("C:\\Dir\\file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(ShlwapiTest, PathFindFileName_DeepPath) {
    auto result = Utils::path_find_filename("C:\\Users\\Test\\Documents\\readme.md");
    EXPECT_EQ(result, "readme.md");
}

TEST_F(ShlwapiTest, PathFindFileName_NoDirectory) {
    // Just filename returns whole string
    auto result = Utils::path_find_filename("file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(ShlwapiTest, PathFindFileName_ForwardSlash) {
    auto result = Utils::path_find_filename("C:/Dir/file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(ShlwapiTest, PathFindFileName_RootPath) {
    auto result = Utils::path_find_filename("C:\\");
    EXPECT_EQ(result, "");
}

TEST_F(ShlwapiTest, PathFindFileName_Empty) {
    auto result = Utils::path_find_filename("");
    EXPECT_EQ(result, "");
}

TEST_F(ShlwapiTest, PathFindFileName_JustDrive) {
    auto result = Utils::path_find_filename("C:");
    EXPECT_EQ(result, "C:");
}

TEST_F(ShlwapiTest, PathFindFileName_TrailingSlash) {
    auto result = Utils::path_find_filename("C:\\Dir\\");
    EXPECT_EQ(result, "");
}

// =============================================================================
// PathRemoveFileSpecW Tests
// =============================================================================

TEST_F(ShlwapiTest, PathRemoveFileSpec_RemovesFilename) {
    auto [result, changed] = Utils::path_remove_filespec("C:\\Dir\\file.txt");
    EXPECT_EQ(result, "C:\\Dir");
    EXPECT_TRUE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_DeepPath) {
    auto [result, changed] = Utils::path_remove_filespec("C:\\Users\\Test\\file.txt");
    EXPECT_EQ(result, "C:\\Users\\Test");
    EXPECT_TRUE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_TrailingSlashStripped) {
    auto [result, changed] = Utils::path_remove_filespec("C:\\Dir\\subdir\\");
    EXPECT_EQ(result, "C:\\Dir");
    EXPECT_TRUE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_NoSeparator) {
    auto [result, changed] = Utils::path_remove_filespec("file.txt");
    EXPECT_EQ(result, "file.txt");
    EXPECT_FALSE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_RootPath) {
    auto [result, changed] = Utils::path_remove_filespec("C:\\");
    // Windows PathRemoveFileSpec("C:\\") strips trailing sep → "C:", no slash found → unchanged
    EXPECT_EQ(result, "C:");
    EXPECT_FALSE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_Empty) {
    auto [result, changed] = Utils::path_remove_filespec("");
    EXPECT_EQ(result, "");
    EXPECT_FALSE(changed);
}

TEST_F(ShlwapiTest, PathRemoveFileSpec_ForwardSlash) {
    auto [result, changed] = Utils::path_remove_filespec("C:/Dir/file.txt");
    EXPECT_EQ(result, "C:\\Dir");
    EXPECT_TRUE(changed);
}

// =============================================================================
// PathAddBackslashW Tests
// =============================================================================

TEST_F(ShlwapiTest, PathAddBackslash_AddsWhenMissing) {
    std::string result = Utils::path_add_backslash("C:\\Dir");
    EXPECT_EQ(result, "C:\\Dir\\");
}

TEST_F(ShlwapiTest, PathAddBackslash_AlreadyPresent) {
    std::string result = Utils::path_add_backslash("C:\\Dir\\");
    EXPECT_EQ(result, "C:\\Dir\\");
}

TEST_F(ShlwapiTest, PathAddBackslash_Empty) {
    std::string result = Utils::path_add_backslash("");
    EXPECT_EQ(result, "\\");
}

TEST_F(ShlwapiTest, PathAddBackslash_NormalizesForwardSlash) {
    std::string result = Utils::path_add_backslash("C:/Dir/");
    EXPECT_EQ(result, "C:\\Dir\\");
}

// =============================================================================
// StrChrW Tests
// =============================================================================

TEST_F(ShlwapiTest, StrChrW_FindsCharacter) {
    auto idx = Utils::str_chr_w(L"Hello World", L'o');
    EXPECT_EQ(idx, 4);
}

TEST_F(ShlwapiTest, StrChrW_FindsFirstOccurrence) {
    auto idx = Utils::str_chr_w(L"Hello World", L'l');
    EXPECT_EQ(idx, 2);
}

TEST_F(ShlwapiTest, StrChrW_CharNotFound) {
    auto idx = Utils::str_chr_w(L"Hello World", L'z');
    EXPECT_EQ(idx, -1);
}

TEST_F(ShlwapiTest, StrChrW_EmptyString) {
    auto idx = Utils::str_chr_w(L"", L'a');
    EXPECT_EQ(idx, -1);
}

TEST_F(ShlwapiTest, StrChrW_FindsFirstChar) {
    auto idx = Utils::str_chr_w(L"Hello", L'H');
    EXPECT_EQ(idx, 0);
}

TEST_F(ShlwapiTest, StrChrW_FindsLastChar) {
    auto idx = Utils::str_chr_w(L"Hello", L'o');
    EXPECT_EQ(idx, 4);
}

TEST_F(ShlwapiTest, StrChrW_FindsBackslash) {
    auto idx = Utils::str_chr_w(L"C:\\Dir\\file.txt", L'\\');
    EXPECT_EQ(idx, 2);
}

// =============================================================================
// StrStrIW Tests (case-insensitive substring search)
// =============================================================================

TEST_F(ShlwapiTest, StrStrW_FindsSubstring) {
    auto idx = Utils::str_str_iw(L"Hello World", L"World");
    EXPECT_EQ(idx, 6);
}

TEST_F(ShlwapiTest, StrStrW_CaseInsensitive) {
    auto idx = Utils::str_str_iw(L"Hello World", L"WORLD");
    EXPECT_EQ(idx, 6);
}

TEST_F(ShlwapiTest, StrStrW_MixedCase) {
    auto idx = Utils::str_str_iw(L"HELLO WORLD", L"world");
    EXPECT_EQ(idx, 6);
}

TEST_F(ShlwapiTest, StrStrW_SubstringNotFound) {
    auto idx = Utils::str_str_iw(L"Hello World", L"xyz");
    EXPECT_EQ(idx, -1);
}

TEST_F(ShlwapiTest, StrStrW_EmptyNeedle) {
    auto idx = Utils::str_str_iw(L"Hello World", L"");
    EXPECT_EQ(idx, 0);
}

TEST_F(ShlwapiTest, StrStrW_EmptyHaystack) {
    auto idx = Utils::str_str_iw(L"", L"test");
    EXPECT_EQ(idx, -1);
}

TEST_F(ShlwapiTest, StrStrW_NeedleLongerThanHaystack) {
    auto idx = Utils::str_str_iw(L"Hi", L"Hello World");
    EXPECT_EQ(idx, -1);
}

TEST_F(ShlwapiTest, StrStrW_ExactMatch) {
    auto idx = Utils::str_str_iw(L"Hello", L"Hello");
    EXPECT_EQ(idx, 0);
}

TEST_F(ShlwapiTest, StrStrW_PartialMatchAtStart) {
    auto idx = Utils::str_str_iw(L"Hello World", L"Hell");
    EXPECT_EQ(idx, 0);
}

TEST_F(ShlwapiTest, StrStrW_PartialMatchAtEnd) {
    auto idx = Utils::str_str_iw(L"Hello World", L"rld");
    EXPECT_EQ(idx, 8);
}

// =============================================================================
// PathIsRelativeW Tests
// =============================================================================

TEST_F(ShlwapiTest, IsRelative_Empty) {
    EXPECT_TRUE(Utils::is_relative_ascii_path(""));
}

TEST_F(ShlwapiTest, IsRelative_RelativePath) {
    EXPECT_TRUE(Utils::is_relative_ascii_path("dir\\file.txt"));
}

TEST_F(ShlwapiTest, IsRelative_JustFilename) {
    EXPECT_TRUE(Utils::is_relative_ascii_path("file.txt"));
}

TEST_F(ShlwapiTest, IsRelative_DriveAbsolute) {
    EXPECT_FALSE(Utils::is_relative_ascii_path("C:\\Dir\\file.txt"));
}

TEST_F(ShlwapiTest, IsRelative_DriveAbsoluteForwardSlash) {
    EXPECT_FALSE(Utils::is_relative_ascii_path("C:/Dir/file.txt"));
}

TEST_F(ShlwapiTest, IsRelative_UNCPath) {
    EXPECT_FALSE(Utils::is_relative_ascii_path("\\\\server\\share"));
}

TEST_F(ShlwapiTest, IsRelative_RootedPath) {
    EXPECT_FALSE(Utils::is_relative_ascii_path("\\dir\\file.txt"));
}

TEST_F(ShlwapiTest, IsRelative_DriveOnly) {
    // "C:" without separator is relative according to Windows semantics
    EXPECT_TRUE(Utils::is_relative_ascii_path("C:"));
}

TEST_F(ShlwapiTest, IsRelative_DriveWithFile) {
    // "C:file.txt" is relative to current directory on C:
    EXPECT_TRUE(Utils::is_relative_ascii_path("C:file.txt"));
}

// =============================================================================
// Helper Function Tests
// =============================================================================

TEST_F(ShlwapiTest, ToLowerAscii_UpperCase) {
    EXPECT_EQ(Utils::tolower_ascii_u16(u'A'), u'a');
    EXPECT_EQ(Utils::tolower_ascii_u16(u'Z'), u'z');
}

TEST_F(ShlwapiTest, ToLowerAscii_LowerCase) {
    EXPECT_EQ(Utils::tolower_ascii_u16(u'a'), u'a');
    EXPECT_EQ(Utils::tolower_ascii_u16(u'z'), u'z');
}

TEST_F(ShlwapiTest, ToLowerAscii_NonAlpha) {
    EXPECT_EQ(Utils::tolower_ascii_u16(u'0'), u'0');
    EXPECT_EQ(Utils::tolower_ascii_u16(u'@'), u'@');
    EXPECT_EQ(Utils::tolower_ascii_u16(u'\\'), u'\\');
}

TEST_F(ShlwapiTest, IsPathSep_Backslash) {
    EXPECT_TRUE(Utils::is_path_sep('\\'));
}

TEST_F(ShlwapiTest, IsPathSep_ForwardSlash) {
    EXPECT_TRUE(Utils::is_path_sep('/'));
}

TEST_F(ShlwapiTest, IsPathSep_NotSeparator) {
    EXPECT_FALSE(Utils::is_path_sep('a'));
    EXPECT_FALSE(Utils::is_path_sep(':'));
    EXPECT_FALSE(Utils::is_path_sep('.'));
}

TEST_F(ShlwapiTest, NormalizeSeps_ForwardToBack) {
    EXPECT_EQ(Utils::normalize_seps("C:/Dir/file.txt"), "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, NormalizeSeps_AlreadyBackslash) {
    EXPECT_EQ(Utils::normalize_seps("C:\\Dir\\file.txt"), "C:\\Dir\\file.txt");
}

TEST_F(ShlwapiTest, NormalizeSeps_Mixed) {
    EXPECT_EQ(Utils::normalize_seps("C:/Dir\\file/sub"), "C:\\Dir\\file\\sub");
}

TEST_F(ShlwapiTest, StripTrailingSeps_SingleBackslash) {
    EXPECT_EQ(Utils::strip_trailing_seps("C:\\Dir\\"), "C:\\Dir");
}

TEST_F(ShlwapiTest, StripTrailingSeps_MultipleSlashes) {
    EXPECT_EQ(Utils::strip_trailing_seps("C:\\Dir\\//\\"), "C:\\Dir");
}

TEST_F(ShlwapiTest, StripTrailingSeps_NoTrailing) {
    EXPECT_EQ(Utils::strip_trailing_seps("C:\\Dir"), "C:\\Dir");
}

TEST_F(ShlwapiTest, StripTrailingSeps_Empty) {
    EXPECT_EQ(Utils::strip_trailing_seps(""), "");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(ShlwapiTest, EdgeCase_VeryLongPath) {
    std::string long_dir(200, 'a');
    std::string file = "test.txt";
    std::string result = Utils::path_combine(long_dir, file);
    EXPECT_EQ(result, long_dir + "\\" + file);
}

TEST_F(ShlwapiTest, EdgeCase_PathWithSpaces) {
    std::string result = Utils::path_combine("C:\\Program Files", "My App\\data.txt");
    EXPECT_EQ(result, "C:\\Program Files\\My App\\data.txt");
}

TEST_F(ShlwapiTest, EdgeCase_PathWithDots) {
    // Note: path_combine doesn't resolve . or .. - it just concatenates
    std::string result = Utils::path_combine("C:\\Dir", "..\\Other");
    EXPECT_EQ(result, "C:\\Dir\\..\\Other");
}

TEST_F(ShlwapiTest, EdgeCase_FindFilename_OnlySeparators) {
    auto result = Utils::path_find_filename("\\\\\\");
    EXPECT_EQ(result, "");
}

// =============================================================================
// SHStripMneumonic (ordinals 225 / 203)
// =============================================================================

// Spotify's launcher delay-loads this by ordinal. When the export was missing,
// GetProcAddress returned NULL, the Visual C++ delay-load helper raised
// 0xC06D007F (ERROR_PROC_NOT_FOUND), and the guest then called the address the
// helper never filled in — landing at EIP 0.
class ShStripMneumonicTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find(const char* name) const {
        for (const auto& e : exports.entries) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }

    void write_wide(const std::wstring& text) {
        for (usize i = 0; i < text.size(); ++i) {
            (void)memory.write_u16(BASE + static_cast<u32>(i) * 2,
                                   static_cast<u16>(text[i]));
        }
        (void)memory.write_u16(BASE + static_cast<u32>(text.size()) * 2, 0);
    }

    std::wstring read_wide() const {
        std::wstring out;
        for (u32 i = 0; i < 256; ++i) {
            auto v = memory.read_u16(BASE + i * 2);
            if (v.is_err() || v.value() == 0) break;
            out.push_back(static_cast<wchar_t>(v.value()));
        }
        return out;
    }

    u32 call(const char* name) {
        const ExportEntry* entry = find(name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0;
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4), BASE);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(ShStripMneumonicTest, ExportedByNameAndDocumentedOrdinal) {
    const ExportEntry* wide = find("SHStripMneumonicW");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->ordinal, 225u) << "delay-load imports it by ordinal";
    EXPECT_EQ(static_cast<u32>(wide->stdcall_bytes), 4u);

    const ExportEntry* ansi = find("SHStripMneumonicA");
    ASSERT_NE(ansi, nullptr);
    EXPECT_EQ(ansi->ordinal, 203u);
    EXPECT_EQ(static_cast<u32>(ansi->stdcall_bytes), 4u);
}

TEST_F(ShStripMneumonicTest, StripsInlineMarkerAndReturnsCharacter) {
    write_wide(L"Str&ing");
    EXPECT_EQ(call("SHStripMneumonicW"), static_cast<u32>('i'));
    EXPECT_EQ(read_wide(), L"String");
}

TEST_F(ShStripMneumonicTest, StripsParenthesizedFormAndPrecedingSpace) {
    write_wide(L"String (&S)");
    EXPECT_EQ(call("SHStripMneumonicW"), static_cast<u32>('S'));
    EXPECT_EQ(read_wide(), L"String");
}

TEST_F(ShStripMneumonicTest, NoMarkerReturnsZeroAndLeavesTextAlone) {
    write_wide(L"Plain");
    EXPECT_EQ(call("SHStripMneumonicW"), 0u);
    EXPECT_EQ(read_wide(), L"Plain");
}

// "&&" is the escape for a literal ampersand and is not the mnemonic.
TEST_F(ShStripMneumonicTest, DoubledAmpersandCollapsesAndIsNotTheMnemonic) {
    write_wide(L"Fish && Chips");
    EXPECT_EQ(call("SHStripMneumonicW"), 0u);
    EXPECT_EQ(read_wide(), L"Fish & Chips");

    write_wide(L"A && B&C");
    EXPECT_EQ(call("SHStripMneumonicW"), static_cast<u32>('C'));
    EXPECT_EQ(read_wide(), L"A & BC");
}

TEST_F(ShStripMneumonicTest, NullPointerReturnsZero) {
    const ExportEntry* entry = find("SHStripMneumonicW");
    ASSERT_NE(entry, nullptr);
    (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4), 0);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    entry->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// The ANSI variant has no parenthesized form; that is Unicode-only.
TEST_F(ShStripMneumonicTest, AnsiVariantStripsInlineMarkerOnly) {
    const char* text = "Str&ing";
    for (u32 i = 0; text[i] != 0; ++i) {
        (void)memory.write_u8(BASE + i, static_cast<u8>(text[i]));
    }
    (void)memory.write_u8(BASE + 7, 0);
    EXPECT_EQ(call("SHStripMneumonicA"), static_cast<u32>('i'));
    std::string out;
    for (u32 i = 0; i < 32; ++i) {
        auto v = memory.read_u8(BASE + i);
        if (v.is_err() || v.value() == 0) break;
        out.push_back(static_cast<char>(v.value()));
    }
    EXPECT_EQ(out, "String");
}

// =============================================================================
// PathMatchSpecW
// =============================================================================

class PathMatchSpecTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find() const {
        for (const auto& entry : exports.entries) {
            if (entry.name == "PathMatchSpecW") return &entry;
        }
        return nullptr;
    }

    void write_wide(GuestAddr address, const std::wstring& text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(
                address + static_cast<u32>(i) * 2,
                static_cast<u16>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(
            address + static_cast<u32>(text.size()) * 2, 0).is_ok());
    }

    u32 call(GuestAddr path, GuestAddr spec) {
        const ExportEntry* entry = find();
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0;
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4),
                               static_cast<u32>(path));
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 8),
                               static_cast<u32>(spec));
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr SPEC = 0x3000;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(PathMatchSpecTest, ExportedByNameWithTwoStdcallArguments) {
    const ExportEntry* entry = find();
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ordinal, 0u);
    EXPECT_EQ(static_cast<u32>(entry->stdcall_bytes), 8u);
}

TEST_F(PathMatchSpecTest, MatchesExtensionCaseInsensitively) {
    write_wide(BASE, L"C:\\Users\\Default\\cache.PAK");
    write_wide(SPEC, L"*.pak");
    EXPECT_EQ(call(BASE, SPEC), 1u);
}

TEST_F(PathMatchSpecTest, QuestionMarkMatchesExactlyOneCharacter) {
    write_wide(BASE, L"chrome_100_percent.pak");
    write_wide(SPEC, L"chrome_?00_percent.pak");
    EXPECT_EQ(call(BASE, SPEC), 1u);

    write_wide(BASE, L"chrome_1000_percent.pak");
    EXPECT_EQ(call(BASE, SPEC), 0u);
}

TEST_F(PathMatchSpecTest, StarCanMatchZeroOrManyCharacters) {
    write_wide(SPEC, L"lib*.dll");

    write_wide(BASE, L"lib.dll");
    EXPECT_EQ(call(BASE, SPEC), 1u);

    write_wide(BASE, L"libcef.dll");
    EXPECT_EQ(call(BASE, SPEC), 1u);
}

TEST_F(PathMatchSpecTest, ExactPatternRejectsDifferentPath) {
    write_wide(BASE, L"C:\\Spotify\\resources.pak");
    write_wide(SPEC, L"C:\\Spotify\\chrome.dll");
    EXPECT_EQ(call(BASE, SPEC), 0u);
}

TEST_F(PathMatchSpecTest, NullPointersFailWithoutGuestMemoryAccess) {
    EXPECT_EQ(call(0, SPEC), 0u);
    EXPECT_EQ(call(BASE, 0), 0u);
}

// =============================================================================
// PathFindExtensionA — ANSI twin of PathFindExtensionW
// =============================================================================

class PathFindExtensionATest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find() const {
        for (const auto& entry : exports.entries) {
            if (entry.name == "PathFindExtensionA") return &entry;
        }
        return nullptr;
    }

    void write_ansi(const char* text) {
        for (usize i = 0; text[i] != 0; ++i) {
            ASSERT_TRUE(memory.write_u8(
                BASE + static_cast<u32>(i),
                static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(BASE + static_cast<u32>(std::strlen(text)),
                                    0).is_ok());
    }

    GuestAddr call(GuestAddr path) {
        const ExportEntry* entry = find();
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0;
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4),
                               static_cast<u32>(path));
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<GuestAddr>(cpu.gpr.rax);
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(PathFindExtensionATest, ReturnsPointerToDot) {
    write_ansi("C:\\Temp\\game.exe");
    const GuestAddr result = call(BASE);
    EXPECT_EQ(result, BASE + 12); // ".exe"
    EXPECT_EQ(memory.read_u8(result).value_or(0), static_cast<u8>('.'));
}

TEST_F(PathFindExtensionATest, NoExtensionReturnsNullTerminator) {
    write_ansi("C:\\Temp\\README");
    const GuestAddr result = call(BASE);
    EXPECT_EQ(result, BASE + 14);
    EXPECT_EQ(memory.read_u8(result).value_or(1), 0u);
}

TEST_F(PathFindExtensionATest, DotInDirectoryDoesNotCount) {
    const char* path = "C:\\foo.bar\\file";
    write_ansi(path);
    const GuestAddr result = call(BASE);
    EXPECT_EQ(result, BASE + std::strlen(path));
    EXPECT_EQ(memory.read_u8(result).value_or(1), 0u);
}

TEST_F(PathFindExtensionATest, PathFindFileNameAReturnsBasename) {
    const ExportEntry* entry = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "PathFindFileNameA") { entry = &e; break; }
    }
    ASSERT_NE(entry, nullptr);
    write_ansi("C:\\Temp\\game.exe");
    (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4),
                           static_cast<u32>(BASE));
    Marshaler m(memory);
    m.attach_cpu(cpu);
    entry->handler(m);
    const GuestAddr result = static_cast<GuestAddr>(cpu.gpr.rax);
    EXPECT_EQ(result, BASE + 8); // "game.exe"
    EXPECT_EQ(memory.read_u8(result).value_or(0), static_cast<u8>('g'));
}

class PathCombineATest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find(const char* name) const {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void write_ansi(GuestAddr addr, const char* text) {
        for (usize i = 0; text[i] != 0; ++i) {
            ASSERT_TRUE(memory.write_u8(
                addr + static_cast<u32>(i),
                static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(
            addr + static_cast<u32>(std::strlen(text)), 0).is_ok());
    }

    std::string read_ansi(GuestAddr addr) {
        std::string out;
        for (u32 i = 0; i < 260; ++i) {
            const u8 ch = memory.read_u8(addr + i).value_or(0);
            if (ch == 0) break;
            out.push_back(static_cast<char>(ch));
        }
        return out;
    }

    GuestAddr call_combine(GuestAddr dest, GuestAddr dir, GuestAddr file) {
        const ExportEntry* entry = find("PathCombineA");
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0;
        const u32 sp = static_cast<u32>(cpu.sp());
        (void)memory.write_u32(sp + 4, static_cast<u32>(dest));
        (void)memory.write_u32(sp + 8, static_cast<u32>(dir));
        (void)memory.write_u32(sp + 12, static_cast<u32>(file));
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<GuestAddr>(cpu.gpr.rax);
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(PathCombineATest, CombinesDirectoryAndFile) {
    constexpr GuestAddr dest = BASE;
    constexpr GuestAddr dir = BASE + 0x100;
    constexpr GuestAddr file = BASE + 0x180;
    write_ansi(dir, "C:\\Program Files\\App");
    write_ansi(file, "game.exe");
    const GuestAddr result = call_combine(dest, dir, file);
    EXPECT_EQ(result, dest);
    EXPECT_EQ(read_ansi(dest), "C:\\Program Files\\App\\game.exe");
}

TEST_F(PathCombineATest, PublishesAnsiPathTwins) {
    EXPECT_NE(find("PathCombineA"), nullptr);
    EXPECT_NE(find("PathAppendA"), nullptr);
    EXPECT_NE(find("PathRemoveFileSpecA"), nullptr);
    EXPECT_NE(find("PathIsRelativeA"), nullptr);
    EXPECT_NE(find("PathFileExistsA"), nullptr);
}

TEST_F(PathCombineATest, PathIsDirectoryWHasStdcallOneArg) {
    // Learn: BOOL PathIsDirectoryW(LPCWSTR) — x86 stdcall pops 4 bytes.
    // Missing-export fail-thunks use stdcall_bytes=0 and corrupt guest stacks.
    const ExportEntry* entry = find("PathIsDirectoryW");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(static_cast<u32>(entry->stdcall_bytes), 4u);

    (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4), 0);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    entry->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

class PathRenameExtensionATest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find(const char* name) const {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void write_ansi(GuestAddr addr, const char* text) {
        for (usize i = 0; text[i] != 0; ++i) {
            ASSERT_TRUE(memory.write_u8(
                addr + static_cast<u32>(i),
                static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(
            addr + static_cast<u32>(std::strlen(text)), 0).is_ok());
    }

    std::string read_ansi(GuestAddr addr) {
        std::string out;
        for (usize i = 0; i < 260; ++i) {
            const auto ch = memory.read_u8(addr + static_cast<u32>(i));
            if (ch.is_err() || ch.value() == 0) break;
            out.push_back(static_cast<char>(ch.value()));
        }
        return out;
    }

    u32 call(GuestAddr path, GuestAddr ext) {
        const ExportEntry* entry = find("PathRenameExtensionA");
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0;
        EXPECT_EQ(entry->stdcall_bytes, 8);
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4),
                               static_cast<u32>(path));
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 8),
                               static_cast<u32>(ext));
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr EXT = 0x2800;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(PathRenameExtensionATest, ReplacesExistingExtension) {
    write_ansi(BASE, "C:\\Bin\\payload.ex_");
    write_ansi(EXT, ".exe");
    EXPECT_EQ(call(BASE, EXT), 1u);
    EXPECT_EQ(read_ansi(BASE), "C:\\Bin\\payload.exe");
}

TEST_F(PathRenameExtensionATest, AppendsExtensionWhenMissing) {
    write_ansi(BASE, "C:\\Temp\\README");
    write_ansi(EXT, ".txt");
    EXPECT_EQ(call(BASE, EXT), 1u);
    EXPECT_EQ(read_ansi(BASE), "C:\\Temp\\README.txt");
}

TEST_F(PathRenameExtensionATest, RejectsResultThatExceedsMaxPath) {
    std::string path(256, 'a');
    write_ansi(BASE, path.c_str());
    write_ansi(EXT, ".exe");
    EXPECT_EQ(call(BASE, EXT), 0u);
    EXPECT_EQ(read_ansi(BASE), path);
}

TEST_F(PathRenameExtensionATest, PublishesWideExport) {
    const ExportEntry* wide = find("PathRenameExtensionW");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->stdcall_bytes, 8);
}

// =============================================================================
// SHDeleteKeyW / SHDeleteKeyA
//
// Learn: "Deletes a subkey and all its descendants", returning a system error
// code. The recursion is the point — RegDeleteKey on NT refuses a key that
// still has subkeys. Unity's PlayerPrefs.DeleteAll goes through this export,
// and before this both forms were stubs (SHDeleteKeyA fabricated
// ERROR_FILE_NOT_FOUND; SHDeleteKeyW did not exist at all, so it reached the
// missing-export stub and returned 0xFFFFFFFF).
// =============================================================================

class ShDeleteKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        thunks = std::make_unique<RegistryThunks>(registry, handles);
        exports = create_shlwapi_thunks(nullptr, nullptr, thunks.get());
        ASSERT_TRUE(memory.map_region(
            0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x8000;
    }

    const ExportEntry* find(std::string_view name) const {
        for (const auto& e : exports.entries) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }

    void write_wide(GuestAddr addr, std::wstring_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(
                addr + i * 2, static_cast<u16>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(addr + text.size() * 2, 0).is_ok());
    }

    void write_ansi(GuestAddr addr, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u8(
                addr + i, static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(addr + text.size(), 0).is_ok());
    }

    u32 call(const char* export_name, u64 hkey, GuestAddr subkey) {
        const ExportEntry* entry = find(export_name);
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0xFFFFFFFFu;
        cpu.gpr.rcx = hkey;
        cpu.gpr.rdx = subkey;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// Build HKCU\Software\<company>\<product> with a value, the shape Unity
    /// PlayerPrefs creates and DeleteAll then removes.
    void seed_player_prefs() {
        u64 key = 0;
        u32 disposition = 0;
        ASSERT_EQ(thunks->reg_create_key_ex_w(
                      capsule::HKEY_CURRENT_USER,
                      L"Software\\Johan Gronvall\\BoplBattle",
                      &key, &disposition),
                  ERROR_SUCCESS);
        const u32 value = 7;
        ASSERT_EQ(thunks->reg_set_value_ex_w(
                      key, L"Volume", REG_DWORD,
                      reinterpret_cast<const u8*>(&value), sizeof(value)),
                  ERROR_SUCCESS);
        ASSERT_EQ(thunks->reg_close_key(key), ERROR_SUCCESS);
    }

    u32 open_status(const std::wstring& path) {
        u64 key = 0;
        const u32 err =
            thunks->reg_open_key_ex_w(capsule::HKEY_CURRENT_USER, path, 0, 0, &key);
        if (err == ERROR_SUCCESS) (void)thunks->reg_close_key(key);
        return err;
    }

    static constexpr GuestAddr SUBKEY = 0x2000;
    capsule::Registry registry;
    HandleTable handles;
    std::unique_ptr<RegistryThunks> thunks;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(ShDeleteKeyTest, BothFormsRegisteredAsStdcallTwoArgs) {
    const ExportEntry* wide = find("SHDeleteKeyW");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->stdcall_bytes, 8u);

    const ExportEntry* ansi = find("SHDeleteKeyA");
    ASSERT_NE(ansi, nullptr);
    EXPECT_EQ(ansi->stdcall_bytes, 8u);
}

TEST_F(ShDeleteKeyTest, WideFormDeletesKeyAndAllDescendants) {
    seed_player_prefs();
    ASSERT_EQ(open_status(L"Software\\Johan Gronvall\\BoplBattle"),
              ERROR_SUCCESS);

    // The parent still has a subkey, which is exactly what plain RegDeleteKey
    // refuses; SHDeleteKey must remove the whole tree.
    write_wide(SUBKEY, L"Software\\Johan Gronvall");
    EXPECT_EQ(call("SHDeleteKeyW", capsule::HKEY_CURRENT_USER, SUBKEY), ERROR_SUCCESS);

    EXPECT_NE(open_status(L"Software\\Johan Gronvall\\BoplBattle"),
              ERROR_SUCCESS);
    EXPECT_NE(open_status(L"Software\\Johan Gronvall"), ERROR_SUCCESS);
}

TEST_F(ShDeleteKeyTest, AnsiFormDeletesKeyAndAllDescendants) {
    seed_player_prefs();
    write_ansi(SUBKEY, "Software\\Johan Gronvall");
    EXPECT_EQ(call("SHDeleteKeyA", capsule::HKEY_CURRENT_USER, SUBKEY), ERROR_SUCCESS);
    EXPECT_NE(open_status(L"Software\\Johan Gronvall"), ERROR_SUCCESS);
}

TEST_F(ShDeleteKeyTest, MissingKeyReportsFileNotFound) {
    write_wide(SUBKEY, L"Software\\No Such Vendor");
    const u32 err = call("SHDeleteKeyW", capsule::HKEY_CURRENT_USER, SUBKEY);
    EXPECT_NE(err, ERROR_SUCCESS);          // no fake success
    EXPECT_NE(err, 0xFFFFFFFFu);            // no missing-export stub value
    EXPECT_EQ(err, 2u);                     // ERROR_FILE_NOT_FOUND
}

TEST_F(ShDeleteKeyTest, NullSubkeyClearsKeyInPlace) {
    seed_player_prefs();
    // A NULL pszSubKey clears the values and subkeys of hkey; the key the
    // caller holds open survives.
    u64 company = 0;
    ASSERT_EQ(thunks->reg_open_key_ex_w(
                  capsule::HKEY_CURRENT_USER, L"Software\\Johan Gronvall", 0, 0,
                  &company),
              ERROR_SUCCESS);
    EXPECT_EQ(call("SHDeleteKeyW", company, 0), ERROR_SUCCESS);
    EXPECT_EQ(thunks->reg_close_key(company), ERROR_SUCCESS);

    EXPECT_NE(open_status(L"Software\\Johan Gronvall\\BoplBattle"),
              ERROR_SUCCESS);
    EXPECT_EQ(open_status(L"Software\\Johan Gronvall"), ERROR_SUCCESS);
}

TEST_F(ShDeleteKeyTest, WithoutRegistryFailsHonestly) {
    // A runtime that never wired the registry must not report a successful
    // delete: ERROR_INVALID_HANDLE, never ERROR_SUCCESS.
    DllExports unwired = create_shlwapi_thunks(nullptr, nullptr, nullptr);
    const ExportEntry* entry = nullptr;
    for (const auto& e : unwired.entries) {
        if (e.name == "SHDeleteKeyW") entry = &e;
    }
    ASSERT_NE(entry, nullptr);

    write_wide(SUBKEY, L"Software\\Johan Gronvall");
    cpu.gpr.rcx = capsule::HKEY_CURRENT_USER;
    cpu.gpr.rdx = SUBKEY;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    entry->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 6u); // ERROR_INVALID_HANDLE
}

// =============================================================================
// S7 audit: shlwapi path contracts measured on Windows 11
// (tools/oracle_probe/s7_shlwapi_probe.expected; x86 and x64 agree except
// IsOS(OS_WOW6432)). Driven through the exports as a 32-bit guest.
// =============================================================================

class ShlwapiS7Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        exports = create_shlwapi_thunks();
    }

    const ExportEntry* find(std::string_view name) const {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void put(GuestAddr at, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(at + i * 2,
                static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(at + text.size() * 2, 0).is_ok());
    }

    std::string get(GuestAddr at) {
        std::string out;
        for (usize i = 0; i < 1024; ++i) {
            const u16 unit = memory.read_u16(at + i * 2).value_or(0);
            if (unit == 0) break;
            out.push_back(static_cast<char>(unit));
        }
        return out;
    }

    u32 call(std::string_view name, std::initializer_list<u32> args) {
        const ExportEntry* entry = find(name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xBAD;
        EXPECT_EQ(entry->stdcall_bytes, args.size() * 4) << name;
        u32 slot = 1;
        for (u32 value : args) {
            (void)memory.write_u32(static_cast<u32>(cpu.sp() + slot * 4), value);
            ++slot;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    std::string canonicalize(std::string_view in) {
        put(SRC, in);
        put(DST, "poison");
        EXPECT_EQ(call("PathCanonicalizeW", {DST, SRC}), 1u) << in;
        return get(DST);
    }

    std::string append(std::string_view path, std::string_view more) {
        put(DST, path);
        put(SRC, more);
        EXPECT_EQ(call("PathAppendW", {DST, SRC}), 1u) << path << " + " << more;
        return get(DST);
    }

    bool match(std::string_view path, std::string_view spec) {
        put(SRC, path);
        put(DST, spec);
        return call("PathMatchSpecW", {SRC, DST}) != 0;
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr SRC = BASE;
    static constexpr GuestAddr DST = BASE + 0x1000;
    static constexpr GuestAddr STACK = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(ShlwapiS7Test, PathCanonicalizeWMatchesWindows) {
    EXPECT_EQ(canonicalize("C:\\a\\b\\..\\c"), "C:\\a\\c");
    EXPECT_EQ(canonicalize("C:\\.."), "C:\\");
    EXPECT_EQ(canonicalize("C:\\a\\..\\..\\b"), "C:\\b");
    EXPECT_EQ(canonicalize("a\\..\\..\\b"), "\\b");
    EXPECT_EQ(canonicalize("..\\x"), "x");
    EXPECT_EQ(canonicalize(""), "\\");
    EXPECT_EQ(canonicalize("C:"), "C:\\");
    EXPECT_EQ(canonicalize("C:\\a\\"), "C:\\a\\");
    EXPECT_EQ(canonicalize("C:\\a\\."), "C:\\a");
    EXPECT_EQ(canonicalize("C:\\a\\.."), "C:\\");
    EXPECT_EQ(canonicalize("C:/a/../b"), "C:/a/../b");
    EXPECT_EQ(canonicalize("C:\\a\\\\b"), "C:\\a\\\\b");
    EXPECT_EQ(canonicalize("\\\\srv\\sh\\.."), "\\\\srv");
    EXPECT_EQ(canonicalize("\\\\srv\\sh\\a\\..\\b"), "\\\\srv\\sh\\b");
    EXPECT_EQ(canonicalize("C:\\a...\\b"), "C:\\a...\\b");
    EXPECT_EQ(canonicalize("x:\\y\\z."), "x:\\y\\z");
    EXPECT_EQ(canonicalize("C:\\a\\b.\\c"), "C:\\a\\b.\\c");
    EXPECT_EQ(canonicalize("."), "\\");
    EXPECT_EQ(canonicalize("a\\b\\."), "a\\b");
}

TEST_F(ShlwapiS7Test, PathCanonicalizeWFailsOnMaxPathAndNull) {
    // 259 characters fit; 260 fail with ERROR_FILENAME_EXCED_RANGE and an
    // empty destination.
    std::string fits = "C:\\" + std::string(256, 'x');
    put(SRC, fits);
    EXPECT_EQ(call("PathCanonicalizeW", {DST, SRC}), 1u);
    EXPECT_EQ(get(DST), fits);
    const std::string too_long = "C:\\" + std::string(257, 'x');
    put(SRC, too_long);
    put(DST, "poison");
    EXPECT_EQ(call("PathCanonicalizeW", {DST, SRC}), 0u);
    EXPECT_EQ(get(DST), "");
    EXPECT_EQ(call("PathCanonicalizeW", {DST, 0}), 0u);
    ASSERT_NE(find("PathCanonicalizeA"), nullptr);
}

TEST_F(ShlwapiS7Test, PathAppendWMatchesWindows) {
    EXPECT_EQ(append("C:\\a", "b"), "C:\\a\\b");
    EXPECT_EQ(append("C:\\a\\", "b"), "C:\\a\\b");
    EXPECT_EQ(append("C:\\a", "\\b"), "C:\\a\\b");
    EXPECT_EQ(append("..\\p1\\p2", "p3"), "p1\\p2\\p3");
    EXPECT_EQ(append("C:\\a", "..\\b"), "C:\\b");
    EXPECT_EQ(append("C:\\a", "\\\\srv\\sh"), "\\\\srv\\sh");
    EXPECT_EQ(append("C:\\a", ""), "C:\\a");
    EXPECT_EQ(append("", "b"), "b");
    EXPECT_EQ(append("C:\\a", "C:\\z"), "C:\\z");
    EXPECT_EQ(append("C:\\a", "b/c"), "C:\\a\\b/c");
    // Overflow: FALSE and the path emptied.
    put(DST, "C:\\" + std::string(200, 'x'));
    put(SRC, std::string(100, 'y'));
    EXPECT_EQ(call("PathAppendW", {DST, SRC}), 0u);
    EXPECT_EQ(get(DST), "");
}

TEST_F(ShlwapiS7Test, PathMatchSpecWHandlesSpecLists) {
    EXPECT_TRUE(match("a.txt", "*.doc;*.txt"));
    EXPECT_TRUE(match("a.txt", " *.txt"));
    EXPECT_TRUE(match("a.txt", "*.doc; *.txt"));
    EXPECT_TRUE(match("abc", "*.*"));
    EXPECT_TRUE(match("a.TXT", "*.txt"));
    EXPECT_TRUE(match("C:\\d\\a.txt", "*.txt"));
    EXPECT_TRUE(match("", "*"));
    EXPECT_TRUE(match("a.txt", ""));
    EXPECT_TRUE(match("a", "a;"));
    EXPECT_TRUE(match("a.txt", ";*.txt"));
    EXPECT_FALSE(match("a.txt", "*.doc"));
}

TEST_F(ShlwapiS7Test, FindExtensionFileNameAndRemoveExtensionAgree) {
    struct Case { const char* path; u32 ext; u32 name; };
    const Case cases[] = {
        {"C:\\d\\a.txt", 6, 5}, {"C:\\d.x\\a", 8, 7}, {"a b.t xt", 8, 0},
        {"a.b c", 5, 0}, {"C:\\dir\\", 7, 3}, {"C:foo.bar", 5, 2},
        {"a/b.c", 3, 2}, {"a.b/c", 1, 4}, {".hidden", 0, 0}, {"a.", 1, 0},
        {"", 0, 0}, {"C:\\", 3, 0},
    };
    for (const auto& c : cases) {
        put(SRC, c.path);
        EXPECT_EQ(call("PathFindExtensionW", {SRC}), SRC + c.ext * 2) << c.path;
        EXPECT_EQ(call("PathFindFileNameW", {SRC}), SRC + c.name * 2) << c.path;
    }
    // ANSI twin follows the same separator rule.
    const char* ansi = "a.b c";
    for (usize i = 0; i <= std::strlen(ansi); ++i) {
        ASSERT_TRUE(memory.write_u8(DST + i, static_cast<u8>(ansi[i])).is_ok());
    }
    EXPECT_EQ(call("PathFindExtensionA", {DST}), DST + 5);
    const char* slash = "a.b/c";
    for (usize i = 0; i <= std::strlen(slash); ++i) {
        ASSERT_TRUE(memory.write_u8(DST + i, static_cast<u8>(slash[i])).is_ok());
    }
    EXPECT_EQ(call("PathFindExtensionA", {DST}), DST + 1);
}

TEST_F(ShlwapiS7Test, IsOSWow6432IsTrueOnlyForThirtyTwoBitGuests) {
    EXPECT_EQ(call("IsOS", {30u}), 1u);  // OS_WOW6432 in a 32-bit guest
    EXPECT_EQ(call("IsOS", {1u}), 1u);   // OS_NT
    EXPECT_EQ(call("IsOS", {29u}), 0u);  // OS_ANYSERVER
}

TEST(ShlwapiColorTest, RgbToHlsMatchesWindowsBothBitnesses) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
        "../../oracle_data/color_rgb_hls_probe.expected";
    for (bool mode64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x2800;
        Marshaler m(memory); m.attach_cpu(cpu);
        const auto exports = create_shlwapi_thunks();
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& e) { return e.name == "ColorRGBToHLS"; });
        ASSERT_NE(it, exports.entries.end());
        EXPECT_EQ(it->stdcall_bytes, 16u);
        std::ifstream input(path);
        ASSERT_TRUE(input.good());
        u32 color, hue, lum, sat, error;
        unsigned count = 0;
        while (input >> std::hex >> color >> std::dec >> hue >> lum >> sat >> std::hex >> error) {
            const u32 args[] = {color, 0x1000, 0x1002, 0x1004};
            if (mode64) {
                cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
                cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
            } else {
                for (u32 i = 0; i < 4; ++i)
                    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, args[i]).is_ok());
            }
            set_last_error(0x1234);
            it->handler(m);
            EXPECT_EQ(memory.read_u16(0x1000).value(), hue) << color;
            EXPECT_EQ(memory.read_u16(0x1002).value(), lum) << color;
            EXPECT_EQ(memory.read_u16(0x1004).value(), sat) << color;
            EXPECT_EQ(get_last_error(), error);
            ++count;
        }
        EXPECT_EQ(count, 264u);
    }
}

TEST(ShlwapiColorTest, HlsToRgbMatchesWindowsBothBitnesses) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
        "../../oracle_data/color_hls_rgb_probe.expected";
    for (bool mode64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x2800;
        Marshaler m(memory); m.attach_cpu(cpu);
        const auto exports = create_shlwapi_thunks();
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& e) { return e.name == "ColorHLSToRGB"; });
        ASSERT_NE(it, exports.entries.end());
        EXPECT_EQ(it->stdcall_bytes, 12u);
        std::ifstream input(path);
        ASSERT_TRUE(input.good());
        u32 color, hue, lum, sat, error;
        unsigned count = 0;
        while (input >> std::dec >> hue >> lum >> sat >> std::hex >> color >> error) {
            const u32 args[] = {hue, lum, sat};
            if (mode64) {
                cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1]; cpu.gpr.r8 = args[2];
            } else {
                for (u32 i = 0; i < 3; ++i)
                    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, args[i]).is_ok());
            }
            set_last_error(0x1234);
            it->handler(m);
            EXPECT_EQ(cpu.gpr.rax, color) << hue << "," << lum << "," << sat;
            EXPECT_EQ(get_last_error(), error);
            ++count;
        }
        EXPECT_EQ(count, 541u);
    }
}

// =============================================================================
// SHGetValue / SHSetValue / SHDeleteValue / SHDeleteEmptyKey (A and W)
//
// Expected values are Windows 11's, from
// tools/oracle_probe/shlwapi_regsec_shell_probe{,.x86}.expected (identical for
// x86 and x64); each check names the probe line it reproduces. Run in both
// calling conventions.
// =============================================================================

#include <winapi/kernel32/environment/environment.hpp>

class ShRegValueTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        thunks = std::make_unique<RegistryThunks>(registry, handles);
        thunks->set_environment_manager(&environment);
        environment.set_variable("SystemDrive", "C:");
        environment.set_variable("windir", "C:\\WINDOWS");
        exports = create_shlwapi_thunks(nullptr, nullptr, thunks.get());
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    }
    bool x64() const { return GetParam(); }

    const ExportEntry* find(std::string_view name) const {
        for (const auto& e : exports.entries)
            if (e.name == name) return &e;
        return nullptr;
    }
    GuestAddr wide(GuestAddr at, std::wstring_view text) {
        for (usize i = 0; i < text.size(); ++i)
            EXPECT_TRUE(memory.write_u16(at + i * 2, static_cast<u16>(text[i])).is_ok());
        EXPECT_TRUE(memory.write_u16(at + text.size() * 2, 0).is_ok());
        return at;
    }
    GuestAddr ansi(GuestAddr at, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i)
            EXPECT_TRUE(memory.write_u8(at + i, static_cast<u8>(text[i])).is_ok());
        EXPECT_TRUE(memory.write_u8(at + text.size(), 0).is_ok());
        return at;
    }
    u32 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = find(name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xFFFFFFFFu;
        EXPECT_EQ(entry->stdcall_bytes, args.size() * 4) << name;
        cpu = cpu::CpuState{};
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack;
        usize i = 0;
        for (u64 a : args) {
            if (x64()) {
                if (i == 0) cpu.gpr.rcx = a;
                else if (i == 1) cpu.gpr.rdx = a;
                else if (i == 2) cpu.gpr.r8 = a;
                else if (i == 3) cpu.gpr.r9 = a;
                else EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8, a).is_ok());
            } else {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + i * 4, static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    /// SHGetValue{W,A}(HKCU, KEY, value, &type, data, &cb) with cb = cap
    /// (data NULL when cap is ~0u: a size query).
    struct Got { u32 ret, type, cb; std::vector<u8> data; };
    Got get(bool w, GuestAddr value, u32 cap, bool with_type = true) {
        for (u32 j = 0; j < 128; ++j) (void)memory.write_u8(kData + j, 0xCC);
        (void)memory.write_u32(kType, 0xDEADu);
        (void)memory.write_u32(kSize, cap == ~0u ? 0u : cap);
        Got g{};
        g.ret = call(w ? "SHGetValueW" : "SHGetValueA",
                     {capsule::HKEY_CURRENT_USER, w ? kKeyW : kKeyA, value,
                      with_type ? kType : 0, cap == ~0u ? 0 : kData, kSize});
        g.type = memory.read_u32(kType).value_or(0);
        g.cb = memory.read_u32(kSize).value_or(0);
        for (u32 j = 0; j < 40; ++j) g.data.push_back(memory.read_u8(kData + j).value_or(0));
        return g;
    }
    static std::vector<u8> hex(std::string_view text) {
        std::vector<u8> out;
        for (usize i = 0; i + 1 < text.size(); i += 2)
            out.push_back(static_cast<u8>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
        return out;
    }
    static std::vector<u8> head(const Got& g, usize n) { return {g.data.begin(), g.data.begin() + n}; }
    void set_raw(const std::wstring& name, u32 type, std::vector<u8> bytes) {
        u64 key = 0;
        ASSERT_EQ(thunks->reg_create_key_ex_w(capsule::HKEY_CURRENT_USER,
                                              L"Software\\VitreoProbe\\shl", &key, nullptr), 0u);
        ASSERT_EQ(thunks->reg_set_value_ex_w(key, name, type, bytes.data(),
                                             static_cast<u32>(bytes.size())), 0u);
        (void)thunks->reg_close_key(key);
    }
    void prepare_keys() {
        wide(kKeyW, L"Software\\VitreoProbe\\shl");
        ansi(kKeyA, "Software\\VitreoProbe\\shl");
    }

    static constexpr GuestAddr kStack = 0x18000, kKeyW = 0x10000, kKeyA = 0x10200;
    static constexpr GuestAddr kName = 0x10400, kData = 0x10800, kType = 0x10C00,
                               kSize = 0x10C08, kBuf = 0x11000;
    capsule::Registry registry;
    HandleTable handles;
    GuestMemory memory;
    EnvironmentManager environment{"test.exe", memory};
    std::unique_ptr<RegistryThunks> thunks;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_P(ShRegValueTest, StringSizesAndTermination) {
    prepare_keys();
    // set_sz / set_sz_noterm: SHSetValueW creates the subkey.
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz"), 1,
                                   wide(kBuf, L"abc"), 8}), 0u);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"szn"), 1,
                                   wide(kBuf, L"abc"), 6}), 0u);
    const GuestAddr sz = wide(kName, L"sz");
    Got g = get(true, sz, 64);                         // get_sz
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 1u); EXPECT_EQ(g.cb, 8u);
    EXPECT_EQ(head(g, 10), hex("6100620063000000cccc"));
    g = get(true, sz, ~0u);                            // get_sz_sizeonly
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 10u); EXPECT_EQ(g.type, 1u);
    g = get(true, sz, 4);                              // get_sz_small
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 10u); EXPECT_EQ(g.type, 1u);
    EXPECT_EQ(head(g, 4), hex("cccccccc"));
    EXPECT_EQ(get(true, sz, 8).ret, 0u);               // get_sz_cap8
    g = get(true, sz, 7);                              // get_sz_cap7
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 10u);
    g = get(true, sz, 64, false);                      // get_notype
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 0xDEADu); EXPECT_EQ(g.cb, 8u);
    g = get(true, wide(kName, L"szn"), 64);            // get_sz_noterm
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 8u);
    EXPECT_EQ(head(g, 8), hex("6100620063000000"));

    // An odd-length REG_SZ (get_raw_odd / get_raw_odd_sizeonly).
    set_raw(L"raw_odd", 1, {0x77, 0, 0x78, 0, 0x79, 0, 0x7a});
    g = get(true, wide(kName, L"raw_odd"), 64);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 8u);
    EXPECT_EQ(head(g, 8), hex("7700780079000000"));
    EXPECT_EQ(get(true, wide(kName, L"raw_odd"), ~0u).cb, 9u);

    // The ANSI form converts and sizes in bytes (geta_sz, geta_sz_small,
    // geta_sz_sizeonly, set_a / geta_asz).
    g = get(false, ansi(kName, "sz"), 64);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 1u); EXPECT_EQ(g.cb, 4u);
    EXPECT_EQ(head(g, 5), hex("61626300cc"));
    g = get(false, ansi(kName, "sz"), 2);
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 5u);
    EXPECT_EQ(get(false, ansi(kName, "sz"), ~0u).cb, 5u);
    EXPECT_EQ(call("SHSetValueA", {capsule::HKEY_CURRENT_USER, kKeyA, ansi(kName, "asz"), 1,
                                   ansi(kBuf, "hi"), 3}), 0u);
    g = get(false, ansi(kName, "asz"), 64);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 3u); EXPECT_EQ(head(g, 3), hex("686900"));

    // pvData without pcbData; neither (get_data_nocb, get_sz_nocb).
    EXPECT_EQ(call("SHGetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz"), kType,
                                   kData, 0}), 87u);
    (void)memory.write_u32(kType, 0xDEADu);
    EXPECT_EQ(call("SHGetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz"), kType,
                                   0, 0}), 0u);
    EXPECT_EQ(memory.read_u32(kType).value_or(0), 1u);
}

TEST_P(ShRegValueTest, ExpandSzIsExpandedAndSizedAsTheLargerForm) {
    prepare_keys();
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"ex"), 2,
                                   wide(kBuf, L"%SystemDrive%\\x"), 32}), 0u);
    const GuestAddr ex = wide(kName, L"ex");
    Got g = get(true, ex, 128);                        // get_expand
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 1u); EXPECT_EQ(g.cb, 32u);
    EXPECT_EQ(head(g, 34), hex("43003a005c007800000065006d004400720069007600650025005c0078000000cccc"));
    g = get(true, ex, ~0u);                            // get_expand_sizeonly
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 1u); EXPECT_EQ(g.cb, 32u);
    g = get(true, ex, 8);                              // get_expand_small
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 32u); EXPECT_EQ(head(g, 8), hex("cccccccccccccccc"));
    g = get(false, ansi(kName, "ex"), 128);            // geta_expand
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 16u);
    EXPECT_EQ(head(g, 16), hex("433a5c7800656d4472697665255c7800"));
    EXPECT_EQ(get(false, ansi(kName, "ex"), ~0u).cb, 16u);  // geta_expand_sizeonly

    // An expansion longer than the stored string (get_grow*, geta_grow*).
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"grow"), 2,
                                   wide(kBuf, L"%windir%\\x"), 22}), 0u);
    const GuestAddr grow = wide(kName, L"grow");
    g = get(true, grow, 128);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 26u);
    EXPECT_EQ(head(g, 26), hex("43003a005c00570049004e0044004f00570053005c0078000000"));
    EXPECT_EQ(get(true, grow, ~0u).cb, 26u);
    g = get(true, grow, 22);
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 26u);
    g = get(false, ansi(kName, "grow"), 128);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 13u);
    EXPECT_EQ(head(g, 13), hex("433a5c57494e444f57535c7800"));
    EXPECT_EQ(get(false, ansi(kName, "grow"), ~0u).cb, 14u);
    g = get(false, ansi(kName, "grow"), 11);
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 14u);
}

TEST_P(ShRegValueTest, MultiBinaryDwordAndMissing) {
    prepare_keys();
    set_raw(L"multi", 7, hex("61000000620000000000"));
    Got g = get(true, wide(kName, L"multi"), 64);      // get_multi
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 7u); EXPECT_EQ(g.cb, 10u);
    EXPECT_EQ(get(true, wide(kName, L"multi"), ~0u).cb, 14u);   // get_multi_sizeonly
    g = get(true, wide(kName, L"multi"), 4);           // get_multi_small
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 14u);
    g = get(false, ansi(kName, "multi"), 64);          // geta_multi
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 5u); EXPECT_EQ(head(g, 5), hex("6100620000"));
    EXPECT_EQ(get(false, ansi(kName, "multi"), ~0u).cb, 7u);    // geta_multi_sizeonly

    const u32 dword = 0x12345678u;
    (void)memory.write_u32(kBuf, dword);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"dw"), 4,
                                   kBuf, 4}), 0u);
    g = get(true, wide(kName, L"dw"), 64);             // get_dword
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 4u); EXPECT_EQ(g.cb, 4u);
    EXPECT_EQ(head(g, 4), hex("78563412"));
    EXPECT_EQ(get(true, wide(kName, L"dw"), ~0u).cb, 4u);       // get_dword_sizeonly
    g = get(true, wide(kName, L"dw"), 2);              // get_dword_small
    EXPECT_EQ(g.ret, 234u); EXPECT_EQ(g.cb, 4u); EXPECT_EQ(head(g, 4), hex("cccccccc"));

    // set_null_data / set_null_data_cb / get_nul; set_badtype.
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"nul"), 3, 0, 0}), 0u);
    g = get(true, wide(kName, L"nul"), 64);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.type, 3u); EXPECT_EQ(g.cb, 0u);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"nul2"), 3, 0, 4}), 998u);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"bt"), 0x99,
                                   ansi(kBuf, "x"), 1}), 0u);

    // Default value through NULL and "" (set_default, get_default*).
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, 0, 1, wide(kBuf, L"d"), 4}), 0u);
    g = get(true, 0, 64);
    EXPECT_EQ(g.ret, 0u); EXPECT_EQ(g.cb, 4u); EXPECT_EQ(head(g, 4), hex("64000000"));
    EXPECT_EQ(get(true, wide(kName, L""), 64).cb, 4u);

    // get_missing_value writes REG_NONE; get_missing_key leaves the type.
    g = get(true, wide(kName, L"nope"), 64);
    EXPECT_EQ(g.ret, 2u); EXPECT_EQ(g.type, 0u); EXPECT_EQ(g.cb, 64u);
    wide(kBuf, L"Software\\VitreoProbe\\shl\\nope");
    (void)memory.write_u32(kType, 0xDEADu);
    (void)memory.write_u32(kSize, 64);
    EXPECT_EQ(call("SHGetValueW", {capsule::HKEY_CURRENT_USER, kBuf, wide(kName, L"sz"), kType,
                                   kData, kSize}), 2u);
    EXPECT_EQ(memory.read_u32(kType).value_or(0), 0xDEADu);
    // get_badhkey / set_badhkey.
    (void)memory.write_u32(kSize, 8);
    EXPECT_EQ(call("SHGetValueW", {0x1234, kKeyW, wide(kName, L"sz"), kType, kData, kSize}), 6u);
    EXPECT_EQ(memory.read_u32(kSize).value_or(0), 8u);
    EXPECT_EQ(call("SHSetValueW", {0x1234, kKeyW, wide(kName, L"x"), 1, wide(kBuf, L"a"), 4}), 6u);
}

TEST_P(ShRegValueTest, DeleteValueAndDeleteEmptyKey) {
    prepare_keys();
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz"), 1,
                                   wide(kBuf, L"abc"), 8}), 0u);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, kKeyW, 0, 1, wide(kBuf, L"d"), 4}), 0u);
    EXPECT_EQ(call("SHDeleteValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz")}), 0u);
    EXPECT_EQ(call("SHDeleteValueW", {capsule::HKEY_CURRENT_USER, kKeyW, wide(kName, L"sz")}), 2u);
    wide(kBuf, L"Software\\VitreoProbe\\shl\\nope");
    EXPECT_EQ(call("SHDeleteValueW", {capsule::HKEY_CURRENT_USER, kBuf, wide(kName, L"sz")}), 2u);
    EXPECT_EQ(call("SHDeleteValueW", {capsule::HKEY_CURRENT_USER, kKeyW, 0}), 0u);  // delval_default
    EXPECT_EQ(get(true, 0, 64).ret, 2u);

    auto create = [&](const std::wstring& path) {
        u64 key = 0;
        ASSERT_EQ(thunks->reg_create_key_ex_w(capsule::HKEY_CURRENT_USER, path, &key, nullptr), 0u);
        (void)thunks->reg_close_key(key);
    };
    const std::wstring base = L"Software\\VitreoProbe\\shl\\";
    create(base + L"e1");
    create(base + L"e2\\c");
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, wide(kBuf, base + L"e3"),
                                   wide(kName, L"v"), 1, wide(kData, L"a"), 4}), 0u);
    EXPECT_EQ(call("SHSetValueW", {capsule::HKEY_CURRENT_USER, wide(kBuf, base + L"e4"), 0, 1,
                                   wide(kData, L"a"), 4}), 0u);
    auto del_empty = [&](const std::wstring& sub) {
        return call("SHDeleteEmptyKeyW", {capsule::HKEY_CURRENT_USER, wide(kBuf, base + sub)});
    };
    EXPECT_EQ(del_empty(L"e1"), 0u);                   // delempty_empty
    EXPECT_EQ(del_empty(L"e1"), 2u);                   // delempty_gone
    EXPECT_EQ(del_empty(L"e2"), 145u);                 // delempty_subkey
    EXPECT_EQ(del_empty(L"e3"), 145u);                 // delempty_value
    EXPECT_EQ(del_empty(L"e4"), 145u);                 // delempty_default_value
    EXPECT_EQ(call("SHDeleteEmptyKeyA", {capsule::HKEY_CURRENT_USER,
                                         ansi(kBuf, "Software\\VitreoProbe\\shl\\e2\\c")}), 0u);
    EXPECT_EQ(del_empty(L"e2"), 0u);                   // delempty_after_child

    u64 e5 = 0;
    ASSERT_EQ(thunks->reg_create_key_ex_w(capsule::HKEY_CURRENT_USER, base + L"e5", &e5, nullptr), 0u);
    EXPECT_EQ(call("SHDeleteEmptyKeyW", {e5, 0}), 87u);              // delempty_null_sub
    EXPECT_EQ(call("SHDeleteEmptyKeyW", {e5, wide(kBuf, L"")}), 0u); // delempty_empty_sub
    (void)thunks->reg_close_key(e5);
    u64 probe = 0;
    EXPECT_EQ(thunks->reg_open_key_ex_w(capsule::HKEY_CURRENT_USER, base + L"e5", 0, 0, &probe), 2u);
    EXPECT_EQ(call("SHDeleteEmptyKeyW", {0x1234, wide(kBuf, L"x")}), 6u);  // delempty_badhkey
}

INSTANTIATE_TEST_SUITE_P(Bitness, ShRegValueTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// =============================================================================
// PathQuoteSpaces{A,W} / PathUnquoteSpaces{A,W} against Windows:
// tools/oracle_probe/shlwapi_quote_spaces_probe{,.x86}.expected. The probe's
// cases are replayed through the exports and each line is rendered exactly as
// the probe prints it (minus the last error, which neither function touches).
// =============================================================================

class QuoteSpacesTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        exports = create_shlwapi_thunks();
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    }
    bool x64() const { return GetParam(); }

    u32 call(const char* name, u64 arg) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries)
            if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xFFFFFFFFu;
        EXPECT_EQ(entry->stdcall_bytes, 4u) << name;
        cpu = cpu::CpuState{};
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack;
        if (x64()) cpu.gpr.rcx = arg;
        else EXPECT_TRUE(memory.write_u32(kStack + 4, static_cast<u32>(arg)).is_ok());
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// One probe line: run_w / run_a, then compress_print without err=.
    std::string run(bool wide, bool quote, const std::string& label, const std::string& in) {
        const u32 unit = wide ? 2 : 1;
        for (u32 i = 0; i < kCap; ++i)
            EXPECT_TRUE((wide ? memory.write_u16(kBuf + i * 2, '#') : memory.write_u8(kBuf + i, '#')).is_ok());
        for (usize i = 0; i <= in.size(); ++i) {
            // The probe widens with MultiByteToWideChar(CP_ACP = 1252): only
            // 0xA0 is outside ASCII, and it maps to U+00A0.
            const u16 c = i < in.size() ? static_cast<u8>(in[i]) : 0;
            EXPECT_TRUE((wide ? memory.write_u16(kBuf + i * 2, c) : memory.write_u8(kBuf + i, static_cast<u8>(c))).is_ok());
        }
        const char* name = quote ? (wide ? "PathQuoteSpacesW" : "PathQuoteSpacesA")
                                 : (wide ? "PathUnquoteSpacesW" : "PathUnquoteSpacesA");
        const u32 r = call(name, kBuf);
        auto unit_at = [&](usize i) -> u16 {
            return wide ? memory.read_u16(kBuf + i * unit).value_or(0xFFFF) : memory.read_u8(kBuf + i).value_or(0xFF);
        };
        auto dump = [&](usize from, usize n) {
            std::string out = "[";
            char hex[16];
            for (usize i = from; i < from + n; ++i) {
                const u16 c = unit_at(i);
                if (c == 0) out += "\\0";
                else if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
                else {
                    std::snprintf(hex, sizeof hex, wide ? "\\u%04x" : "\\x%02x", c);
                    out += hex;
                }
            }
            return out + "]";
        };
        char head[96];
        std::snprintf(head, sizeof head, "%s %-24s ret=%u buf=", quote ? (wide ? "QW" : "QA") : (wide ? "UW" : "UA"),
                      label.c_str(), r);
        const usize len = in.size();
        return head + (len > 40 ? dump(0, 3) + "...(x run)..." + dump(len - 3, 7) : dump(0, len + 4));
    }

    static std::string boundary_input(bool quote, int len, int space_at) {
        std::string s(static_cast<usize>(len), 'x');
        if (space_at >= 0) s[static_cast<usize>(space_at)] = ' ';
        if (!quote) s.front() = s.back() = '"';
        return s;
    }

    /// The probe's shlwapi.dll section, "err=57005 " removed (every line
    /// reports the 0xDEAD the probe set: the last error is untouched).
    std::vector<std::string> expected_lines() const {
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
                          "tests" / "oracle_data" /
                          (x64() ? "shlwapi_quote_spaces_probe.expected" : "shlwapi_quote_spaces_probe.x86.expected");
        std::ifstream in(path);
        std::vector<std::string> lines;
        bool section = false;
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("bits=", 0) == 0) {
                EXPECT_EQ(line, x64() ? "bits=64" : "bits=32");
                continue;
            }
            if (line.rfind("== ", 0) == 0) {
                section = line == "== shlwapi.dll";
                continue;
            }
            if (!section) continue;
            const auto err = line.find("err=57005");
            EXPECT_NE(err, std::string::npos) << line;
            if (err != std::string::npos) line.erase(err, std::string("err=57005").size() + (err + 9 < line.size() ? 1 : 0));
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lines.push_back(line);
        }
        return lines;
    }

    static constexpr GuestAddr kBuf = 0x11000;
    static constexpr GuestAddr kStack = 0x1F000;
    static constexpr u32 kCap = 600;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_P(QuoteSpacesTest, EveryProbeCaseMatchesWindows) {
    std::vector<std::string> got;
    for (const char* name : {"QW", "QA", "UW", "UA"}) {
        const bool quote = name[0] == 'Q', wide = name[1] == 'W';
        const std::string fn = std::string(quote ? "PathQuoteSpaces" : "PathUnquoteSpaces") + (wide ? "W" : "A");
        char line[64];
        std::snprintf(line, sizeof line, "%s %-24s ret=%u", name, "NULL", call(fn.c_str(), 0));
        got.push_back(line);
    }
    auto both = [&](bool quote, const std::string& label, const std::string& in) {
        got.push_back(run(true, quote, label, in));
        got.push_back(run(false, quote, label, in));
    };
    const std::vector<std::pair<std::string, std::string>> q = {
        {"empty", ""}, {"nospace", "C:\\dir\\file"}, {"space", "C:\\a b\\c"}, {"onlyspace", " "},
        {"leading", " a"}, {"trailing", "a "}, {"tab", "a\tb"}, {"quoted", "\"a b\""},
        {"quoted-nospace", "\"ab\""}, {"half-quoted", "\"a b"}, {"inner-quote", "a \"b\" c"},
        {"twospaces", "a  b"}, {"nbsp-ansi", "a\xa0" "b"},
    };
    for (const auto& [label, in] : q) both(true, label, in);
    for (int len = 254; len <= 262; ++len) both(true, "len" + std::to_string(len), boundary_input(true, len, 5));
    both(true, "len300", boundary_input(true, 300, 5));
    both(true, "len259", boundary_input(true, 259, -1));
    const std::vector<std::pair<std::string, std::string>> u = {
        {"empty", ""}, {"noquote", "abc"}, {"one-quote", "\""}, {"two-quotes", "\"\""},
        {"quoted", "\"a b\""}, {"quoted-nospace", "\"abc\""}, {"open-only", "\"abc"}, {"close-only", "abc\""},
        {"inner", "\"a\"b\""}, {"lead-space", " \"a\""}, {"trail-space", "\"a\" "}, {"triple", "\"\"\""},
        {"double-wrapped", "\"\"a\"\""},
    };
    for (const auto& [label, in] : u) both(false, label, in);
    for (int len = 258; len <= 262; ++len) both(false, "len" + std::to_string(len), boundary_input(false, len, -1));
    both(false, "len300", boundary_input(false, 300, -1));
    EXPECT_EQ(got, expected_lines());
}

INSTANTIATE_TEST_SUITE_P(Bitness, QuoteSpacesTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// =============================================================================
// StrCmpIW / StrCmpN{,I}{A,W} (Learn shlwapi StrCmpIW, StrCmpNW, StrCmpNIW,
// StrCmpNA, StrCmpNIA): zero when equal, negative when psz1 sorts first,
// positive when after; the I forms ignore case; nChar limits the compare.
// =============================================================================

class StrCmpFamilyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(STACK + 0x800);
        set_shlwapi_locale_strings(&strings);
        exports = create_shlwapi_thunks();
    }
    void TearDown() override { set_shlwapi_locale_strings(nullptr); }

    void put(GuestAddr at, const std::string& text, bool wide) {
        for (usize i = 0; i <= text.size(); ++i) {
            const u8 c = i < text.size() ? static_cast<u8>(text[i]) : 0;
            if (wide) ASSERT_TRUE(memory.write_u16(at + static_cast<u32>(i) * 2, c).is_ok());
            else ASSERT_TRUE(memory.write_u8(at + static_cast<u32>(i), c).is_ok());
        }
    }
    i32 cmp(const char* name, const std::string& a, const std::string& b, std::optional<i32> n = std::nullopt) {
        const bool wide = std::string(name).back() == 'W';
        put(A, a, wide);
        put(B, b, wide);
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries)
            if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 99;
        EXPECT_EQ(entry->stdcall_bytes, n ? 12u : 8u) << name;
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4), A);
        (void)memory.write_u32(static_cast<u32>(cpu.sp() + 8), B);
        if (n) (void)memory.write_u32(static_cast<u32>(cpu.sp() + 12), static_cast<u32>(*n));
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<i32>(static_cast<u32>(cpu.gpr.rax));
    }

    static constexpr GuestAddr BASE = 0x2000;
    static constexpr GuestAddr A = 0x2000;
    static constexpr GuestAddr B = 0x3000;
    static constexpr GuestAddr STACK = 0x80000;
    LocaleStringManager strings;
    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_F(StrCmpFamilyTest, StrCmpIWIgnoresCase) {
    EXPECT_EQ(cmp("StrCmpIW", "ISymWrapper.dll", "isymwrapper.DLL"), 0);
    EXPECT_LT(cmp("StrCmpIW", "abc", "ABD"), 0);
    EXPECT_GT(cmp("StrCmpIW", "abd", "ABC"), 0);
    EXPECT_LT(cmp("StrCmpIW", "ab", "abc"), 0);
}

TEST_F(StrCmpFamilyTest, StrCmpNComparesAtMostNChars) {
    for (const char* name : {"StrCmpNW", "StrCmpNA"}) {
        EXPECT_EQ(cmp(name, "abcX", "abcY", 3), 0) << name;
        EXPECT_LT(cmp(name, "abcX", "abcY", 4), 0) << name;
        EXPECT_NE(cmp(name, "abc", "ABC", 3), 0) << name;  // case-sensitive
        EXPECT_EQ(cmp(name, "a", "b", 0), 0) << name;      // nothing compared
        EXPECT_LT(cmp(name, "ab", "abc", 5), 0) << name;   // shorter string first
    }
    for (const char* name : {"StrCmpNIW", "StrCmpNIA"}) {
        EXPECT_EQ(cmp(name, "ABCx", "abcY", 3), 0) << name;
        EXPECT_LT(cmp(name, "ABCx", "abcY", 4), 0) << name;
    }
}

TEST_F(PathCombineATest, PublishesDirectoryEmptyFamily) {
    for(const char* name:{"PathIsDirectoryEmptyA","PathIsDirectoryEmptyW"}) {
        const auto* entry=find(name);
        ASSERT_NE(entry,nullptr);
        EXPECT_EQ(entry->stdcall_bytes,4u);
    }
}

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <winapi/kernel32/locale/codepage_convert.hpp>
#include "directory_empty_oracle.inc"
TEST(DirectoryEmptyContractTest, MatchesWindowsBothEncodingsAndBitnesses) {
    char temporary[]="/tmp/vitreo-directory-empty-XXXXXX";
    const char* root=mkdtemp(temporary); ASSERT_NE(root,nullptr);
    const std::filesystem::path host_root(root);
    for(const char* dir:{"empty","nonempty","subdir/child","hidden", "caf\xc3\xa9"})
        std::filesystem::create_directories(host_root/dir);
    for(const char* file:{"file","nonempty/file","hidden/file"}) std::ofstream(host_root/file)<<"x";
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C',std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    const auto exports=create_shlwapi_thunks(&vfs);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x3000,0x4000,MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    for(const auto& expected:directory_empty_cases) {
        SCOPED_TRACE(::testing::Message()<<expected.name<<" wide="<<expected.wide<<" x64="<<expected.x64);
        if(expected.x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp=0x6800;
        Marshaler m(memory);m.attach_cpu(cpu);
        const ExportEntry* entry=nullptr;
        for(const auto& candidate:exports.entries)
            if(candidate.name==(expected.wide?"PathIsDirectoryEmptyW":"PathIsDirectoryEmptyA"))entry=&candidate;
        ASSERT_NE(entry,nullptr);
        codepage::set_ansi_code_page(expected.utf8 ? codepage::kCpUtf8 : codepage::kAnsiCodePage);
        std::string name=expected.name,path=name;
        if(name=="extended-empty")path="\\\\?\\C:\\empty";
        if(name=="absent-drive")path="Z:";
        if(name.starts_with("multibyte")) {
            path.clear();
            for(usize i=0;i<std::stoul(name.substr(9));++i)path+=expected.wide||expected.utf8?"\xc3\xa9":"\xe9";
        }
        if(name=="dot")path=".";
        if(name=="empty-dot")path="empty\\.";
        if(name=="empty-parent")path="empty\\..";
        if(name=="missing-parent")path="missing\\child";
        if(name=="trailing-slash")path="empty\\";
        if(name=="forward-slash")path="empty/";
        if(name=="space")path="empty ";
        if(name=="period")path="empty.";
        if(name=="wildcard")path="empt*";
        if(name=="wildcard-child")path="empty\\*";
        if(name=="empty-string")path="";
        if(name=="unicode")path=expected.wide||expected.utf8?"caf\xc3\xa9":"caf\xe9";
        if(name=="drive"||name=="drive-in-empty")path="C:";
        if(name=="drive-dot-in-empty")path="C:.";
        if(name.starts_with("length"))path=std::string(std::stoul(name.substr(6)),'a');
        if(name.starts_with("separator")){path=std::string(std::stoul(name.substr(9)),'a');path.back()='\\';}
        vfs.set_cwd(name=="drive-in-empty"||name=="drive-dot-in-empty"?"C:\\empty":"C:\\");
        GuestAddr pointer=0x3000;
        if(name=="null")pointer=0;
        else if(name=="bad-pointer")pointer=1;
        else if(expected.wide)ASSERT_TRUE(m.write_wide_string(pointer,path).is_ok());
        else ASSERT_TRUE(memory.write_bytes(pointer,reinterpret_cast<const u8*>(path.c_str()),path.size()+1).is_ok());
        if(expected.x64)cpu.gpr.rcx=pointer;
        else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp+4,static_cast<u32>(pointer)).is_ok());
        set_last_error(0x12345678);cpu.pending_raise.active=false;
        entry->handler(m);
        EXPECT_EQ(cpu.gpr.rax,expected.result);
        EXPECT_EQ(get_last_error(),expected.last);
        EXPECT_EQ(cpu.pending_raise.active,expected.fault);
    }
    codepage::set_ansi_code_page(codepage::kAnsiCodePage);
    std::filesystem::remove_all(host_root);
}

TEST(DirectoryEmptyContractTest, CountsGeneratedGuestFilesAndReportsEnumerationErrors) {
    class DirectoryBackend : public capsule::HostFileSystem {
    public:
        using HostFileSystem::HostFileSystem;
        Result<std::vector<capsule::DirectoryEntry>,capsule::VfsError> list_dir(std::string_view path) override {
            if(path=="denied")return Result<std::vector<capsule::DirectoryEntry>,capsule::VfsError>::err(capsule::VfsError::AccessDenied);
            if(path=="dots")return Result<std::vector<capsule::DirectoryEntry>,capsule::VfsError>::ok({{".",{}},{"..",{}}});
            return HostFileSystem::list_dir(path);
        }
    };
    char temporary[]="/tmp/vitreo-directory-generated-XXXXXX";
    const char* root=mkdtemp(temporary);ASSERT_NE(root,nullptr);
    std::filesystem::create_directory(std::filesystem::path(root)/"generated");
    std::filesystem::create_directories(std::filesystem::path(root)/"server/share/empty");
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C',std::make_unique<DirectoryBackend>(root)).is_ok());
    const auto exports=create_shlwapi_thunks(&vfs);
    const auto generated=std::make_shared<const std::vector<u8>>(std::initializer_list<u8>{1,2,3});
    ASSERT_TRUE(vfs.publish_generated_file("C:\\generated\\guest.dll",generated).is_ok());
    GuestMemory memory;ASSERT_TRUE(memory.map_region(0x3000,0x2000,MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    for(bool x64:{false,true})for(bool wide:{false,true}) {
        if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x4800;Marshaler m(memory);m.attach_cpu(cpu);
        const ExportEntry* entry=nullptr;
        for(const auto& candidate:exports.entries)
            if(candidate.name==(wide?"PathIsDirectoryEmptyW":"PathIsDirectoryEmptyA"))entry=&candidate;
        ASSERT_NE(entry,nullptr);
        for(const char* path:{"C:\\generated","C:\\dots","C:\\denied","\\\\server\\share\\empty","\\\\?\\UNC\\server\\share\\empty","\\\\.\\pipe\\name"}) {
            if(wide)ASSERT_TRUE(m.write_wide_string(0x3000,path).is_ok());
            else ASSERT_TRUE(memory.write_bytes(0x3000,reinterpret_cast<const u8*>(path),std::strlen(path)+1).is_ok());
            if(x64)cpu.gpr.rcx=0x3000;else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp+4,0x3000).is_ok());
            set_last_error(0x12345678);entry->handler(m);
            if(std::string_view(path).starts_with("\\\\")) {EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),120u);}
            else if(std::string_view(path)=="C:\\dots") {EXPECT_EQ(cpu.gpr.rax,1u);EXPECT_EQ(get_last_error(),18u);}
            else if(std::string_view(path)=="C:\\denied") {EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),5u);}
            else {EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),0x12345678u);}
        }
    }
    std::filesystem::remove_all(root);
}

TEST_F(PathCombineATest, PublishesRootFamily) {
    for(const char* name:{"PathIsRootA","PathIsRootW"}) {
        const auto* entry=find(name);ASSERT_NE(entry,nullptr);EXPECT_EQ(entry->stdcall_bytes,4u);
    }
}

#include "path_is_root_oracle.inc"
TEST(PathRootContractTest, EveryWindowsPathCaseMatchesBothGuestCallingConventions) {
    GuestMemory memory;ASSERT_TRUE(memory.map_region(0x10000,0x20000,MemoryProtection::ReadWrite).is_ok());
    const auto exports=create_shlwapi_thunks();
    for(const auto& expected:root_cases) {
        codepage::set_ansi_code_page(expected.utf8 ? codepage::kCpUtf8 : codepage::kAnsiCodePage);
        SCOPED_TRACE(::testing::Message()<<"case="<<expected.id<<" wide="<<expected.wide<<" x64="<<expected.x64);
        cpu::CpuState cpu;if(expected.x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x28000;Marshaler m(memory);m.attach_cpu(cpu);
        const ExportEntry* entry=nullptr;
        for(const auto& candidate:exports.entries)if(candidate.name==(expected.wide?"PathIsRootW":"PathIsRootA"))entry=&candidate;
        ASSERT_NE(entry,nullptr);EXPECT_EQ(entry->stdcall_bytes,4u);
        const GuestAddr pointer=expected.pointer==-1?0:expected.pointer==-2?1:0x10000;
        std::vector<u8> before;
        if(expected.pointer==0) {
            for(u16 unit:expected.units){before.push_back(static_cast<u8>(unit));if(expected.wide)before.push_back(static_cast<u8>(unit>>8));}
            before.push_back(0);if(expected.wide)before.push_back(0);
            ASSERT_TRUE(memory.write_bytes(pointer,before.data(),before.size()).is_ok());
        }
        if(expected.x64)cpu.gpr.rcx=pointer;
        else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp+4,static_cast<u32>(pointer)).is_ok());
        set_last_error(0x12345678);entry->handler(m);
        EXPECT_EQ(cpu.gpr.rax,expected.result);EXPECT_EQ(get_last_error(),expected.last);
        EXPECT_EQ(cpu.pending_raise.active,expected.fault);
        if(expected.pointer==0){std::vector<u8> after(before.size());ASSERT_TRUE(memory.read_bytes(pointer,after.data(),after.size()).is_ok());EXPECT_EQ(after,before);}
    }
    codepage::set_ansi_code_page(codepage::kAnsiCodePage);
}

TEST(PathRootContractTest, ExhaustiveWindowsDriveCharacterClassification) {
    GuestMemory memory;ASSERT_TRUE(memory.map_region(0x10000,0x20000,MemoryProtection::ReadWrite).is_ok());
    const auto exports=create_shlwapi_thunks();
    for(bool wide:{false,true})for(bool x64:{false,true}) {
        cpu::CpuState cpu;if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x28000;Marshaler m(memory);m.attach_cpu(cpu);
        const ExportEntry* entry=nullptr;
        for(const auto& candidate:exports.entries)if(candidate.name==(wide?"PathIsRootW":"PathIsRootA"))entry=&candidate;
        ASSERT_NE(entry,nullptr);
        if(x64)cpu.gpr.rcx=0x10000;else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp+4,0x10000).is_ok());
        const auto mask=wide?root_drive_mask_W:root_drive_mask_A;
        const auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
        for(u32 unit=0;unit<(wide?65536u:256u);++unit) {
            if(wide){const u16 path[]={static_cast<u16>(unit),':','\\',0};ASSERT_TRUE(memory.write_bytes(0x10000,reinterpret_cast<const u8*>(path),sizeof(path)).is_ok());}
            else{const u8 path[]={static_cast<u8>(unit),':','\\',0};ASSERT_TRUE(memory.write_bytes(0x10000,path,sizeof(path)).is_ok());}
            const unsigned byte=static_cast<unsigned>((nibble(mask[(unit/8)*2])<<4)|nibble(mask[(unit/8)*2+1]));
            entry->handler(m);
            ASSERT_EQ(cpu.gpr.rax,(byte>>(unit%8))&1u)<<"unit="<<unit<<" wide="<<wide<<" x64="<<x64;
        }
    }
}
