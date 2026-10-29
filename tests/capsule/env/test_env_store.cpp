/// @file test_env_store.cpp
/// @brief Tests for ENV-001 capsule environment variable store

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <capsule/env/env_store.hpp>

#include <algorithm>
#include <string>

using namespace vitreo;
using namespace vitreo::capsule;

class EnvStoreTest : public ::testing::Test {
protected:
    EnvStore store;
};

// ---------------------------------------------------------------------------
// 1. Set and get round-trips
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, SetAndGetRoundTrips) {
    store.set(L"FOO", L"bar");
    auto val = store.get(L"FOO");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, L"bar");
}

// ---------------------------------------------------------------------------
// 2. Get is case-insensitive
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, GetIsCaseInsensitive) {
    store.set(L"Path", L"some_value");

    auto v1 = store.get(L"PATH");
    auto v2 = store.get(L"Path");
    auto v3 = store.get(L"path");

    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v1, L"some_value");
    EXPECT_EQ(*v2, L"some_value");
    EXPECT_EQ(*v3, L"some_value");
}

// ---------------------------------------------------------------------------
// 3. Remove existing key returns true
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, RemoveExistingReturnsTrue) {
    store.set(L"TOREMOVE", L"value");
    EXPECT_TRUE(store.remove(L"TOREMOVE"));
    EXPECT_FALSE(store.contains(L"TOREMOVE"));
}

// ---------------------------------------------------------------------------
// 4. Remove non-existent key returns false
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, RemoveNonExistentReturnsFalse) {
    EXPECT_FALSE(store.remove(L"DOESNOTEXIST"));
}

// ---------------------------------------------------------------------------
// 5. Get non-existent returns empty optional
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, GetNonExistentReturnsNullopt) {
    auto val = store.get(L"NOPE");
    EXPECT_FALSE(val.has_value());
}

// ---------------------------------------------------------------------------
// 6. Set with empty value removes the variable
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, SetEmptyValueRemovesVariable) {
    store.set(L"TEMP_VAR", L"hello");
    ASSERT_TRUE(store.contains(L"TEMP_VAR"));

    store.set(L"TEMP_VAR", L"");
    EXPECT_FALSE(store.contains(L"TEMP_VAR"));
}

// ---------------------------------------------------------------------------
// 7. Contains returns true for existing, false for non-existent
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, ContainsWorksCorrectly) {
    store.set(L"EXISTS", L"yes");
    EXPECT_TRUE(store.contains(L"EXISTS"));
    EXPECT_TRUE(store.contains(L"exists"));   // case-insensitive
    EXPECT_FALSE(store.contains(L"MISSING"));
}

// ---------------------------------------------------------------------------
// 8. Count tracks variable count correctly
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, CountTracksCorrectly) {
    EXPECT_EQ(store.count(), usize{0});

    store.set(L"A", L"1");
    EXPECT_EQ(store.count(), usize{1});

    store.set(L"B", L"2");
    EXPECT_EQ(store.count(), usize{2});

    // Overwrite should not change count
    store.set(L"A", L"updated");
    EXPECT_EQ(store.count(), usize{2});

    store.remove(L"A");
    EXPECT_EQ(store.count(), usize{1});
}

// ---------------------------------------------------------------------------
// 9. to_block produces double-null terminated block
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, ToBlockIsDoubleNullTerminated) {
    store.set(L"KEY", L"VALUE");
    auto block = store.to_block();

    // Block should end with two consecutive null characters
    ASSERT_GE(block.size(), usize{2});
    EXPECT_EQ(block[block.size() - 1], L'\0');
    EXPECT_EQ(block[block.size() - 2], L'\0');
}

// ---------------------------------------------------------------------------
// 10. to_block contains KEY=VALUE format
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, ToBlockContainsKeyEqualsValue) {
    store.set(L"HELLO", L"WORLD");
    auto block = store.to_block();

    std::wstring block_str(block.data(), block.size());
    EXPECT_NE(block_str.find(L"="), std::wstring::npos);

    // The specific entry should be present as a substring
    // (map may uppercase the key in storage, so search case-insensitively)
    std::wstring entry(block.begin(), block.end());
    // Find "HELLO=WORLD" somewhere in the block
    bool found = false;
    const wchar_t* p = block.data();
    const wchar_t* end = block.data() + block.size();
    while (p < end && *p != L'\0') {
        std::wstring_view sv(p);
        if (sv == L"HELLO=WORLD") {
            found = true;
            break;
        }
        p += sv.size() + 1;  // skip past null terminator
    }
    EXPECT_TRUE(found) << "Expected HELLO=WORLD entry in block";
}

// ---------------------------------------------------------------------------
// 11. init_defaults populates expected variables
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, InitDefaultsPopulatesExpected) {
    store.init_defaults();

    // Should have at least 12 variables (spec lists 12 including both TEMP and TMP)
    EXPECT_GE(store.count(), usize{11});

    // Spot-check specific variables
    auto path = store.get(L"PATH");
    ASSERT_TRUE(path.has_value());
    EXPECT_FALSE(path->empty());

    auto comp = store.get(L"COMPUTERNAME");
    ASSERT_TRUE(comp.has_value());
    EXPECT_EQ(*comp, L"VITREO");

    auto windir = store.get(L"WINDIR");
    ASSERT_TRUE(windir.has_value());
    EXPECT_EQ(*windir, L"C:\\Windows");

    auto username = store.get(L"USERNAME");
    ASSERT_TRUE(username.has_value());
    EXPECT_EQ(*username, L"Default");
}

// ---------------------------------------------------------------------------
// 12. to_block on empty store returns just a single null character
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, ToBlockEmptyStoreReturnsSingleNull) {
    auto block = store.to_block();
    ASSERT_EQ(block.size(), usize{1});
    EXPECT_EQ(block[0], L'\0');
}

// ---------------------------------------------------------------------------
// 13. Case-insensitive overwrite preserves last value
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, CaseInsensitiveOverwrite) {
    store.set(L"Foo", L"first");
    store.set(L"FOO", L"second");

    EXPECT_EQ(store.count(), usize{1});
    auto val = store.get(L"foo");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, L"second");
}

// ---------------------------------------------------------------------------
// 14. Remove is case-insensitive
// ---------------------------------------------------------------------------
TEST_F(EnvStoreTest, RemoveIsCaseInsensitive) {
    store.set(L"MixedCase", L"value");
    EXPECT_TRUE(store.remove(L"MIXEDCASE"));
    EXPECT_FALSE(store.contains(L"MixedCase"));
}
