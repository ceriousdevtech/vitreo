/// @file test_resource_x64.cpp
/// @brief x64 HMODULE / LPCWSTR must not be truncated for resource APIs.
///
/// Chrome's mini_installer (ImageBase 0x140000000) calls
/// EnumResourceNamesW / FindResourceW / LoadResource with pointer-width
/// module and type/name strings. Truncating those to u32 made Enum fail with
/// ERROR_RESOURCE_TYPE_NOT_FOUND and the installer exit 112
/// (UNABLE_TO_EXTRACT_CHROME_ARCHIVE) before writing CHROME.7Z.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kImageBase = 0x140000000ull;
constexpr u32 kRsrcRva = 0x1000;
constexpr GuestAddr kTypeStr = 0x140002000ull;
constexpr GuestAddr kNameStr = 0x140002100ull;
constexpr GuestAddr kCallback = 0x140003000ull;

void write_u16_le(GuestMemory& mem, GuestAddr addr, u16 v) {
    ASSERT_TRUE(mem.write_u16(addr, v).is_ok());
}

void write_u32_le(GuestMemory& mem, GuestAddr addr, u32 v) {
    ASSERT_TRUE(mem.write_u32(addr, v).is_ok());
}

void write_wide(GuestMemory& mem, GuestAddr addr, const char16_t* s) {
    for (usize i = 0; s[i] != 0; ++i) {
        write_u16_le(mem, addr + i * 2, static_cast<u16>(s[i]));
    }
    write_u16_le(mem, addr + std::char_traits<char16_t>::length(s) * 2, 0);
}

/// Minimal PE resource tree: type "BN" / name "CHROME.7Z" / lang 1033.
void build_named_resource_tree(GuestMemory& mem, GuestAddr image_base,
                               u32 rsrc_rva, const char* payload,
                               u32 payload_size) {
    const GuestAddr rsrc = image_base + rsrc_rva;

    // String table inside .rsrc (offsets from rsrc base). Keep directories and
    // the data entry from overlapping — a lang dir is 16+8 bytes.
    constexpr u32 kNameDirOff = 0x30;
    constexpr u32 kLangDirOff = 0x50;
    constexpr u32 kDataEntryOff = 0x70;
    constexpr u32 kTypeNameOff = 0x90;
    constexpr u32 kResNameOff = 0xA0;
    constexpr u32 kPayloadOff = 0xC0;

    auto write_res_string = [&](u32 off, const char16_t* s) {
        const u16 len = static_cast<u16>(std::char_traits<char16_t>::length(s));
        write_u16_le(mem, rsrc + off, len);
        for (u16 i = 0; i < len; ++i) {
            write_u16_le(mem, rsrc + off + 2 + i * 2, static_cast<u16>(s[i]));
        }
    };
    write_res_string(kTypeNameOff, u"BN");
    write_res_string(kResNameOff, u"CHROME.7Z");

    // Root directory: 1 named entry.
    for (u32 i = 0; i < 16; ++i) {
        ASSERT_TRUE(mem.write_u8(rsrc + i, 0).is_ok());
    }
    write_u16_le(mem, rsrc + 12, 1); // NumberOfNamedEntries
    write_u16_le(mem, rsrc + 14, 0); // NumberOfIdEntries
    write_u32_le(mem, rsrc + 16, kTypeNameOff | 0x80000000u);
    write_u32_le(mem, rsrc + 20, kNameDirOff | 0x80000000u);

    // Name directory: 1 named entry.
    for (u32 i = 0; i < 16; ++i) {
        ASSERT_TRUE(mem.write_u8(rsrc + kNameDirOff + i, 0).is_ok());
    }
    write_u16_le(mem, rsrc + kNameDirOff + 12, 1);
    write_u16_le(mem, rsrc + kNameDirOff + 14, 0);
    write_u32_le(mem, rsrc + kNameDirOff + 16, kResNameOff | 0x80000000u);
    write_u32_le(mem, rsrc + kNameDirOff + 20, kLangDirOff | 0x80000000u);

    // Language directory: id 1033 → data entry.
    for (u32 i = 0; i < 16; ++i) {
        ASSERT_TRUE(mem.write_u8(rsrc + kLangDirOff + i, 0).is_ok());
    }
    write_u16_le(mem, rsrc + kLangDirOff + 12, 0);
    write_u16_le(mem, rsrc + kLangDirOff + 14, 1);
    write_u32_le(mem, rsrc + kLangDirOff + 16, 1033u);
    write_u32_le(mem, rsrc + kLangDirOff + 20, kDataEntryOff);

    // Data entry: OffsetToData is image RVA.
    write_u32_le(mem, rsrc + kDataEntryOff, rsrc_rva + kPayloadOff);
    write_u32_le(mem, rsrc + kDataEntryOff + 4, payload_size);
    write_u32_le(mem, rsrc + kDataEntryOff + 8, 0);
    write_u32_le(mem, rsrc + kDataEntryOff + 12, 0);

    ASSERT_TRUE(mem.write_bytes(rsrc + kPayloadOff,
                                reinterpret_cast<const Byte*>(payload),
                                payload_size)
                    .is_ok());
}

class ResourceX64Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kImageBase, 0x10000,
                                       MemoryProtection::ReadWrite)
                        .is_ok());
        // Scratch for EnumResourceNames string names.
        ASSERT_TRUE(memory_.map_region(0x5A200000, 0x1000,
                                       MemoryProtection::ReadWrite)
                        .is_ok());

        static constexpr char kPayload[] = "CHROME7ZDATA";
        build_named_resource_tree(memory_, kImageBase, kRsrcRva, kPayload,
                                  static_cast<u32>(sizeof(kPayload) - 1));
        write_wide(memory_, kTypeStr, u"BN");
        write_wide(memory_, kNameStr, u"CHROME.7Z");

        mgr_.set_resource_info(kImageBase, kRsrcRva, 0x1000);

        cpu_.init_long_mode();
        // Stack outside the 64 KiB image mapping at kImageBase.
        constexpr GuestAddr kStack = 0x140010000ull;
        ASSERT_TRUE(memory_.map_region(kStack, 0x1000,
                                       MemoryProtection::ReadWrite)
                        .is_ok());
        cpu_.gpr.rsp = kStack + 0x800;
        marshaler_.attach_cpu(cpu_);
    }

    GuestMemory memory_;
    capsule::Vfs vfs_;
    Kernel32ExtManager mgr_{memory_, vfs_};
    cpu::CpuState cpu_;
    Marshaler marshaler_{memory_};
};

} // namespace

TEST_F(ResourceX64Test, FindResourceWAcceptsHighImageBaseAndStringPointers) {
    cpu_.gpr.rcx = kImageBase;
    cpu_.gpr.rdx = kNameStr;
    cpu_.gpr.r8 = kTypeStr;
    mgr_.impl_FindResourceW(marshaler_);
    const u32 hrsrc = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(hrsrc, 0u);

    cpu_.gpr.rcx = kImageBase;
    cpu_.gpr.rdx = hrsrc;
    mgr_.impl_SizeofResource(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 12u);

    cpu_.gpr.rcx = kImageBase;
    cpu_.gpr.rdx = hrsrc;
    mgr_.impl_LoadResource(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, kImageBase + kRsrcRva + 0xC0u)
        << "LoadResource must return the full 64-bit guest address";

    cpu_.gpr.rcx = cpu_.gpr.rax;
    mgr_.impl_LockResource(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, kImageBase + kRsrcRva + 0xC0u);
}

TEST_F(ResourceX64Test, EnumResourceNamesWEnumeratesHighModuleNamedType) {
    std::vector<std::u16string> seen;
    mgr_.set_guest_call([&](GuestAddr fn, const u64* args, usize count,
                            bool is_64bit) -> u64 {
        EXPECT_EQ(fn, kCallback);
        EXPECT_TRUE(is_64bit);
        EXPECT_EQ(count, 4u);
        EXPECT_EQ(args[0], kImageBase);
        EXPECT_EQ(args[1], kTypeStr);
        if (count >= 3) {
            auto name =
                marshaler_.read_wide_string(static_cast<GuestAddr>(args[2]));
            EXPECT_TRUE(name.is_ok());
            if (name.is_ok()) {
                std::u16string w;
                for (char c : name.value()) {
                    w.push_back(
                        static_cast<char16_t>(static_cast<unsigned char>(c)));
                }
                seen.push_back(std::move(w));
            }
        }
        return 1; // continue
    });

    cpu_.gpr.rcx = kImageBase;
    cpu_.gpr.rdx = kTypeStr;
    cpu_.gpr.r8 = kCallback;
    cpu_.gpr.r9 = 0x99;
    mgr_.impl_EnumResourceNamesW(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], u"CHROME.7Z");
}

TEST_F(ResourceX64Test, TruncatedModuleBaseWouldMissWithoutFullPointers) {
    // Document the failure mode: low 32 bits of ImageBase alone are not a
    // registered module and must not accidentally hit the main-image path.
    const GuestAddr truncated = static_cast<GuestAddr>(
        static_cast<u32>(kImageBase));
    EXPECT_NE(truncated, kImageBase);
    EXPECT_NE(truncated, 0u);

    cpu_.gpr.rcx = truncated;
    cpu_.gpr.rdx = kNameStr;
    cpu_.gpr.r8 = kTypeStr;
    mgr_.impl_FindResourceW(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
}
