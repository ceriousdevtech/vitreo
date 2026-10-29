/// @file test_imagehlp.cpp
/// @brief ImageHlp PE certificate-table contract tests

#include <gtest/gtest.h>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/system/imagehlp/imagehlp.hpp>

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

void write_u16(Bytes& bytes, usize offset, u16 value) {
    bytes[offset] = static_cast<u8>(value);
    bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void write_u32(Bytes& bytes, usize offset, u32 value) {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

void append_certificate(
    Bytes& bytes, u16 type, std::span<const u8> payload) {
    const u32 length = static_cast<u32>(8 + payload.size());
    const usize padded_length = (length + 7u) & ~usize{7};
    const usize offset = bytes.size();
    bytes.resize(offset + padded_length, 0);
    write_u32(bytes, offset, length);
    write_u16(bytes, offset + 4, 0x0200);
    write_u16(bytes, offset + 6, type);
    std::copy(payload.begin(), payload.end(), bytes.begin() +
              static_cast<std::ptrdiff_t>(offset + 8));
}

Bytes make_pe_with_certificates() {
    constexpr usize PE_OFFSET = 0x80;
    constexpr usize OPTIONAL_OFFSET = PE_OFFSET + 24;
    constexpr usize CERT_DIRECTORY = OPTIONAL_OFFSET + 112 + 4 * 8;
    constexpr usize CERT_OFFSET = 0x200;
    Bytes bytes(CERT_OFFSET, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    write_u32(bytes, 0x3c, PE_OFFSET);
    bytes[PE_OFFSET] = 'P';
    bytes[PE_OFFSET + 1] = 'E';
    write_u16(bytes, PE_OFFSET + 20, 0xF0);
    write_u16(bytes, OPTIONAL_OFFSET, 0x20B);
    write_u32(bytes, OPTIONAL_OFFSET + 108, 16);

    append_certificate(bytes, 2, std::array<u8, 4>{1, 2, 3, 4});
    append_certificate(bytes, 1, std::array<u8, 2>{5, 6});
    write_u32(bytes, CERT_DIRECTORY, CERT_OFFSET);
    write_u32(bytes, CERT_DIRECTORY + 4,
              static_cast<u32>(bytes.size() - CERT_OFFSET));
    return bytes;
}

class ImageHlpTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("vitreo-imagehlp-test-" + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        ASSERT_TRUE(std::filesystem::create_directories(root_, error));
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_.string()))
                        .is_ok());
        ASSERT_TRUE(memory_.map_region(
            MEMORY_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        files_ = std::make_unique<FileManager>(vfs_, handles_, memory_);
        imagehlp_ = std::make_unique<ImageHlpManager>(*files_);
        handles_.set_close_callback(
            [this](Handle handle, HandleType type, void* object) {
                return files_->on_handle_close(handle, type, object);
            });
    }

    void TearDown() override {
        handles_.set_close_callback({});
        imagehlp_.reset();
        files_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void write_file(std::string_view name, std::span<const u8> bytes) {
        std::ofstream stream(root_ / name, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.good());
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream.good());
    }

    Handle open_file(u32 access) {
        return files_->create_file("C:\\signed.exe", access,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   OPEN_EXISTING, 0);
    }

    static constexpr GuestAddr MEMORY_BASE = 0x10000;
    static constexpr GuestAddr COUNT = MEMORY_BASE + 0x100;
    static constexpr GuestAddr INDICES = MEMORY_BASE + 0x200;
    static constexpr GuestAddr CERTIFICATE = MEMORY_BASE + 0x300;
    static constexpr GuestAddr NEW_INDEX = MEMORY_BASE + 0x400;

    std::filesystem::path root_;
    GuestMemory memory_;
    HandleTable handles_;
    capsule::Vfs vfs_;
    std::unique_ptr<FileManager> files_;
    std::unique_ptr<ImageHlpManager> imagehlp_;
};

TEST_F(ImageHlpTest, EnumeratesAllCertificatesAndFiltersByType) {
    write_file("signed.exe", make_pe_with_certificates());
    const Handle handle = open_file(GENERIC_READ);
    ASSERT_NE(handle, INVALID_HANDLE);
    Marshaler marshaler{memory_};

    ASSERT_TRUE(imagehlp_->enumerate_certificates(
        marshaler, handle, 0xFF, COUNT, INDICES, 4));
    EXPECT_EQ(memory_.read_u32(COUNT).value(), 2u);
    EXPECT_EQ(memory_.read_u32(INDICES).value(), 0u);
    EXPECT_EQ(memory_.read_u32(INDICES + 4).value(), 1u);

    ASSERT_TRUE(imagehlp_->enumerate_certificates(
        marshaler, handle, 1, COUNT, INDICES, 1));
    EXPECT_EQ(memory_.read_u32(COUNT).value(), 1u);
    EXPECT_EQ(memory_.read_u32(INDICES).value(), 1u);
    EXPECT_TRUE(handles_.close(handle));
}

TEST_F(ImageHlpTest, AddsAndRemovesAlignedCertificateEntries) {
    write_file("signed.exe", make_pe_with_certificates());
    const Handle handle = open_file(GENERIC_READ | GENERIC_WRITE);
    ASSERT_NE(handle, INVALID_HANDLE);
    Marshaler marshaler{memory_};

    Bytes certificate(11, 0);
    write_u32(certificate, 0, 11);
    write_u16(certificate, 4, 0x0200);
    write_u16(certificate, 6, 3);
    certificate[8] = 7;
    certificate[9] = 8;
    certificate[10] = 9;
    ASSERT_TRUE(memory_.write_bytes(
        CERTIFICATE, reinterpret_cast<const Byte*>(certificate.data()),
        certificate.size()).is_ok());

    ASSERT_TRUE(imagehlp_->add_certificate(
        marshaler, handle, CERTIFICATE, NEW_INDEX));
    EXPECT_EQ(memory_.read_u32(NEW_INDEX).value(), 2u);
    ASSERT_TRUE(imagehlp_->remove_certificate(handle, 1));

    ASSERT_TRUE(imagehlp_->enumerate_certificates(
        marshaler, handle, 0xFF, COUNT, INDICES, 4));
    EXPECT_EQ(memory_.read_u32(COUNT).value(), 2u);
    ASSERT_TRUE(imagehlp_->enumerate_certificates(
        marshaler, handle, 3, COUNT, INDICES, 1));
    EXPECT_EQ(memory_.read_u32(COUNT).value(), 1u);
    EXPECT_EQ(memory_.read_u32(INDICES).value(), 1u);
    EXPECT_TRUE(handles_.close(handle));
}

TEST_F(ImageHlpTest, MutationsRequireReadWriteFileAccess) {
    write_file("signed.exe", make_pe_with_certificates());
    const Handle handle = open_file(GENERIC_READ);
    ASSERT_NE(handle, INVALID_HANDLE);
    Marshaler marshaler{memory_};
    ASSERT_TRUE(marshaler.write_u32(CERTIFICATE, 8).is_ok());

    EXPECT_FALSE(imagehlp_->add_certificate(
        marshaler, handle, CERTIFICATE, NEW_INDEX));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_FALSE(imagehlp_->remove_certificate(handle, 0));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_TRUE(handles_.close(handle));
}

// Stdcall sizes are the x86 import-library decorations (imagehlp.lib /
// dbghelp.lib): a DWORD64 argument takes 8 bytes.
TEST_F(ImageHlpTest, PublishesDocumentedExportsAndStdcallSizes) {
    const DllExports exports = create_imagehlp_thunks(*imagehlp_);
    const auto bytes_of = [&](const char* name) -> int {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.stdcall_bytes;
        }
        return -1;
    };
    EXPECT_EQ(bytes_of("ImageAddCertificate"), 12);
    EXPECT_EQ(bytes_of("ImageRemoveCertificate"), 8);
    EXPECT_EQ(bytes_of("ImageEnumerateCertificates"), 20);
    // Forwarded to dbghelp, as on Windows.
    EXPECT_EQ(bytes_of("SymFromAddr"), 20);            // _SymFromAddr@20
    EXPECT_EQ(bytes_of("SymGetLineFromAddr64"), 20);   // _SymGetLineFromAddr64@20
    EXPECT_EQ(bytes_of("SymGetModuleBase64"), 12);     // _SymGetModuleBase64@12
    EXPECT_EQ(bytes_of("SymFunctionTableAccess64"), 12);
    EXPECT_EQ(bytes_of("StackWalk64"), 36);
    EXPECT_EQ(bytes_of("SymLoadModule"), 24);          // _SymLoadModule@24
    EXPECT_EQ(bytes_of("SymGetSymFromAddr"), 16);      // _SymGetSymFromAddr@16
    // Not an imagehlp export on Windows 11.
    EXPECT_EQ(bytes_of("SymInitializeW"), -1);
}

} // namespace
