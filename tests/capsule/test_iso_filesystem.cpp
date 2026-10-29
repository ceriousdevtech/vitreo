#include <gtest/gtest.h>

#include "capsule/vfs/iso_filesystem.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace vitreo;
using namespace vitreo::capsule;

namespace {

void write_le32(u8* out, u32 value) {
    out[0] = static_cast<u8>(value);
    out[1] = static_cast<u8>(value >> 8);
    out[2] = static_cast<u8>(value >> 16);
    out[3] = static_cast<u8>(value >> 24);
}

void write_be32(u8* out, u32 value) {
    out[0] = static_cast<u8>(value >> 24);
    out[1] = static_cast<u8>(value >> 16);
    out[2] = static_cast<u8>(value >> 8);
    out[3] = static_cast<u8>(value);
}

usize write_record(u8* out, u32 extent, u32 size, bool directory,
                   std::span<const u8> name) {
    const usize length = 33 + name.size() + ((name.size() % 2) == 0 ? 1 : 0);
    out[0] = static_cast<u8>(length);
    write_le32(out + 2, extent);
    write_be32(out + 6, extent);
    write_le32(out + 10, size);
    write_be32(out + 14, size);
    out[25] = directory ? 2 : 0;
    out[28] = 1;
    out[31] = 1;
    out[32] = static_cast<u8>(name.size());
    std::copy(name.begin(), name.end(), out + 33);
    return length;
}

std::filesystem::path make_test_iso() {
    const auto path = std::filesystem::temp_directory_path() / "vitreo_iso_fs_test.iso";
    std::vector<u8> image(32 * 2048, 0);

    u8* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::copy_n("CD001", 5, pvd + 1);
    pvd[6] = 1;
    std::copy_n("VITREO_TEST", 11, pvd + 40);
    std::fill(pvd + 51, pvd + 72, ' ');
    write_le32(pvd + 80, 32);
    write_be32(pvd + 84, 32);
    const std::array<u8, 1> root_name{0};
    write_record(pvd + 156, 20, 2048, true, root_name);

    u8* terminator = image.data() + 17 * 2048;
    terminator[0] = 255;
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    u8* directory = image.data() + 20 * 2048;
    usize offset = write_record(directory, 20, 2048, true, root_name);
    const std::array<u8, 1> parent_name{1};
    offset += write_record(directory + offset, 20, 2048, true, parent_name);
    constexpr std::string_view filename = "HELLO.TXT;1";
    offset += write_record(directory + offset, 21, 12, false,
                           std::span<const u8>(reinterpret_cast<const u8*>(filename.data()),
                                               filename.size()));
    (void)offset;
    std::copy_n("Hello, ISO!", 11, image.data() + 21 * 2048);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    return path;
}

std::vector<u8> ucs2be(std::string_view ascii) {
    std::vector<u8> out;
    out.reserve(ascii.size() * 2);
    for (const char ch : ascii) {
        out.push_back(0);
        out.push_back(static_cast<u8>(ch));
    }
    return out;
}

/// A disc mastered with both trees: the 8.3 primary one and a Joliet
/// supplementary one carrying the real long filenames, which is what a real
/// Windows CD looks like.
std::filesystem::path make_joliet_iso() {
    const auto path =
        std::filesystem::temp_directory_path() / "vitreo_iso_joliet_test.iso";
    std::vector<u8> image(40 * 2048, 0);
    const std::array<u8, 1> root_name{0};
    const std::array<u8, 1> parent_name{1};

    // Primary volume descriptor, root directory at sector 20.
    u8* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::copy_n("CD001", 5, pvd + 1);
    pvd[6] = 1;
    std::copy_n("SHORTNAMES", 10, pvd + 40);
    std::fill(pvd + 50, pvd + 72, ' ');
    write_le32(pvd + 80, 40);
    write_be32(pvd + 84, 40);
    write_record(pvd + 156, 20, 2048, true, root_name);

    // Joliet supplementary descriptor, root directory at sector 22.
    u8* svd = image.data() + 17 * 2048;
    svd[0] = 2;
    std::copy_n("CD001", 5, svd + 1);
    svd[6] = 1;
    const auto joliet_label = ucs2be("Long Names");
    std::copy(joliet_label.begin(), joliet_label.end(), svd + 40);
    std::copy_n("%/E", 3, svd + 88);  // UCS-2 level 3
    write_le32(svd + 80, 40);
    write_be32(svd + 84, 40);
    write_record(svd + 156, 22, 2048, true, root_name);

    u8* terminator = image.data() + 18 * 2048;
    terminator[0] = 255;
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    // Primary tree: the 8.3 name.
    u8* primary_dir = image.data() + 20 * 2048;
    usize offset = write_record(primary_dir, 20, 2048, true, root_name);
    offset += write_record(primary_dir + offset, 20, 2048, true, parent_name);
    constexpr std::string_view short_name = "README.TXT;1";
    write_record(primary_dir + offset, 24, 12, false,
                 std::span<const u8>(
                     reinterpret_cast<const u8*>(short_name.data()),
                     short_name.size()));

    // Joliet tree: the same file under its real name.
    u8* joliet_dir = image.data() + 22 * 2048;
    offset = write_record(joliet_dir, 22, 2048, true, root_name);
    offset += write_record(joliet_dir + offset, 22, 2048, true, parent_name);
    const auto long_name = ucs2be("Read Me First.txt");
    write_record(joliet_dir + offset, 24, 12, false, long_name);

    std::copy_n("Hello, ISO!", 11, image.data() + 24 * 2048);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    return path;
}

std::pair<std::filesystem::path, std::filesystem::path> make_test_bin_and_cue() {
    const auto iso_path = make_test_iso();
    std::ifstream iso(iso_path, std::ios::binary);
    std::vector<u8> iso_data((std::istreambuf_iterator<char>(iso)),
                             std::istreambuf_iterator<char>());
    const auto bin_path = iso_path.parent_path() / "vitreo_iso_fs_test.bin";
    std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
    for (usize offset = 0; offset < iso_data.size(); offset += 2048) {
        std::array<u8, 2352> sector{};
        std::copy_n(iso_data.data() + offset, 2048, sector.data() + 16);
        bin.write(reinterpret_cast<const char*>(sector.data()), sector.size());
    }
    bin.close();

    const auto cue_path = iso_path.parent_path() / "vitreo_iso_fs_test.cue";
    std::ofstream cue(cue_path, std::ios::trunc);
    cue << "FILE \"" << bin_path.filename().string() << "\" BINARY\n"
        << "  TRACK 01 MODE1/2352\n"
        << "    INDEX 01 00:00:00\n";
    return {bin_path, cue_path};
}

} // namespace

TEST(IsoFileSystemTest, ListsReadsSeeksAndRejectsWrites) {
    const auto path = make_test_iso();
    IsoFileSystem fs(path.string());
    ASSERT_TRUE(fs.valid()) << fs.error_message();

    VolumeInfo volume;
    ASSERT_TRUE(fs.volume_info(volume));
    EXPECT_EQ(volume.label, "VITREO_TEST");
    EXPECT_EQ(volume.filesystem_name, "CDFS");
    EXPECT_EQ(volume.filesystem_flags & 0x4u, 0u);
    EXPECT_EQ(volume.sectors_per_cluster, 1u);
    EXPECT_EQ(volume.bytes_per_sector, 2048u);
    EXPECT_EQ(volume.free_clusters, 0u);
    EXPECT_EQ(volume.total_clusters, 32u);

    auto listing = fs.list_dir("");
    ASSERT_TRUE(listing.is_ok());
    ASSERT_EQ(listing.value().size(), 1u);
    EXPECT_EQ(listing.value()[0].name, "HELLO.TXT");
    EXPECT_TRUE(has_attribute(listing.value()[0].info.attributes, FileAttributes::ReadOnly));

    auto opened = fs.open("hello.txt", AccessMode::Read, ShareMode::Read,
                          CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    std::array<u8, 16> buffer{};
    auto read = fs.read(opened.value(), buffer);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read.value(), 12u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buffer.data()), 11), "Hello, ISO!");

    auto seek = fs.seek(opened.value(), 7, SeekOrigin::Begin);
    ASSERT_TRUE(seek.is_ok());
    std::array<u8, 4> tail{};
    read = fs.read(opened.value(), tail);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(tail.data()), 4), "ISO!");
    EXPECT_TRUE(fs.close(opened.value()).is_ok());

    auto write_open = fs.open("HELLO.TXT", AccessMode::Write, ShareMode::None,
                              CreationDisposition::OpenExisting);
    ASSERT_TRUE(write_open.is_err());
    EXPECT_EQ(write_open.error(), VfsError::AccessDenied);

    std::filesystem::remove(path);
}

TEST(IsoFileSystemTest, ReadsMode1BinAndCueWithoutExtraction) {
    const auto [bin_path, cue_path] = make_test_bin_and_cue();
    for (const auto& path : {bin_path, cue_path}) {
        IsoFileSystem fs(path.string());
        ASSERT_TRUE(fs.valid()) << path << ": " << fs.error_message();
        auto opened = fs.open("HELLO.TXT", AccessMode::Read, ShareMode::Read,
                              CreationDisposition::OpenExisting);
        ASSERT_TRUE(opened.is_ok());
        std::array<u8, 11> contents{};
        auto read = fs.read(opened.value(), contents);
        ASSERT_TRUE(read.is_ok());
        EXPECT_EQ(std::string(reinterpret_cast<char*>(contents.data()), contents.size()),
                  "Hello, ISO!");
    }
    std::filesystem::remove(bin_path);
    std::filesystem::remove(cue_path);
    std::filesystem::remove(std::filesystem::temp_directory_path() /
                            "vitreo_iso_fs_test.iso");
}

TEST(IsoFileSystemTest, PrefersJolietLongNamesOverThe83PrimaryTree) {
    const auto path = make_joliet_iso();
    IsoFileSystem fs(path.string());
    ASSERT_TRUE(fs.valid()) << fs.error_message();

    VolumeInfo volume;
    ASSERT_TRUE(fs.volume_info(volume));
    EXPECT_EQ(volume.label, "Long Names");

    auto listing = fs.list_dir("");
    ASSERT_TRUE(listing.is_ok());
    ASSERT_EQ(listing.value().size(), 1u);
    EXPECT_EQ(listing.value()[0].name, "Read Me First.txt");

    // The Joliet name resolves, and lookups stay case-insensitive.
    auto opened = fs.open("read me first.txt", AccessMode::Read, ShareMode::Read,
                          CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    std::array<u8, 11> contents{};
    auto read = fs.read(opened.value(), contents);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(contents.data()), contents.size()),
              "Hello, ISO!");

    // The 8.3 name belongs to the primary tree, which Joliet replaces.
    auto short_name = fs.stat("README.TXT");
    EXPECT_TRUE(short_name.is_err());

    std::filesystem::remove(path);
}

TEST(IsoFileSystemTest, KeepsPrimaryTreeWhenNoJolietDescriptorExists) {
    const auto path = make_test_iso();
    IsoFileSystem fs(path.string());
    ASSERT_TRUE(fs.valid()) << fs.error_message();
    auto listing = fs.list_dir("");
    ASSERT_TRUE(listing.is_ok());
    ASSERT_EQ(listing.value().size(), 1u);
    EXPECT_EQ(listing.value()[0].name, "HELLO.TXT");
    std::filesystem::remove(path);
}
