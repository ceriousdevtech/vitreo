#pragma once
/// @file module_test_fixture.hpp
/// @brief Shared test-DLL builders and gtest fixtures for the ModuleManager tests (test_module*.cpp)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <core/pe/parser/pe_structures.hpp>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

inline std::vector<u8> build_notification_test_dll(bool is_64bit) {
    constexpr u32 pe_offset = 0x80;
    constexpr u32 headers_size = 0x200;
    constexpr u32 section_offset = 0x200;
    std::vector<u8> data(0x400, 0);

    auto* dos = reinterpret_cast<pe::DosHeader*>(data.data());
    dos->e_magic = pe::DOS_MAGIC;
    dos->e_lfanew = pe_offset;
    const u32 signature = pe::PE_SIGNATURE;
    std::memcpy(data.data() + pe_offset, &signature, sizeof(signature));

    auto* coff = reinterpret_cast<pe::CoffHeader*>(
        data.data() + pe_offset + sizeof(signature));
    coff->Machine = static_cast<u16>(
        is_64bit ? pe::MachineType::AMD64 : pe::MachineType::I386);
    coff->NumberOfSections = 1;
    coff->SizeOfOptionalHeader = static_cast<u16>(
        is_64bit ? sizeof(pe::OptionalHeader64)
                 : sizeof(pe::OptionalHeader32));
    coff->Characteristics = pe::CoffCharacteristics::EXECUTABLE_IMAGE |
                            pe::CoffCharacteristics::DLL;

    const usize optional_offset =
        pe_offset + sizeof(signature) + sizeof(pe::CoffHeader);
    if (is_64bit) {
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
            data.data() + optional_offset);
        optional->Magic = pe::PE32PLUS_MAGIC;
        optional->ImageBase = 0x0000000180000000ull;
        optional->SectionAlignment = 0x1000;
        optional->FileAlignment = 0x200;
        optional->SizeOfImage = 0x2000;
        optional->SizeOfHeaders = headers_size;
        optional->Subsystem = static_cast<u16>(pe::Subsystem::WindowsCui);
        optional->NumberOfRvaAndSizes = pe::DIR_COUNT;
    } else {
        auto* optional = reinterpret_cast<pe::OptionalHeader32*>(
            data.data() + optional_offset);
        optional->Magic = pe::PE32_MAGIC;
        optional->ImageBase = 0x18000000u;
        optional->SectionAlignment = 0x1000;
        optional->FileAlignment = 0x200;
        optional->SizeOfImage = 0x2000;
        optional->SizeOfHeaders = headers_size;
        optional->Subsystem = static_cast<u16>(pe::Subsystem::WindowsCui);
        optional->NumberOfRvaAndSizes = pe::DIR_COUNT;
    }

    auto* section = reinterpret_cast<pe::SectionHeader*>(
        data.data() + optional_offset + coff->SizeOfOptionalHeader);
    std::memcpy(section->Name, ".text\0\0\0", 8);
    section->VirtualSize = 1;
    section->VirtualAddress = 0x1000;
    section->SizeOfRawData = 0x200;
    section->PointerToRawData = section_offset;
    section->Characteristics = pe::SCN_CNT_CODE | pe::SCN_MEM_EXECUTE |
                               pe::SCN_MEM_READ;
    data[section_offset] = 0xC3;
    return data;
}

inline std::vector<u8> build_static_tls_test_dll(bool is_64bit) {
    auto data = build_notification_test_dll(is_64bit);
    constexpr u32 pe_offset = 0x80;
    constexpr u32 section_offset = 0x200;
    constexpr u32 tls_rva = 0x1100;
    constexpr u32 template_rva = 0x1180;
    constexpr u32 index_rva = 0x1190;

    const usize optional_offset = pe_offset + sizeof(u32) +
                                  sizeof(pe::CoffHeader);
    u64 image_base = 0;
    if (is_64bit) {
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
            data.data() + optional_offset);
        optional->AddressOfEntryPoint = 0x1000;
        optional->DataDirectory[pe::DIR_TLS] = {
            tls_rva, static_cast<u32>(sizeof(u64) * 4 + sizeof(u32) * 2)};
        image_base = optional->ImageBase;
    } else {
        auto* optional = reinterpret_cast<pe::OptionalHeader32*>(
            data.data() + optional_offset);
        optional->AddressOfEntryPoint = 0x1000;
        optional->DataDirectory[pe::DIR_TLS] = {
            tls_rva, static_cast<u32>(sizeof(u32) * 6)};
        image_base = optional->ImageBase;
    }

    auto write32 = [&](usize offset, u32 value) {
        std::memcpy(data.data() + offset, &value, sizeof(value));
    };
    auto write64 = [&](usize offset, u64 value) {
        std::memcpy(data.data() + offset, &value, sizeof(value));
    };
    const usize tls_file = section_offset + (tls_rva - 0x1000);
    if (is_64bit) {
        write64(tls_file + 0, image_base + template_rva);
        write64(tls_file + 8, image_base + template_rva + 8);
        write64(tls_file + 16, image_base + index_rva);
        write64(tls_file + 24, 0);
        write32(tls_file + 32, 16);
    } else {
        write32(tls_file + 0, static_cast<u32>(image_base + template_rva));
        write32(tls_file + 4,
                static_cast<u32>(image_base + template_rva + 8));
        write32(tls_file + 8, static_cast<u32>(image_base + index_rva));
        write32(tls_file + 12, 0);
        write32(tls_file + 16, 16);
    }
    const std::array<u8, 8> tls_template = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
    std::memcpy(data.data() + section_offset + (template_rva - 0x1000),
                tls_template.data(), tls_template.size());

    auto* coff = reinterpret_cast<pe::CoffHeader*>(
        data.data() + pe_offset + sizeof(u32));
    auto* section = reinterpret_cast<pe::SectionHeader*>(
        data.data() + optional_offset + coff->SizeOfOptionalHeader);
    section->Characteristics |= pe::SCN_MEM_WRITE;
    return data;
}

inline void write_test_file(const std::filesystem::path& path,
                            const std::vector<u8>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

// Declared at global scope (not in an anonymous namespace) so every
// translation unit that defines TEST_P bodies for this suite names the same
// fixture type, as gtest requires.
class DllNotificationDispatchTest :
    public ::testing::TestWithParam<bool> {};

// =============================================================================
// Test Fixture
// =============================================================================

class ModuleManagerTest : public ::testing::Test {
protected:
    static constexpr GuestAddr THUNK_BASE    = 0x70000000;
    static constexpr GuestAddr MAIN_EXE_BASE = 0x00400000;

    void SetUp() override {
        registry_ = std::make_unique<ThunkRegistry>(THUNK_BASE);

        // Register a dummy kernel32 DLL with test functions
        DllExports k32_exports;
        k32_exports.entries.push_back(ExportEntry{"TestFunc", 1, [](Marshaler&) {}});
        k32_exports.entries.push_back(ExportEntry{"AnotherFunc", 2, [](Marshaler&) {}});
        auto result = registry_->register_dll("kernel32.dll", std::move(k32_exports));
        ASSERT_TRUE(result.is_ok());

        // Register a dummy ntdll
        DllExports ntdll_exports;
        ntdll_exports.entries.push_back(ExportEntry{"NtQueryInfo", 1, [](Marshaler&) {}});
        auto ntdll_result = registry_->register_dll("ntdll.dll", std::move(ntdll_exports));
        ASSERT_TRUE(ntdll_result.is_ok());

        mm_ = std::make_unique<ModuleManager>(*registry_);
        mm_->set_main_module(MAIN_EXE_BASE, "C:\\Program Files\\test.exe");
        mm_->register_module("kernel32.dll", ModuleManager::KERNEL32_BASE,
                             "C:\\Windows\\System32\\kernel32.dll");
    }

    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<ModuleManager> mm_;
};
