/// @file test_lz32.cpp
/// @brief Tests for the legacy LZ32 file APIs

#include <gtest/gtest.h>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/system/lz32/lz32.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using Bytes = std::vector<u8>;

void write_u32(Bytes& bytes, usize offset, u32 value) {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

Bytes make_szdd_literals(std::span<const u8> plain, u8 replaced = '_') {
    Bytes compressed(14, 0);
    const std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    std::copy(signature.begin(), signature.end(), compressed.begin());
    compressed[8] = 'A';
    compressed[9] = replaced;
    write_u32(compressed, 10, static_cast<u32>(plain.size()));
    for (usize offset = 0; offset < plain.size(); offset += 8) {
        const usize count = std::min<usize>(8, plain.size() - offset);
        compressed.push_back(static_cast<u8>((1u << count) - 1u));
        compressed.insert(
            compressed.end(),
            plain.begin() + static_cast<std::ptrdiff_t>(offset),
            plain.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return compressed;
}

class Lz32Test : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo-lz32-test-" + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        ASSERT_TRUE(std::filesystem::create_directories(root, error));
        ASSERT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(
                     root.string())).is_ok());
    }

    void TearDown() override {
        manager.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void create_manager() {
        manager = std::make_unique<Lz32Manager>(vfs);
    }

    void write_file(std::string_view relative_path, std::span<const u8> bytes) {
        std::ofstream stream(
            root / relative_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.good());
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream.good());
    }

    [[nodiscard]] Bytes read_file(std::string_view relative_path) {
        std::ifstream stream(root / relative_path, std::ios::binary);
        return Bytes(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    std::unique_ptr<Lz32Manager> manager;
};

[[nodiscard]] const ExportEntry* find_export(
    const DllExports& exports, std::string_view name) {
    for (const auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

TEST_F(Lz32Test, CopiesUncompressedFileAndReturnsByteCount) {
    const Bytes input{'p', 'l', 'a', 'i', 'n'};
    write_file("source.bin", input);
    create_manager();

    const auto source = manager->open_file("C:\\source.bin", 0);
    const auto destination = manager->open_file("C:\\output.bin", 0x1001);
    ASSERT_GE(source.handle, 0);
    ASSERT_GE(destination.handle, 0);

    EXPECT_EQ(manager->copy(source.handle, destination.handle), 5);
    manager->close(destination.handle);
    EXPECT_EQ(read_file("output.bin"), input);
}

TEST_F(Lz32Test, ExpandsSzddAndUsesUnderscoreFilenameFallback) {
    const Bytes plain{'s', 'e', 't', 'u', 'p', '-', 'd', 'l', 'l'};
    write_file("setup.dl_", make_szdd_literals(plain));
    create_manager();

    const auto source = manager->open_file("C:\\setup.dll", 0);
    const auto destination = manager->open_file("C:\\setup.dll.out", 0x1001);
    ASSERT_GE(source.handle, 0);
    ASSERT_GE(destination.handle, 0);
    EXPECT_EQ(source.path, "C:\\setup.dl_");

    EXPECT_EQ(manager->copy(source.handle, destination.handle), 9);
    manager->close(destination.handle);
    EXPECT_EQ(read_file("setup.dll.out"), plain);
}

TEST_F(Lz32Test, RejectsInvalidAndClosedHandles) {
    create_manager();
    EXPECT_EQ(manager->copy(99, 100), lz_errors::BADINHANDLE);

    const auto destination = manager->open_file("C:\\output.bin", 0x1001);
    ASSERT_GE(destination.handle, 0);
    EXPECT_EQ(
        manager->copy(99, destination.handle), lz_errors::BADINHANDLE);
    manager->close(destination.handle);
    EXPECT_EQ(
        manager->copy(destination.handle, destination.handle),
        lz_errors::BADINHANDLE);
}

TEST_F(Lz32Test, RecoversSzddOriginalNameFromHeader) {
    const Bytes plain{'s', 'e', 't', 'u', 'p'};
    write_file("setup.dl_", make_szdd_literals(plain, 'l'));
    create_manager();

    const auto from_compressed = manager->expanded_name("C:\\setup.dl_");
    EXPECT_EQ(from_compressed.code, 1);
    EXPECT_EQ(from_compressed.name, "C:\\setup.dll");

    const auto from_original = manager->expanded_name("C:\\setup.dll");
    EXPECT_EQ(from_original.code, 1);
    EXPECT_EQ(from_original.name, "C:\\setup.dll");
}

TEST_F(Lz32Test, DuplicatesSourceNameWhenFileIsNotSzdd) {
    const Bytes mz{'M', 'Z', 0x90, 0x00};
    write_file("payload.ex_", mz);
    create_manager();

    const auto from_media = manager->expanded_name("C:\\payload.ex_");
    EXPECT_EQ(from_media.code, 1);
    EXPECT_EQ(from_media.name, "C:\\payload.ex_");

    const auto from_original = manager->expanded_name("C:\\payload.exe");
    EXPECT_EQ(from_original.code, 1);
    EXPECT_EQ(from_original.name, "C:\\payload.exe");
}

TEST_F(Lz32Test, ExpandedNameRejectsMissingFile) {
    create_manager();
    const auto missing = manager->expanded_name("C:\\missing.exe");
    EXPECT_EQ(missing.code, lz_errors::BADVALUE);
    EXPECT_TRUE(missing.name.empty());
}

TEST_F(Lz32Test, PublishesDocumentedOrdinalsAndStdcallSizes) {
    create_manager();
    const DllExports exports = create_lz32_thunks(*manager);
    ASSERT_EQ(exports.entries.size(), 6u);

    const auto* open_a = find_export(exports, "LZOpenFileA");
    const auto* open_w = find_export(exports, "LZOpenFileW");
    const auto* copy = find_export(exports, "LZCopy");
    const auto* close = find_export(exports, "LZClose");
    const auto* expanded_a = find_export(exports, "GetExpandedNameA");
    const auto* expanded_w = find_export(exports, "GetExpandedNameW");
    ASSERT_NE(open_a, nullptr);
    ASSERT_NE(open_w, nullptr);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(close, nullptr);
    ASSERT_NE(expanded_a, nullptr);
    ASSERT_NE(expanded_w, nullptr);
    EXPECT_EQ(open_a->ordinal, 9);
    EXPECT_EQ(open_a->stdcall_bytes, 12);
    EXPECT_EQ(open_w->ordinal, 10);
    EXPECT_EQ(open_w->stdcall_bytes, 12);
    EXPECT_EQ(copy->ordinal, 5);
    EXPECT_EQ(copy->stdcall_bytes, 8);
    EXPECT_EQ(close->ordinal, 3);
    EXPECT_EQ(close->stdcall_bytes, 4);
    EXPECT_EQ(expanded_a->ordinal, 7);
    EXPECT_EQ(expanded_a->stdcall_bytes, 8);
    EXPECT_EQ(expanded_w->ordinal, 8);
    EXPECT_EQ(expanded_w->stdcall_bytes, 8);
}

} // namespace