/// @file test_pathcch.cpp
/// @brief Contract tests for the KernelBase `PathCch*` family.
///
/// Every expectation here is an exact HRESULT plus the exact resulting string,
/// because the failure this family exists to prevent is a caller treating a
/// non-S_OK return as success. The worked examples and truth tables come from
/// the Microsoft Learn pathcch.h reference pages named on each block.

#include <gtest/gtest.h>

#include <winapi/kernel32/path/pathcch_core.hpp>

#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

using pathcch::PathString;

namespace {

/// Printable form for gtest failure messages (paths here are ASCII).
std::string narrow(const PathString& s) {
    std::string out;
    out.reserve(s.size());
    for (const char16_t c : s) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

constexpr usize kBig = 512; // comfortable buffer for the "normal case" rows

} // namespace

// =============================================================================
// PathCchSkipRoot
// nf-pathcch-pathcchskiproot
// =============================================================================

TEST(PathCchSkipRoot, RecognisedRootForms) {
    struct Case {
        const char16_t* path;
        u32 expected_hr;
        usize expected_root_len;
    };
    const Case cases[] = {
        // Drive-absolute and drive-relative.
        {u"C:\\dir\\file", pathcch::kSOk, 3},
        {u"C:\\", pathcch::kSOk, 3},
        {u"C:", pathcch::kSOk, 2},
        {u"C:dir", pathcch::kSOk, 2},
        // Rooted, no drive.
        {u"\\dir\\file", pathcch::kSOk, 1},
        {u"\\", pathcch::kSOk, 1},
        // UNC.
        {u"\\\\server\\share\\dir", pathcch::kSOk, 15},
        {u"\\\\server\\share", pathcch::kSOk, 14},
        {u"\\\\server", pathcch::kSOk, 8},
        {u"\\\\", pathcch::kSOk, 2},
        // Extended-length prefixed.
        {u"\\\\?\\C:\\dir", pathcch::kSOk, 7},
        {u"\\\\?\\C:", pathcch::kSOk, 6},
        {u"\\\\?\\UNC\\srv\\shr\\x", pathcch::kSOk, 16},
        {u"\\\\?\\UNC\\srv\\shr", pathcch::kSOk, 15},
        {u"\\\\?\\Volume{abc}\\dir", pathcch::kSOk, 16},
        // No recognisable root.
        {u"relative\\path", pathcch::kEInvalidArg, 0},
        {u"file.txt", pathcch::kEInvalidArg, 0},
        {u"", pathcch::kEInvalidArg, 0},
    };

    for (const auto& c : cases) {
        usize root_len = 12345;
        const u32 hr = pathcch::skip_root(c.path, root_len);
        EXPECT_EQ(hr, c.expected_hr) << narrow(c.path);
        EXPECT_EQ(root_len, c.expected_root_len) << narrow(c.path);
    }
}

TEST(PathCchSkipRoot, RootOnlyPathPointsAtTerminator) {
    // Learn: "If the path consists of only a root, this value will point to
    // the string's terminating null character."
    const PathString path = u"C:\\";
    usize root_len = 0;
    EXPECT_EQ(pathcch::skip_root(path, root_len), pathcch::kSOk);
    EXPECT_EQ(root_len, path.size());
}

// =============================================================================
// PathCchIsRoot
// nf-pathcch-pathcchisroot (truth table reproduced verbatim)
// =============================================================================

TEST(PathCchIsRoot, DocumentedTruthTable) {
    struct Case {
        const char16_t* path;
        bool expected;
    };
    const Case cases[] = {
        {u"c:\\", true},
        {u"c:", false},
        {u"c:\\path1", false},
        {u"\\path1", true},
        {u"path1", false},
        {u"\\\\path1\\path2", true},
        {u"\\\\path1\\path2\\", false},
        {u"\\\\path1\\path2\\path3", false},
        {u"\\\\path1", true},
        {u"\\\\path1\\", false},
        {u"\\\\", true},
        {u"\\\\?\\UNC\\", true},
        {u"\\\\?\\UNC\\path1\\path2", true},
        {u"\\\\?\\UNC\\path1\\path2\\", false},
        {u"\\\\?\\UNC\\path1\\path2\\path3", false},
        {u"\\\\?\\UNC\\path1", true},
        {u"\\\\?\\UNC\\path1\\", false},
        {u"\\\\?\\c:\\", true},
        {u"\\\\?\\c:", false},
        {u"\\\\?\\c:\\path1", false},
        {u"\\\\?\\Volume{guid}\\", true},
        {u"\\\\?\\Volume{guid}", false},
        {u"\\\\?\\Volume{guid}\\path1", false},
        {u"", false},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(pathcch::is_root(c.path), c.expected) << narrow(c.path);
    }
}

TEST(PathCchIsRoot, NullPathIsNotARoot) {
    EXPECT_FALSE(pathcch::is_root(PathString(), /*is_null=*/true));
}

// =============================================================================
// PathIsUNCEx
// nf-pathcch-pathisuncex
// =============================================================================

TEST(PathIsUNCEx, ClassifiesAndLocatesServer) {
    struct Case {
        const char16_t* path;
        bool expected;
        usize server_index;
    };
    const Case cases[] = {
        {u"\\\\server\\share", true, 2},
        {u"\\\\server", true, 2},
        {u"\\\\?\\UNC\\server\\share", true, 8},
        {u"\\\\?\\unc\\server\\share", true, 8}, // device namespace is case-blind
        {u"\\\\?\\C:\\dir", false, 0},
        {u"\\\\.\\PhysicalDrive0", false, 0},
        {u"C:\\dir", false, 0},
        {u"\\dir", false, 0},
        {u"", false, 0},
    };
    for (const auto& c : cases) {
        usize server = 999;
        EXPECT_EQ(pathcch::is_unc(c.path, &server), c.expected) << narrow(c.path);
        if (c.expected) EXPECT_EQ(server, c.server_index) << narrow(c.path);
    }
    EXPECT_FALSE(pathcch::is_unc(PathString(), nullptr, /*is_null=*/true));
}

// =============================================================================
// PathCchCanonicalize / PathCchCanonicalizeEx
// nf-pathcch-pathcchcanonicalize, -pathcchcanonicalizeex
// =============================================================================

TEST(PathCchCanonicalize, DocumentedExamples) {
    struct Case {
        const char16_t* in;
        const char16_t* expected;
    };
    const Case cases[] = {
        // The example table on the Learn page.
        {u"C:\\name_1\\.\\name_2\\..\\name_3", u"C:\\name_1\\name_3"},
        {u"C:\\name_1\\..\\name_2\\.\\name_3", u"C:\\name_2\\name_3"},
        {u"C:\\name_1\\name_2\\.\\name_3\\..\\name_4", u"C:\\name_1\\name_2\\name_4"},
        {u"C:\\name_1\\.\\name_2\\.\\name_3\\..\\name_4\\..", u"C:\\name_1\\name_2"},
        {u"C:\\name_1\\*...", u"C:\\name_1\\*."},
        {u"C:\\..", u"C:\\"},
    };
    for (const auto& c : cases) {
        PathString out;
        const PathString in(c.in);
        EXPECT_EQ(pathcch::canonicalize(out, kBig, &in, pathcch::kNone),
                  pathcch::kSOk)
            << narrow(in);
        EXPECT_EQ(narrow(out), narrow(c.expected)) << narrow(in);
    }
}

TEST(PathCchCanonicalize, TraversalBeyondRootAndEmptyResults) {
    struct Case {
        const char16_t* in;
        const char16_t* expected;
    };
    const Case cases[] = {
        // Learn: more ".." than segments yields S_OK and a single backslash.
        {u"C:\\a\\..\\..", u"C:\\"},
        {u"C:\\..\\..\\..\\b", u"C:\\b"},
        {u"\\a\\..\\..", u"\\"},
        {u"\\\\srv\\shr\\a\\..\\..\\..", u"\\\\srv\\shr\\"},
        {u"\\\\?\\C:\\a\\..\\..\\b", u"\\\\?\\C:\\b"},
        {u"a\\..", u"\\"},          // relative input that empties out
        {u"", u"\\"},               // empty input
        {u"C:", u"C:\\"},           // bare root drive gains a backslash
        {u"C:a\\b", u"C:a\\b"},     // drive-relative must not become absolute
        {u"C:\\a\\\\b", u"C:\\a\\b"}, // repeated separators collapse
        {u"C:\\a\\", u"C:\\a\\"},   // a trailing separator survives
        // Learn: canonicalize "does not convert forward slashes into back
        // slashes", so '/' is an ordinary name character here.
        {u"C:\\a/b\\..\\c", u"C:\\c"},
        // Relative ".." has nothing to remove and must survive.
        {u"..\\a", u"..\\a"},
    };
    for (const auto& c : cases) {
        PathString out;
        const PathString in(c.in);
        EXPECT_EQ(pathcch::canonicalize(out, kBig, &in, pathcch::kNone),
                  pathcch::kSOk)
            << narrow(in);
        EXPECT_EQ(narrow(out), narrow(c.expected)) << narrow(in);
    }
}

TEST(PathCchCanonicalize, NullInputYieldsSingleBackslash) {
    PathString out;
    EXPECT_EQ(pathcch::canonicalize(out, kBig, nullptr, pathcch::kNone),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "\\");
}

TEST(PathCchCanonicalize, BufferBoundaries) {
    const PathString in = u"C:\\a\\.\\b";  // canonicalizes to "C:\a\b", 6 chars
    PathString out;

    // Exact fit: 6 characters plus the terminator.
    EXPECT_EQ(pathcch::canonicalize(out, 7, &in, pathcch::kNone), pathcch::kSOk);
    EXPECT_EQ(narrow(out), "C:\\a\\b");

    // One character too small.
    EXPECT_EQ(pathcch::canonicalize(out, 6, &in, pathcch::kNone),
              pathcch::kInsufficientBuffer);
    EXPECT_TRUE(out.empty()); // failure must not leave a usable-looking result

    // cchPath of 0 and cchPath above PATHCCH_MAX_CCH.
    EXPECT_EQ(pathcch::canonicalize(out, 0, &in, pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::canonicalize(out, pathcch::kPathCchMaxCch + 1, &in,
                                    pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::canonicalize(out, pathcch::kPathCchMaxCch, &in,
                                    pathcch::kNone),
              pathcch::kSOk);
}

TEST(PathCchCanonicalizeEx, LongPathFlagRaisesTheLimit) {
    PathString in = u"C:\\";
    in.append(400, u'a'); // 403 characters, well past MAX_PATH
    PathString out;

    // Without PATHCCH_ALLOW_LONG_PATHS the result is capped at MAX_PATH.
    EXPECT_EQ(pathcch::canonicalize(out, pathcch::kPathCchMaxCch, &in,
                                    pathcch::kNone),
              pathcch::kFilenameTooLong);

    // With the flag, and a buffer larger than MAX_PATH, it succeeds.
    EXPECT_EQ(pathcch::canonicalize(out, pathcch::kPathCchMaxCch, &in,
                                    pathcch::kAllowLongPaths),
              pathcch::kSOk);
    EXPECT_EQ(out.size(), in.size());

    // Learn: "cchPathOut must be greater than MAX_PATH. If it is not, this
    // flag is ignored."
    EXPECT_EQ(pathcch::canonicalize(out, pathcch::kMaxPath, &in,
                                    pathcch::kAllowLongPaths),
              pathcch::kFilenameTooLong);
}

TEST(PathCchCanonicalizeEx, OptionFlags) {
    const PathString in = u"C:\\a\\b";
    PathString out;

    // PATHCCH_ENSURE_TRAILING_SLASH
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &in, pathcch::kEnsureTrailingSlash),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "C:\\a\\b\\");

    // PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH, drive and UNC forms.
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &in,
                                    pathcch::kEnsureIsExtendedLengthPath),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "\\\\?\\C:\\a\\b");
    const PathString unc = u"\\\\srv\\shr\\a";
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &unc,
                                    pathcch::kEnsureIsExtendedLengthPath),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "\\\\?\\UNC\\srv\\shr\\a");

    // PATHCCH_DO_NOT_NORMALIZE_SEGMENTS keeps trailing periods.
    const PathString dotted = u"C:\\a\\b...";
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &dotted, pathcch::kNone),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "C:\\a\\b");
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &dotted,
                                    pathcch::kDoNotNormalizeSegments),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "C:\\a\\b...");
}

TEST(PathCchCanonicalizeEx, RejectsIllegalFlagCombinations) {
    const PathString in = u"C:\\a";
    PathString out;
    // Learn: the FORCE_* options "can be used only when PATHCCH_ALLOW_LONG_PATHS
    // is specified", and are mutually exclusive.
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &in,
                                    pathcch::kForceEnableLongNameProcess),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &in,
                                    pathcch::kAllowLongPaths |
                                        pathcch::kForceEnableLongNameProcess |
                                        pathcch::kForceDisableLongNameProcess),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::canonicalize(out, kBig, &in, 0x80000000u),
              pathcch::kEInvalidArg);
}

// =============================================================================
// PathCchCombine / PathCchCombineEx
// nf-pathcch-pathcchcombine, -pathcchcombineex
// =============================================================================

TEST(PathCchCombineEx, CombinationRules) {
    struct Case {
        const char16_t* first;   // nullptr models a NULL pszPathIn
        const char16_t* second;  // nullptr models a NULL pszMore
        const char16_t* expected;
    };
    const Case cases[] = {
        {u"C:\\dir", u"file.txt", u"C:\\dir\\file.txt"},
        {u"C:\\dir\\", u"file.txt", u"C:\\dir\\file.txt"},
        // "If this path begins with a single backslash, it is combined with
        // only the root of the path pointed to by pszPathIn."
        {u"C:\\dir\\sub", u"\\other", u"C:\\other"},
        {u"\\\\srv\\shr\\x", u"\\y", u"\\\\srv\\shr\\y"},
        // "If this path is fully qualified, it is copied directly."
        {u"C:\\dir", u"D:\\abs", u"D:\\abs"},
        {u"C:\\dir", u"\\\\srv\\shr", u"\\\\srv\\shr"},
        {u"C:\\dir", u"\\\\?\\E:\\x", u"\\\\?\\E:\\x"},
        // Learn: both NULL or empty yields a single backslash.
        {nullptr, nullptr, u"\\"},
        {u"", u"", u"\\"},
        {u"C:\\a\\b", nullptr, u"C:\\a\\b"},
        {nullptr, u"rel\\x", u"rel\\x"},
        // The combined result is canonicalized.
        {u"C:\\a", u"..\\b", u"C:\\b"},
        {u"C:\\a", u"..\\..\\..\\b", u"C:\\b"},
        {u"C:\\a", u".\\b\\.\\c", u"C:\\a\\b\\c"},
    };

    for (const auto& c : cases) {
        PathString out;
        const PathString first = c.first ? PathString(c.first) : PathString();
        const PathString second = c.second ? PathString(c.second) : PathString();
        const u32 hr = pathcch::combine(out, kBig, c.first ? &first : nullptr,
                                        c.second ? &second : nullptr,
                                        pathcch::kNone);
        EXPECT_EQ(hr, pathcch::kSOk)
            << narrow(first) << " + " << narrow(second);
        EXPECT_EQ(narrow(out), narrow(c.expected))
            << narrow(first) << " + " << narrow(second);
    }
}

TEST(PathCchCombineEx, BufferBoundaries) {
    const PathString first = u"C:\\a";
    const PathString second = u"b";   // "C:\a\b", 6 characters
    PathString out;

    EXPECT_EQ(pathcch::combine(out, 7, &first, &second, pathcch::kNone),
              pathcch::kSOk);
    EXPECT_EQ(narrow(out), "C:\\a\\b");

    EXPECT_EQ(pathcch::combine(out, 6, &first, &second, pathcch::kNone),
              pathcch::kInsufficientBuffer);
    EXPECT_TRUE(out.empty());

    EXPECT_EQ(pathcch::combine(out, 0, &first, &second, pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::combine(out, pathcch::kPathCchMaxCch + 1, &first, &second,
                               pathcch::kNone),
              pathcch::kEInvalidArg);
}

TEST(PathCchCombineEx, OverLongInputIsFilenameTooLong) {
    PathString first = u"C:\\";
    first.append(400, u'a');
    const PathString second = u"b";
    PathString out;

    // PathCchCombine (PATHCCH_NONE) is restricted to MAX_PATH.
    EXPECT_EQ(pathcch::combine(out, pathcch::kPathCchMaxCch, &first, &second,
                               pathcch::kNone),
              pathcch::kFilenameTooLong);
    EXPECT_EQ(pathcch::combine(out, pathcch::kPathCchMaxCch, &first, &second,
                               pathcch::kAllowLongPaths),
              pathcch::kSOk);
}

// =============================================================================
// PathCchAppend / PathCchAppendEx
// nf-pathcch-pathcchappend, -pathcchappendex
// =============================================================================

TEST(PathCchAppendEx, AppendsInPlace) {
    PathString path = u"C:\\dir";
    const PathString more = u"file.txt";
    EXPECT_EQ(pathcch::append(path, kBig, &more, pathcch::kNone), pathcch::kSOk);
    EXPECT_EQ(narrow(path), "C:\\dir\\file.txt");

    // A fully-qualified pszMore replaces rather than appends.
    PathString path2 = u"C:\\dir";
    const PathString abs = u"D:\\other";
    EXPECT_EQ(pathcch::append(path2, kBig, &abs, pathcch::kNone), pathcch::kSOk);
    EXPECT_EQ(narrow(path2), "D:\\other");
}

TEST(PathCchAppendEx, ArgumentValidation) {
    PathString path = u"C:\\dir";
    const PathString more = u"file";

    // Learn's error table: "Either pszPath or pszMore is NULL, cchPath is 0,
    // or cchPath is greater than PATHCCH_MAX_CCH."
    EXPECT_EQ(pathcch::append(path, kBig, nullptr, pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::append(path, 0, &more, pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::append(path, pathcch::kPathCchMaxCch + 1, &more,
                              pathcch::kNone),
              pathcch::kEInvalidArg);
    // cchPath smaller than the string already in the buffer is a bad argument.
    EXPECT_EQ(pathcch::append(path, 3, &more, pathcch::kNone),
              pathcch::kEInvalidArg);
    EXPECT_EQ(narrow(path), "C:\\dir") << "failed append must not mutate";
}

TEST(PathCchAppendEx, BufferBoundaries) {
    const PathString more = u"b";
    PathString exact = u"C:\\a";   // becomes "C:\a\b", 6 characters
    EXPECT_EQ(pathcch::append(exact, 7, &more, pathcch::kNone), pathcch::kSOk);
    EXPECT_EQ(narrow(exact), "C:\\a\\b");

    PathString tight = u"C:\\a";
    EXPECT_EQ(pathcch::append(tight, 6, &more, pathcch::kNone),
              pathcch::kInsufficientBuffer);
    EXPECT_EQ(narrow(tight), "C:\\a");
}

// =============================================================================
// PathCchAddBackslash / PathCchAddBackslashEx
// nf-pathcch-pathcchaddbackslashex
// =============================================================================

TEST(PathCchAddBackslashEx, AddsAndReportsEnd) {
    PathString path = u"C:\\dir";
    usize end = 0;
    usize remaining = 0;
    EXPECT_EQ(pathcch::add_backslash(path, 10, &end, &remaining), pathcch::kSOk);
    EXPECT_EQ(narrow(path), "C:\\dir\\");
    EXPECT_EQ(end, 7u);        // offset of the terminating null
    EXPECT_EQ(remaining, 3u);  // unused characters, terminator included
}

TEST(PathCchAddBackslashEx, AlreadyEndsInBackslashIsSFalse) {
    PathString path = u"C:\\dir\\";
    usize end = 0;
    usize remaining = 0;
    EXPECT_EQ(pathcch::add_backslash(path, 10, &end, &remaining),
              pathcch::kSFalse);
    EXPECT_EQ(narrow(path), "C:\\dir\\");
    EXPECT_EQ(end, 7u);
    EXPECT_EQ(remaining, 3u);
}

TEST(PathCchAddBackslashEx, BufferBoundaries) {
    // "C:\dir" is 6 characters; adding a backslash needs 8 including the null.
    PathString exact = u"C:\\dir";
    EXPECT_EQ(pathcch::add_backslash(exact, 8, nullptr, nullptr), pathcch::kSOk);
    EXPECT_EQ(narrow(exact), "C:\\dir\\");

    PathString tight = u"C:\\dir";
    EXPECT_EQ(pathcch::add_backslash(tight, 7, nullptr, nullptr),
              pathcch::kInsufficientBuffer);
    EXPECT_EQ(narrow(tight), "C:\\dir") << "failure must not mutate the buffer";

    PathString any = u"C:\\dir";
    EXPECT_EQ(pathcch::add_backslash(any, 0, nullptr, nullptr),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::add_backslash(any, pathcch::kPathCchMaxCch + 1, nullptr,
                                     nullptr),
              pathcch::kEInvalidArg);
    // cchPath that cannot even hold the existing string is a bad argument.
    EXPECT_EQ(pathcch::add_backslash(any, 6, nullptr, nullptr),
              pathcch::kEInvalidArg);
}

// =============================================================================
// PathCchRemoveBackslash / PathCchRemoveBackslashEx
// nf-pathcch-pathcchremovebackslashex
// =============================================================================

TEST(PathCchRemoveBackslashEx, RemovesTrailingBackslash) {
    PathString path = u"C:\\dir\\";
    usize end = 0;
    usize remaining = 0;
    EXPECT_EQ(pathcch::remove_backslash(path, 10, &end, &remaining),
              pathcch::kSOk);
    EXPECT_EQ(narrow(path), "C:\\dir");
    EXPECT_EQ(end, 6u);
    EXPECT_EQ(remaining, 4u);
}

TEST(PathCchRemoveBackslashEx, RootKeepsItsBackslash) {
    // Learn: on a root path the backslash stays, ppszEnd points *at* the
    // backslash, and the remaining count includes it.
    PathString path = u"C:\\";
    usize end = 0;
    usize remaining = 0;
    EXPECT_EQ(pathcch::remove_backslash(path, 10, &end, &remaining),
              pathcch::kSFalse);
    EXPECT_EQ(narrow(path), "C:\\");
    EXPECT_EQ(end, 2u);
    EXPECT_EQ(remaining, 8u);
}

TEST(PathCchRemoveBackslashEx, NoTrailingBackslashIsSFalse) {
    PathString path = u"C:\\dir";
    usize end = 0;
    usize remaining = 0;
    EXPECT_EQ(pathcch::remove_backslash(path, 10, &end, &remaining),
              pathcch::kSFalse);
    EXPECT_EQ(narrow(path), "C:\\dir");
    EXPECT_EQ(end, 6u);
    EXPECT_EQ(remaining, 4u);

    // A UNC share with a trailing separator is not a root, so it loses it.
    PathString unc = u"\\\\srv\\shr\\";
    EXPECT_EQ(pathcch::remove_backslash(unc, 20, nullptr, nullptr),
              pathcch::kSOk);
    EXPECT_EQ(narrow(unc), "\\\\srv\\shr");
}

// =============================================================================
// PathCchFindExtension
// nf-pathcch-pathcchfindextension
// =============================================================================

TEST(PathCchFindExtension, LocatesExtensionOrTerminator) {
    struct Case {
        const char16_t* path;
        usize expected_index; // offset of '.', or of the terminator
    };
    const Case cases[] = {
        {u"C:\\dir\\file.txt", 11},
        {u"C:\\dir\\file", 11},       // no extension: index of the terminator
        {u"C:\\dir.x\\file", 13},     // a dot in a *directory* is not one
        {u"file.tar.gz", 8},          // the last dot wins
        {u"C:file.txt", 6},
        {u"", 0},
    };
    for (const auto& c : cases) {
        const PathString path(c.path);
        usize index = 999;
        EXPECT_EQ(pathcch::find_extension(path, kBig, index), pathcch::kSOk)
            << narrow(path);
        EXPECT_EQ(index, c.expected_index) << narrow(path);
    }
}

TEST(PathCchFindExtension, ArgumentValidation) {
    const PathString path = u"file.txt";
    usize index = 0;
    EXPECT_EQ(pathcch::find_extension(path, 0, index), pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::find_extension(path, pathcch::kPathCchMaxCch + 1, index),
              pathcch::kEInvalidArg);
    // cchPath too small to hold the string plus its terminator.
    EXPECT_EQ(pathcch::find_extension(path, 8, index), pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::find_extension(path, 9, index), pathcch::kSOk);
}

// =============================================================================
// PathCchAddExtension / PathCchRenameExtension / PathCchRemoveExtension
// nf-pathcch-pathcchaddextension, -pathcchrenameextension,
// -pathcchremoveextension
// =============================================================================

TEST(PathCchAddExtension, AddsWhenAbsent) {
    struct Case {
        const char16_t* path;
        const char16_t* ext;
        u32 expected_hr;
        const char16_t* expected_path;
    };
    const Case cases[] = {
        {u"C:\\dir\\file", u".txt", pathcch::kSOk, u"C:\\dir\\file.txt"},
        // Learn: "This string can be given either with or without a preceding
        // period".
        {u"C:\\dir\\file", u"txt", pathcch::kSOk, u"C:\\dir\\file.txt"},
        // Learn: an existing extension leaves the string alone, with S_FALSE.
        {u"C:\\dir\\file.txt", u".bak", pathcch::kSFalse, u"C:\\dir\\file.txt"},
        // Learn: an empty extension returns the original string unaltered,
        // with S_OK.
        {u"C:\\dir\\file", u"", pathcch::kSOk, u"C:\\dir\\file"},
        {u"C:\\dir\\file", u".", pathcch::kSOk, u"C:\\dir\\file"},
        // Path syntax and wildcards are not valid extension characters.
        {u"C:\\dir\\file", u".a\\b", pathcch::kEInvalidArg, u"C:\\dir\\file"},
        {u"C:\\dir\\file", u".a*", pathcch::kEInvalidArg, u"C:\\dir\\file"},
    };
    for (const auto& c : cases) {
        PathString path(c.path);
        const PathString ext(c.ext);
        EXPECT_EQ(pathcch::add_extension(path, kBig, &ext), c.expected_hr)
            << narrow(c.path) << " + " << narrow(ext);
        EXPECT_EQ(narrow(path), narrow(c.expected_path));
    }
}

TEST(PathCchAddExtension, BufferBoundaries) {
    const PathString ext = u".txt";
    PathString exact = u"file";  // "file.txt" is 8 characters
    EXPECT_EQ(pathcch::add_extension(exact, 9, &ext), pathcch::kSOk);
    EXPECT_EQ(narrow(exact), "file.txt");

    PathString tight = u"file";
    EXPECT_EQ(pathcch::add_extension(tight, 8, &ext),
              pathcch::kInsufficientBuffer);
    EXPECT_EQ(narrow(tight), "file");

    PathString any = u"file";
    EXPECT_EQ(pathcch::add_extension(any, 0, &ext), pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::add_extension(any, pathcch::kPathCchMaxCch + 1, &ext),
              pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::add_extension(any, kBig, nullptr), pathcch::kEInvalidArg);
}

TEST(PathCchRenameExtension, ReplacesOrAdds) {
    struct Case {
        const char16_t* path;
        const char16_t* ext;
        const char16_t* expected;
    };
    const Case cases[] = {
        {u"C:\\dir\\file.txt", u".bak", u"C:\\dir\\file.bak"},
        {u"C:\\dir\\file.txt", u"bak", u"C:\\dir\\file.bak"},
        // Learn: "If the path string does not end with an extension, the new
        // extension is added."
        {u"C:\\dir\\file", u".bak", u"C:\\dir\\file.bak"},
        // Learn: "In the case of an empty string (""), any existing extension
        // in the path string is removed."
        {u"C:\\dir\\file.txt", u"", u"C:\\dir\\file"},
    };
    for (const auto& c : cases) {
        PathString path(c.path);
        const PathString ext(c.ext);
        EXPECT_EQ(pathcch::rename_extension(path, kBig, &ext), pathcch::kSOk)
            << narrow(c.path);
        EXPECT_EQ(narrow(path), narrow(c.expected)) << narrow(c.path);
    }
}

TEST(PathCchRenameExtension, BufferBoundaries) {
    const PathString ext = u".bak";
    PathString exact = u"file.a";  // becomes "file.bak", 8 characters
    EXPECT_EQ(pathcch::rename_extension(exact, 9, &ext), pathcch::kSOk);
    EXPECT_EQ(narrow(exact), "file.bak");

    PathString tight = u"file.a";
    EXPECT_EQ(pathcch::rename_extension(tight, 8, &ext),
              pathcch::kInsufficientBuffer);
    EXPECT_EQ(narrow(tight), "file.a");
}

TEST(PathCchRemoveExtension, RemovesOrReportsSFalse) {
    PathString with = u"C:\\dir\\file.txt";
    EXPECT_EQ(pathcch::remove_extension(with, kBig), pathcch::kSOk);
    EXPECT_EQ(narrow(with), "C:\\dir\\file");

    PathString without = u"C:\\dir\\file";
    EXPECT_EQ(pathcch::remove_extension(without, kBig), pathcch::kSFalse);
    EXPECT_EQ(narrow(without), "C:\\dir\\file");

    PathString any = u"file.txt";
    EXPECT_EQ(pathcch::remove_extension(any, 0), pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::remove_extension(any, pathcch::kPathCchMaxCch + 1),
              pathcch::kEInvalidArg);
}

// =============================================================================
// PathCchRemoveFileSpec
// nf-pathcch-pathcchremovefilespec (example table reproduced)
// =============================================================================

TEST(PathCchRemoveFileSpec, DocumentedExamples) {
    struct Case {
        const char16_t* path;
        u32 expected_hr;
        const char16_t* expected;
    };
    const Case cases[] = {
        {u"C:\\path1", pathcch::kSOk, u"C:\\"},
        {u"C:\\path1\\path2", pathcch::kSOk, u"C:\\path1"},
        {u"C:\\path1\\", pathcch::kSOk, u"C:\\path1"},
        {u"\\\\path1\\path2\\path3", pathcch::kSOk, u"\\\\path1\\path2"},
        {u"\\path1", pathcch::kSOk, u"\\"},
        // Learn: "This function does not affect root paths such as "C:"."
        {u"C:\\", pathcch::kSFalse, u"C:\\"},
        {u"C:", pathcch::kSFalse, u"C:"},
        {u"\\", pathcch::kSFalse, u"\\"},
        {u"\\\\path1\\path2", pathcch::kSFalse, u"\\\\path1\\path2"},
        {u"\\\\?\\C:\\path1", pathcch::kSOk, u"\\\\?\\C:\\"},
    };
    for (const auto& c : cases) {
        PathString path(c.path);
        EXPECT_EQ(pathcch::remove_file_spec(path, kBig), c.expected_hr)
            << narrow(c.path);
        EXPECT_EQ(narrow(path), narrow(c.expected)) << narrow(c.path);
    }
}

// =============================================================================
// PathCchStripToRoot
// nf-pathcch-pathcchstriptoroot (example table reproduced)
// =============================================================================

TEST(PathCchStripToRoot, DocumentedExamples) {
    struct Case {
        const char16_t* path;
        u32 expected_hr;
        const char16_t* expected;
    };
    const Case cases[] = {
        {u"C:\\path1\\path2\\file", pathcch::kSOk, u"C:\\"},
        {u"\\\\path1\\path2\\path3", pathcch::kSOk, u"\\\\path1\\path2"},
        {u"\\path1", pathcch::kSOk, u"\\"},
        {u"\\\\?\\C:\\path1", pathcch::kSOk, u"\\\\?\\C:\\"},
        {u"\\\\?\\UNC\\srv\\shr\\x", pathcch::kSOk, u"\\\\?\\UNC\\srv\\shr"},
        // Already a root.
        {u"C:\\", pathcch::kSFalse, u"C:\\"},
        {u"\\\\path1\\path2", pathcch::kSFalse, u"\\\\path1\\path2"},
        // No root at all.
        {u"relative\\path", pathcch::kEInvalidArg, u"relative\\path"},
    };
    for (const auto& c : cases) {
        PathString path(c.path);
        EXPECT_EQ(pathcch::strip_to_root(path, kBig), c.expected_hr)
            << narrow(c.path);
        EXPECT_EQ(narrow(path), narrow(c.expected)) << narrow(c.path);
    }
}

// =============================================================================
// PathCchStripPrefix
// nf-pathcch-pathcchstripprefix
// =============================================================================

TEST(PathCchStripPrefix, RemovesExtendedLengthPrefix) {
    struct Case {
        const char16_t* path;
        u32 expected_hr;
        const char16_t* expected;
    };
    const Case cases[] = {
        {u"\\\\?\\C:\\dir\\file", pathcch::kSOk, u"C:\\dir\\file"},
        {u"\\\\?\\UNC\\srv\\shr\\x", pathcch::kSOk, u"\\\\srv\\shr\\x"},
        // Nothing to strip.
        {u"C:\\dir", pathcch::kSFalse, u"C:\\dir"},
        {u"\\\\srv\\shr", pathcch::kSFalse, u"\\\\srv\\shr"},
        // A prefix whose remainder is not a usable Win32 path is left alone.
        {u"\\\\?\\Volume{guid}\\x", pathcch::kSFalse, u"\\\\?\\Volume{guid}\\x"},
    };
    for (const auto& c : cases) {
        PathString path(c.path);
        EXPECT_EQ(pathcch::strip_prefix(path, kBig), c.expected_hr)
            << narrow(c.path);
        EXPECT_EQ(narrow(path), narrow(c.expected)) << narrow(c.path);
    }
}

TEST(PathCchStripPrefix, ArgumentValidation) {
    PathString path = u"\\\\?\\C:\\dir";
    EXPECT_EQ(pathcch::strip_prefix(path, 0), pathcch::kEInvalidArg);
    EXPECT_EQ(pathcch::strip_prefix(path, pathcch::kPathCchMaxCch + 1),
              pathcch::kEInvalidArg);
    EXPECT_EQ(narrow(path), "\\\\?\\C:\\dir");
}
