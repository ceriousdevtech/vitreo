/// @file test_paths.cpp
/// @brief Unit tests for WinPath and PathTranslator

#include <gtest/gtest.h>

#include "capsule/vfs/win_path.hpp"
#include "capsule/vfs/path_translator.hpp"

using namespace vitreo::capsule;

// =============================================================================
// WinPath Construction Tests
// =============================================================================

TEST(WinPathTest, ConstructEmpty) {
    WinPath path;
    EXPECT_TRUE(path.is_valid());
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(path.drive_letter(), 'C');
}

TEST(WinPathTest, ConstructFromAbsoluteWithBackslash) {
    WinPath path("C:\\Users\\file.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'C');
    EXPECT_EQ(path.components().size(), 2);
    EXPECT_EQ(path.components()[0], "Users");
    EXPECT_EQ(path.components()[1], "file.txt");
}

TEST(WinPathTest, ConstructFromAbsoluteWithForwardSlash) {
    WinPath path("C:/Users/file.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'C');
    EXPECT_EQ(path.components().size(), 2);
}

TEST(WinPathTest, ConstructFromLowercaseDrive) {
    WinPath path("d:\\data\\file.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'D');
}

TEST(WinPathTest, ConstructFromDriveOnly) {
    WinPath path("C:");
    EXPECT_TRUE(path.is_valid());
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(path.drive_letter(), 'C');
}

TEST(WinPathTest, ConstructFromDriveWithSlash) {
    WinPath path("C:\\");
    EXPECT_TRUE(path.is_valid());
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(path.drive_letter(), 'C');
}

TEST(WinPathTest, ConstructFromAbsoluteNoDrive) {
    WinPath path("\\Users\\file.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'C');  // Default drive
    EXPECT_EQ(path.components().size(), 2);
}

TEST(WinPathTest, ConstructFromRelative) {
    WinPath path("subdir\\file.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'C');  // Default drive
    EXPECT_EQ(path.components().size(), 2);
}

TEST(WinPathTest, ConstructFromEmptyString) {
    WinPath path("");
    EXPECT_TRUE(path.is_valid());
    EXPECT_TRUE(path.empty());
}

TEST(WinPathTest, UncPathIsInvalid) {
    WinPath path("\\\\server\\share\\file.txt");
    EXPECT_FALSE(path.is_valid());
}

TEST(WinPathTest, UncPathWithForwardSlashIsInvalid) {
    WinPath path("//server/share/file.txt");
    EXPECT_FALSE(path.is_valid());
}

TEST(WinPathTest, ExtendedLengthPrefixIsStripped) {
    // "\\?\C:\foo" is the Win32 extended-length form of "C:\foo"
    // (Chromium opens its log files this way).
    WinPath path("\\\\?\\C:\\Program Files (x86)\\Steam\\logs\\cef_log.txt");
    EXPECT_TRUE(path.is_valid());
    EXPECT_EQ(path.drive_letter(), 'C');
    EXPECT_EQ(path.filename(), "cef_log.txt");
    EXPECT_EQ(path.to_windows_string(),
              WinPath("C:\\Program Files (x86)\\Steam\\logs\\cef_log.txt").to_windows_string());
}

TEST(WinPathTest, ExtendedLengthUncStillInvalid) {
    WinPath path("\\\\?\\UNC\\server\\share\\file.txt");
    EXPECT_FALSE(path.is_valid());
}

TEST(WinPathTest, IsUncPath) {
    EXPECT_TRUE(WinPath::is_unc_path("\\\\server\\share"));
    EXPECT_TRUE(WinPath::is_unc_path("//server/share"));
    EXPECT_FALSE(WinPath::is_unc_path("C:\\Users"));
    EXPECT_FALSE(WinPath::is_unc_path("\\Users"));
    EXPECT_FALSE(WinPath::is_unc_path("/Users"));
}

// =============================================================================
// WinPath Components Tests
// =============================================================================

TEST(WinPathTest, Filename) {
    WinPath path("C:\\Users\\John\\file.txt");
    EXPECT_EQ(path.filename(), "file.txt");
}

TEST(WinPathTest, FilenameEmpty) {
    WinPath path("C:\\");
    EXPECT_TRUE(path.filename().empty());
}

TEST(WinPathTest, Extension) {
    WinPath path("C:\\Users\\file.txt");
    EXPECT_EQ(path.extension(), "txt");
}

TEST(WinPathTest, ExtensionMultipleDots) {
    WinPath path("C:\\Users\\file.tar.gz");
    EXPECT_EQ(path.extension(), "gz");
}

TEST(WinPathTest, ExtensionNone) {
    WinPath path("C:\\Users\\README");
    EXPECT_TRUE(path.extension().empty());
}

TEST(WinPathTest, ExtensionHiddenFile) {
    // Files starting with dot and no other dot have no extension
    // (dot at position 0 is not treated as an extension separator)
    WinPath path("C:\\Users\\.gitignore");
    EXPECT_TRUE(path.extension().empty());
}

TEST(WinPathTest, Stem) {
    WinPath path("C:\\Users\\file.txt");
    EXPECT_EQ(path.stem(), "file");
}

TEST(WinPathTest, StemNoExtension) {
    WinPath path("C:\\Users\\README");
    EXPECT_EQ(path.stem(), "README");
}

TEST(WinPathTest, Directory) {
    WinPath path("C:\\Users\\John\\file.txt");
    EXPECT_EQ(path.directory(), "Users/John");
}

TEST(WinPathTest, DirectorySingleComponent) {
    WinPath path("C:\\file.txt");
    EXPECT_EQ(path.directory(), "");
}

TEST(WinPathTest, DirectoryNone) {
    WinPath path("C:\\");
    EXPECT_EQ(path.directory(), "");
}

// =============================================================================
// WinPath Normalization Tests
// =============================================================================

TEST(WinPathTest, NormalizeCurrentDir) {
    WinPath path("C:\\Users\\.\\file.txt");
    auto normalized = path.normalize();
    EXPECT_EQ(normalized.components().size(), 2);
    EXPECT_EQ(normalized.components()[0], "Users");
    EXPECT_EQ(normalized.components()[1], "file.txt");
}

TEST(WinPathTest, NormalizeParentDir) {
    WinPath path("C:\\Users\\John\\..\\file.txt");
    auto normalized = path.normalize();
    EXPECT_EQ(normalized.components().size(), 2);
    EXPECT_EQ(normalized.components()[0], "Users");
    EXPECT_EQ(normalized.components()[1], "file.txt");
}

TEST(WinPathTest, NormalizeMultipleParent) {
    WinPath path("C:\\Users\\John\\Docs\\..\\..\\file.txt");
    auto normalized = path.normalize();
    EXPECT_EQ(normalized.components().size(), 2);
    EXPECT_EQ(normalized.components()[0], "Users");
    EXPECT_EQ(normalized.components()[1], "file.txt");
}

TEST(WinPathTest, NormalizeAtRoot) {
    WinPath path("C:\\..\\file.txt");
    auto normalized = path.normalize();
    EXPECT_EQ(normalized.components().size(), 1);
    EXPECT_EQ(normalized.components()[0], "file.txt");
}

TEST(WinPathTest, NormalizeMixed) {
    WinPath path("C:\\Users\\.\\John\\..\\Docs\\.\\file.txt");
    auto normalized = path.normalize();
    EXPECT_EQ(normalized.components().size(), 3);
    EXPECT_EQ(normalized.components()[0], "Users");
    EXPECT_EQ(normalized.components()[1], "Docs");
    EXPECT_EQ(normalized.components()[2], "file.txt");
}

// =============================================================================
// WinPath Operations Tests
// =============================================================================

TEST(WinPathTest, Parent) {
    WinPath path("C:\\Users\\John\\file.txt");
    auto parent = path.parent();
    EXPECT_EQ(parent.drive_letter(), 'C');
    EXPECT_EQ(parent.components().size(), 2);
    EXPECT_EQ(parent.components()[0], "Users");
    EXPECT_EQ(parent.components()[1], "John");
}

TEST(WinPathTest, ParentOfRoot) {
    WinPath path("C:\\");
    auto parent = path.parent();
    EXPECT_TRUE(parent.empty());
    EXPECT_EQ(parent.drive_letter(), 'C');
}

TEST(WinPathTest, Join) {
    WinPath path("C:\\Users");
    auto joined = path.join("file.txt");
    EXPECT_EQ(joined.components().size(), 2);
    EXPECT_EQ(joined.components()[0], "Users");
    EXPECT_EQ(joined.components()[1], "file.txt");
}

TEST(WinPathTest, JoinMultipleComponents) {
    WinPath path("C:\\");
    auto joined = path.join("Users\\John\\file.txt");
    EXPECT_EQ(joined.components().size(), 3);
}

// =============================================================================
// WinPath Conversion Tests
// =============================================================================

TEST(WinPathTest, ToWindowsString) {
    WinPath path("C:/Users/file.txt");
    EXPECT_EQ(path.to_windows_string(), "C:\\Users\\file.txt");
}

TEST(WinPathTest, ToWindowsStringRoot) {
    WinPath path("C:\\");
    EXPECT_EQ(path.to_windows_string(), "C:\\");
}

TEST(WinPathTest, ToUnixString) {
    WinPath path("C:\\Users\\file.txt");
    EXPECT_EQ(path.to_unix_string(), "c/Users/file.txt");
}

TEST(WinPathTest, ToUnixStringRoot) {
    WinPath path("C:\\");
    EXPECT_EQ(path.to_unix_string(), "c");
}

TEST(WinPathTest, ToUnixStringDifferentDrive) {
    WinPath path("D:\\Data\\file.txt");
    EXPECT_EQ(path.to_unix_string(), "d/Data/file.txt");
}

// =============================================================================
// WinPath Comparison Tests
// =============================================================================

TEST(WinPathTest, EqualsSame) {
    WinPath a("C:\\Users\\file.txt");
    WinPath b("C:\\Users\\file.txt");
    EXPECT_TRUE(a.equals(b));
    EXPECT_TRUE(a == b);
}

TEST(WinPathTest, EqualsCaseInsensitive) {
    WinPath a("C:\\Users\\FILE.TXT");
    WinPath b("c:\\users\\file.txt");
    EXPECT_TRUE(a.equals(b));
}

TEST(WinPathTest, EqualsDifferentSlashes) {
    WinPath a("C:\\Users\\file.txt");
    WinPath b("C:/Users/file.txt");
    EXPECT_TRUE(a.equals(b));
}

TEST(WinPathTest, MakeAbsoluteResolvesRelativeExecutableAgainstCurrentDirectory) {
    EXPECT_EQ(
        make_absolute_win_path(
            "StarEdit.exe", "C:\\Program Files (x86)\\Starcraft"),
        "C:\\Program Files (x86)\\Starcraft\\StarEdit.exe");
}

TEST(WinPathTest, MakeAbsolutePreservesAndNormalizesDriveQualifiedPath) {
    EXPECT_EQ(
        make_absolute_win_path(
            "D:\\Games\\Starcraft\\.\\StarCraft.exe", "C:\\Ignored"),
        "D:\\Games\\Starcraft\\StarCraft.exe");
}

TEST(WinPathTest, MakeAbsoluteUsesCurrentDriveForRootRelativePath) {
    EXPECT_EQ(
        make_absolute_win_path("\\Windows\\System32\\helper.exe", "D:\\Games"),
        "D:\\Windows\\System32\\helper.exe");
}

TEST(WinPathTest, NotEqualsDifferentPath) {
    WinPath a("C:\\Users\\file.txt");
    WinPath b("C:\\Users\\other.txt");
    EXPECT_FALSE(a.equals(b));
    EXPECT_TRUE(a != b);
}

TEST(WinPathTest, NotEqualsDifferentDrive) {
    WinPath a("C:\\Users\\file.txt");
    WinPath b("D:\\Users\\file.txt");
    EXPECT_FALSE(a.equals(b));
}

// =============================================================================
// Case Insensitive Comparison Tests
// =============================================================================

TEST(CaseInsensitiveTest, EqualStrings) {
    EXPECT_TRUE(case_insensitive_equals("hello", "HELLO"));
    EXPECT_TRUE(case_insensitive_equals("Hello", "hElLo"));
    EXPECT_TRUE(case_insensitive_equals("", ""));
}

TEST(CaseInsensitiveTest, NotEqualStrings) {
    EXPECT_FALSE(case_insensitive_equals("hello", "world"));
    EXPECT_FALSE(case_insensitive_equals("hello", "hell"));
}

// =============================================================================
// PathTranslator Construction Tests
// =============================================================================

TEST(PathTranslatorTest, DefaultConstruction) {
    PathTranslator translator;
    EXPECT_FALSE(translator.is_valid());
    EXPECT_FALSE(static_cast<bool>(translator));
}

TEST(PathTranslatorTest, ConstructWithRoot) {
    PathTranslator translator("/home/user/.vitreo/capsules/test");
    EXPECT_TRUE(translator.is_valid());
    EXPECT_EQ(translator.capsule_root(), "/home/user/.vitreo/capsules/test");
}

// =============================================================================
// PathTranslator ToHost Tests
// =============================================================================

TEST(PathTranslatorTest, ToHostSimple) {
    PathTranslator translator("/capsule");
    WinPath win_path("C:\\Users\\file.txt");
    
    auto host = translator.to_host(win_path);
    EXPECT_EQ(host.string(), "/capsule/drives/c/Users/file.txt");
}

TEST(PathTranslatorTest, ToHostRoot) {
    PathTranslator translator("/capsule");
    WinPath win_path("C:\\");
    
    auto host = translator.to_host(win_path);
    EXPECT_EQ(host.string(), "/capsule/drives/c");
}

TEST(PathTranslatorTest, ToHostDifferentDrive) {
    PathTranslator translator("/capsule");
    WinPath win_path("D:\\Data\\info.txt");
    
    auto host = translator.to_host(win_path);
    EXPECT_EQ(host.string(), "/capsule/drives/d/Data/info.txt");
}

TEST(PathTranslatorTest, ToHostDeepPath) {
    PathTranslator translator("/capsule");
    WinPath win_path("C:\\Users\\John\\Documents\\Projects\\file.cpp");
    
    auto host = translator.to_host(win_path);
    EXPECT_EQ(host.string(), "/capsule/drives/c/Users/John/Documents/Projects/file.cpp");
}

// =============================================================================
// PathTranslator ToWindows Tests
// =============================================================================

TEST(PathTranslatorTest, ToWindowsSimple) {
    PathTranslator translator("/capsule");
    
    auto result = translator.to_windows("/capsule/drives/c/Users/file.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->drive_letter(), 'C');
    EXPECT_EQ(result->to_windows_string(), "C:\\Users\\file.txt");
}

TEST(PathTranslatorTest, ToWindowsRoot) {
    PathTranslator translator("/capsule");
    
    auto result = translator.to_windows("/capsule/drives/c");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->drive_letter(), 'C');
    EXPECT_TRUE(result->empty());
}

TEST(PathTranslatorTest, ToWindowsDifferentDrive) {
    PathTranslator translator("/capsule");
    
    auto result = translator.to_windows("/capsule/drives/d/Data/file.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->drive_letter(), 'D');
}

TEST(PathTranslatorTest, ToWindowsOutsideCapsule) {
    PathTranslator translator("/capsule");
    
    auto result = translator.to_windows("/other/path/file.txt");
    EXPECT_FALSE(result.has_value());
}

TEST(PathTranslatorTest, ToWindowsJustDrivesDir) {
    PathTranslator translator("/capsule");
    
    auto result = translator.to_windows("/capsule/drives");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// PathTranslator Validation Tests
// =============================================================================

TEST(PathTranslatorTest, IsWithinCapsule) {
    PathTranslator translator("/capsule");
    
    EXPECT_TRUE(translator.is_within_capsule("/capsule/drives/c/file.txt"));
    EXPECT_TRUE(translator.is_within_capsule("/capsule/other/file.txt"));
    EXPECT_FALSE(translator.is_within_capsule("/other/file.txt"));
}

TEST(PathTranslatorTest, GetDriveForPath) {
    PathTranslator translator("/capsule");
    
    auto drive_c = translator.get_drive_for_path("/capsule/drives/c/Users/file.txt");
    ASSERT_TRUE(drive_c.has_value());
    EXPECT_EQ(*drive_c, 'C');
    
    auto drive_d = translator.get_drive_for_path("/capsule/drives/d/Data");
    ASSERT_TRUE(drive_d.has_value());
    EXPECT_EQ(*drive_d, 'D');
    
    auto no_drive = translator.get_drive_for_path("/other/path");
    EXPECT_FALSE(no_drive.has_value());
}

TEST(PathTranslatorTest, DrivePath) {
    PathTranslator translator("/capsule");
    
    EXPECT_EQ(translator.drive_path('C').string(), "/capsule/drives/c");
    EXPECT_EQ(translator.drive_path('c').string(), "/capsule/drives/c");
    EXPECT_EQ(translator.drive_path('D').string(), "/capsule/drives/d");
}

TEST(PathTranslatorTest, DrivesRoot) {
    PathTranslator translator("/capsule");
    EXPECT_EQ(translator.drives_root().string(), "/capsule/drives");
}

// =============================================================================
// PathTranslator Roundtrip Tests
// =============================================================================

TEST(PathTranslatorTest, RoundtripSimple) {
    PathTranslator translator("/capsule");
    WinPath original("C:\\Users\\John\\file.txt");
    
    auto host = translator.to_host(original);
    auto back = translator.to_windows(host);
    
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(original.equals(*back));
}

TEST(PathTranslatorTest, RoundtripDifferentDrives) {
    PathTranslator translator("/capsule");
    
    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        std::string path_str;
        path_str += drive;
        path_str += ":\\Test\\file.txt";
        
        WinPath original(path_str);
        auto host = translator.to_host(original);
        auto back = translator.to_windows(host);
        
        ASSERT_TRUE(back.has_value()) << "Failed for drive " << drive;
        EXPECT_EQ(back->drive_letter(), drive) << "Drive mismatch for " << drive;
    }
}

TEST(PathTranslatorTest, RoundtripDeepPath) {
    PathTranslator translator("/capsule");
    WinPath original("C:\\a\\b\\c\\d\\e\\f\\g\\h\\i\\j\\file.txt");
    
    auto host = translator.to_host(original);
    auto back = translator.to_windows(host);
    
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(original.components().size(), back->components().size());
    EXPECT_TRUE(original.equals(*back));
}
