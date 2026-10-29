// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_storage_format.cpp
/// @brief Compound File Binary Format writer/reader tests.
///
/// The file a docfile leaves behind outlives the process that wrote it — an
/// installer writes its uninstall log here and reads it back much later — so
/// the bytes have to be right, not merely round-trippable by Vitreo's own
/// reader. These tests pin both: the header fields a foreign reader keys off,
/// and a full tree surviving a write/read cycle.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/com/ole32/storage.hpp>

#include <cstring>
#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

std::shared_ptr<StorageNode> make_stream(const std::string& name,
                                         const std::string& contents) {
    auto node = std::make_shared<StorageNode>();
    node->name = name;
    node->is_storage = false;
    node->data.assign(contents.begin(), contents.end());
    return node;
}

std::shared_ptr<StorageNode> make_root() {
    auto root = std::make_shared<StorageNode>();
    root->name = "Root Entry";
    root->is_storage = true;
    return root;
}

[[nodiscard]] u16 get_u16(const std::vector<u8>& image, usize offset) {
    u16 v = 0; std::memcpy(&v, image.data() + offset, sizeof(v)); return v;
}
[[nodiscard]] u32 get_u32(const std::vector<u8>& image, usize offset) {
    u32 v = 0; std::memcpy(&v, image.data() + offset, sizeof(v)); return v;
}

} // namespace

// =============================================================================
// Header
// =============================================================================

TEST(CompoundFileWriter, EmitsAValidVersion3Header) {
    auto root = make_root();
    const auto image = serialize_compound_file(*root);

    ASSERT_GE(image.size(), 512u);
    static constexpr std::array<u8, 8> kSignature{
        0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    EXPECT_EQ(std::memcmp(image.data(), kSignature.data(), kSignature.size()), 0);

    EXPECT_EQ(get_u16(image, 0x1A), 3);        // major version
    EXPECT_EQ(get_u16(image, 0x1C), 0xFFFE);   // little-endian
    EXPECT_EQ(get_u16(image, 0x1E), 9);        // 512-byte sectors
    EXPECT_EQ(get_u16(image, 0x20), 6);        // 64-byte mini sectors
    EXPECT_EQ(get_u32(image, 0x38), 4096u);    // mini stream cutoff

    // The image must be a whole number of sectors, header included.
    EXPECT_EQ(image.size() % 512u, 0u);
}

TEST(CompoundFileWriter, EmptyDocfileStillHasARootDirectoryEntry) {
    auto root = make_root();
    const auto image = serialize_compound_file(*root);

    const u32 dir_sector = get_u32(image, 0x30);
    const usize dir = 512u + static_cast<usize>(dir_sector) * 512u;
    ASSERT_LE(dir + 128, image.size());

    // Root Entry, object type 5, no children.
    EXPECT_EQ(image[dir + 0x42], 5);
    EXPECT_EQ(get_u32(image, dir + 0x4C), 0xFFFFFFFFu); // child = NOSTREAM
    EXPECT_EQ(get_u16(image, dir + 0x00), u'R');
}

// =============================================================================
// Round trips
// =============================================================================

TEST(CompoundFile, RoundTripsASingleSmallStream) {
    auto root = make_root();
    root->children["Data"] = make_stream("Data", "hello compound file");

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(parsed->children.size(), 1u);
    auto it = parsed->children.find("Data");
    ASSERT_NE(it, parsed->children.end());
    EXPECT_FALSE(it->second->is_storage);
    EXPECT_EQ(std::string(it->second->data.begin(), it->second->data.end()),
              "hello compound file");
}

TEST(CompoundFile, RoundTripsAStreamLargerThanTheMiniCutoff) {
    // Streams >= 4096 bytes live in normal sectors rather than the mini
    // stream; a reader picks between them purely from the recorded size, so
    // both paths have to agree.
    auto root = make_root();
    const std::string big(10000, 'x');
    root->children["Big"] = make_stream("Big", big);

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    auto it = parsed->children.find("Big");
    ASSERT_NE(it, parsed->children.end());
    EXPECT_EQ(it->second->data.size(), big.size());
    EXPECT_EQ(std::string(it->second->data.begin(), it->second->data.end()), big);
}

TEST(CompoundFile, RoundTripsBothSizeClassesTogether) {
    auto root = make_root();
    root->children["Small"] = make_stream("Small", "tiny");
    root->children["Large"] = make_stream("Large", std::string(5000, 'L'));
    root->children["Empty"] = make_stream("Empty", "");

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(parsed->children.size(), 3u);
    EXPECT_EQ(parsed->children["Small"]->data.size(), 4u);
    EXPECT_EQ(parsed->children["Large"]->data.size(), 5000u);
    EXPECT_TRUE(parsed->children["Empty"]->data.empty());
}

TEST(CompoundFile, RoundTripsNestedStorages) {
    auto root = make_root();
    auto folder = std::make_shared<StorageNode>();
    folder->name = "Folder";
    folder->is_storage = true;
    folder->children["Inner"] = make_stream("Inner", "nested contents");

    auto deeper = std::make_shared<StorageNode>();
    deeper->name = "Deeper";
    deeper->is_storage = true;
    deeper->children["Leaf"] = make_stream("Leaf", "leaf");
    folder->children["Deeper"] = deeper;

    root->children["Folder"] = folder;
    root->children["Top"] = make_stream("Top", "top level");

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(parsed->children.size(), 2u);

    auto folder_it = parsed->children.find("Folder");
    ASSERT_NE(folder_it, parsed->children.end());
    ASSERT_TRUE(folder_it->second->is_storage);
    ASSERT_EQ(folder_it->second->children.size(), 2u);

    auto inner = folder_it->second->children.find("Inner");
    ASSERT_NE(inner, folder_it->second->children.end());
    EXPECT_EQ(std::string(inner->second->data.begin(), inner->second->data.end()),
              "nested contents");

    auto deeper_it = folder_it->second->children.find("Deeper");
    ASSERT_NE(deeper_it, folder_it->second->children.end());
    ASSERT_TRUE(deeper_it->second->is_storage);
    auto leaf = deeper_it->second->children.find("Leaf");
    ASSERT_NE(leaf, deeper_it->second->children.end());
    EXPECT_EQ(std::string(leaf->second->data.begin(), leaf->second->data.end()),
              "leaf");
}

TEST(CompoundFile, RoundTripsManySiblings) {
    // Siblings are stored as a binary tree ordered by name length then
    // case-folded name; a reader that walks it must find every one.
    auto root = make_root();
    for (int i = 0; i < 40; ++i) {
        const std::string name = "Stream" + std::to_string(i);
        root->children[name] = make_stream(name, "body-" + std::to_string(i));
    }

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->children.size(), 40u);
    for (int i = 0; i < 40; ++i) {
        const std::string name = "Stream" + std::to_string(i);
        auto it = parsed->children.find(name);
        ASSERT_NE(it, parsed->children.end()) << name;
        EXPECT_EQ(std::string(it->second->data.begin(), it->second->data.end()),
                  "body-" + std::to_string(i));
    }
}

TEST(CompoundFile, ElementNamesAreCaseInsensitive) {
    auto root = make_root();
    root->children["Contents"] = make_stream("Contents", "x");

    // Windows compound-file lookup folds case, so both spellings must hit.
    EXPECT_NE(root->children.find("CONTENTS"), root->children.end());
    EXPECT_NE(root->children.find("contents"), root->children.end());
}

TEST(CompoundFile, PreservesAStorageClsid) {
    auto root = make_root();
    auto folder = std::make_shared<StorageNode>();
    folder->name = "Typed";
    folder->is_storage = true;
    folder->clsid = {0x47,0x62,0x63,0x94,0x39,0xBC,0x8B,0x4B,
                     0xA7,0x28,0x2D,0x1F,0xBE,0xBF,0xA7,0x6A};
    root->children["Typed"] = folder;

    const auto image = serialize_compound_file(*root);
    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    auto it = parsed->children.find("Typed");
    ASSERT_NE(it, parsed->children.end());
    EXPECT_EQ(it->second->clsid, folder->clsid);
}

TEST(CompoundFile, PreservesNamesOutsideLatin1) {
    // Directory entry names are UTF-16 on disk. Installer packages name their
    // table streams in a private Unicode range (roughly U+3800..U+4840), so a
    // reader that narrows each code unit to its low byte silently renames
    // every stream and makes the tables unreachable rather than failing.
    // U+4840 U+3B3F U+43F2 is the encoded form of a table stream name.
    const std::string encoded = "\xE4\xA1\x80\xE3\xAC\xBF\xE4\x8F\xB2";

    auto root = make_root();
    root->children[encoded] = make_stream(encoded, "rows");

    const auto image = serialize_compound_file(*root);

    // Pin the on-disk form, not just the round trip: before this was fixed the
    // reader and writer were symmetrically wrong, so a Vitreo-written file
    // still read back correctly while every real installer package did not.
    // The name must appear as three UTF-16LE code units, not as its UTF-8
    // bytes widened one per unit.
    const std::vector<u8> expected_units = {
        0x40, 0x48,  // U+4840
        0x3F, 0x3B,  // U+3B3F
        0xF2, 0x43,  // U+43F2
    };
    const auto found = std::search(image.begin(), image.end(),
                                   expected_units.begin(), expected_units.end());
    EXPECT_NE(found, image.end())
        << "directory entry name was not written as UTF-16LE";

    auto parsed = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));

    ASSERT_NE(parsed, nullptr);
    auto it = parsed->children.find(encoded);
    ASSERT_NE(it, parsed->children.end());
    EXPECT_EQ(it->second->name, encoded);
}

// =============================================================================
// Rejecting what is not a compound file
// =============================================================================

TEST(CompoundFileReader, RejectsNonCompoundFiles) {
    std::vector<u8> pe(4096, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    EXPECT_EQ(parse_compound_file(std::span<const u8>(pe.data(), pe.size())),
              nullptr);

    const std::vector<u8> empty;
    EXPECT_EQ(parse_compound_file(std::span<const u8>(empty.data(), empty.size())),
              nullptr);

    std::vector<u8> tiny(16, 0);
    EXPECT_EQ(parse_compound_file(std::span<const u8>(tiny.data(), tiny.size())),
              nullptr);
}

TEST(CompoundFileReader, RejectsAnUnsupportedSectorSize) {
    auto root = make_root();
    auto image = serialize_compound_file(*root);
    // 4096-byte sectors (version 4) — recognisable, but not what this reader
    // decodes, so it must decline rather than misread the layout.
    const u16 shift = 12;
    std::memcpy(image.data() + 0x1E, &shift, sizeof(shift));
    EXPECT_EQ(parse_compound_file(std::span<const u8>(image.data(), image.size())),
              nullptr);
}
