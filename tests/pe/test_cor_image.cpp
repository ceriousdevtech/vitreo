/// @file test_cor_image.cpp
/// @brief COR20 image facts and the 64-bit loader's PE32 -> PE32+ rewrite.
///
/// Expected layout from tests/oracle_data/cor_loader_probe64.expected
/// ("exe anycpu" and "dll il-only anycpu library": file vs mapped headers).

#include "core/pe/loader/cor_image.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::pe;

namespace {

void put16(std::vector<u8>& f, usize at, u16 v) { std::memcpy(f.data() + at, &v, 2); }
void put32(std::vector<u8>& f, usize at, u32 v) { std::memcpy(f.data() + at, &v, 4); }
void put64(std::vector<u8>& f, usize at, u64 v) { std::memcpy(f.data() + at, &v, 8); }
u16 get16(const std::vector<u8>& f, usize at) { u16 v; std::memcpy(&v, f.data() + at, 2); return v; }
u32 get32(const std::vector<u8>& f, usize at) { u32 v; std::memcpy(&v, f.data() + at, 4); return v; }
u64 get64(const std::vector<u8>& f, usize at) { u64 v; std::memcpy(&v, f.data() + at, 8); return v; }

constexpr usize kNt = 0x80;
constexpr usize kOpt = kNt + 24;

/// A csc-shaped AnyCPU image: e_lfanew 0x80, three sections, SizeOfHeaders
/// 0x200, a COR20 header at RVA 0x2008 (file 0x208) in .text.
std::vector<u8> il_image(bool pe32plus, u32 cor_flags) {
    std::vector<u8> f(0x800, 0);
    put16(f, 0, 0x5A4D);
    put32(f, 0x3C, kNt);
    put32(f, kNt, 0x00004550);
    put16(f, kNt + 4, pe32plus ? 0x8664 : 0x14C);
    put16(f, kNt + 6, 3);
    const u16 opt_size = pe32plus ? 0xF0 : 0xE0;
    put16(f, kNt + 20, opt_size);
    put16(f, kNt + 22, 0x102);
    put16(f, kOpt, pe32plus ? 0x20B : 0x10B);
    put32(f, kOpt + 16, 0x245E);       // AddressOfEntryPoint
    put32(f, kOpt + 20, 0x2000);       // BaseOfCode
    if (pe32plus) {
        put64(f, kOpt + 24, 0x140000000ull);
    } else {
        put32(f, kOpt + 24, 0x4000);   // BaseOfData
        put32(f, kOpt + 28, 0x400000); // ImageBase
    }
    put32(f, kOpt + 32, 0x2000);
    put32(f, kOpt + 36, 0x200);
    put32(f, kOpt + 56, 0x8000);       // SizeOfImage
    put32(f, kOpt + 60, 0x200);        // SizeOfHeaders
    put16(f, kOpt + 68, 3);
    put16(f, kOpt + 70, 0x8540);
    const usize dirs = kOpt + (pe32plus ? 112 : 96);
    if (pe32plus) {
        put64(f, kOpt + 72, 0x400000); put64(f, kOpt + 80, 0x4000);
        put64(f, kOpt + 88, 0x100000); put64(f, kOpt + 96, 0x2000);
    } else {
        put32(f, kOpt + 72, 0x100000); put32(f, kOpt + 76, 0x1000);
        put32(f, kOpt + 80, 0x100000); put32(f, kOpt + 84, 0x1000);
    }
    put32(f, dirs - 4, 16);
    put32(f, dirs + 1 * 8, 0x2404); put32(f, dirs + 1 * 8 + 4, 0x57);
    put32(f, dirs + 12 * 8, 0x2000); put32(f, dirs + 12 * 8 + 4, 8);
    put32(f, dirs + 14 * 8, 0x2008); put32(f, dirs + 14 * 8 + 4, 0x48);
    const usize table = kOpt + opt_size;
    const char* names[3] = {".text", ".rsrc", ".reloc"};
    for (int i = 0; i < 3; ++i) {
        const usize s = table + 40 * i;
        std::memcpy(f.data() + s, names[i], std::strlen(names[i]));
        put32(f, s + 8, 0x600);
        put32(f, s + 12, 0x2000 + 0x2000 * i);
        put32(f, s + 16, 0x200);
        put32(f, s + 20, 0x200 + 0x200 * i);
    }
    // COR20 header at file 0x208: cb, versions, MetaData {rva 0x2100, size},
    // Flags. Metadata root at file 0x300.
    put32(f, 0x208, 0x48);
    put32(f, 0x208 + 8, 0x2100);
    put32(f, 0x208 + 12, 0x100);
    put32(f, 0x208 + 16, cor_flags);
    put32(f, 0x300, 0x424A5342);
    put32(f, 0x300 + 12, 12);
    std::memcpy(f.data() + 0x310, "v4.0.30319\0\0", 12);
    return f;
}

TEST(CorImage, ReadsFlagsAndMetadataVersion) {
    const auto f = il_image(false, COMIMAGE_FLAGS_ILONLY);
    const auto cor = read_cor_image(f);
    ASSERT_TRUE(cor.has_value());
    EXPECT_FALSE(cor->pe32plus);
    EXPECT_EQ(cor->flags, COMIMAGE_FLAGS_ILONLY);
    EXPECT_EQ(cor->metadata_version, "v4.0.30319");
    EXPECT_TRUE(is_il_only(cor));
    EXPECT_TRUE(il_only_pe32_runs_64bit(cor));
}

TEST(CorImage, BitnessRules) {
    // /platform:x86 (ILONLY|32BITREQUIRED) and anycpu32bitpreferred
    // (ILONLY|32BITREQUIRED|32BITPREFERRED) run 32-bit; /platform:x64 is
    // PE32+; mixed-mode (no ILONLY) is native.
    EXPECT_FALSE(il_only_pe32_runs_64bit(read_cor_image(il_image(false, 0x3))));
    EXPECT_FALSE(il_only_pe32_runs_64bit(read_cor_image(il_image(false, 0x20003))));
    EXPECT_FALSE(il_only_pe32_runs_64bit(read_cor_image(il_image(true, 0x1))));
    EXPECT_FALSE(il_only_pe32_runs_64bit(read_cor_image(il_image(false, 0x0))));
    EXPECT_FALSE(is_il_only(read_cor_image(il_image(false, 0x10))));
}

TEST(CorImage, RewriteMatchesTheOracleLayout) {
    auto f = il_image(false, COMIMAGE_FLAGS_ILONLY);
    const auto before = f;
    ASSERT_TRUE(rewrite_il_only_pe32_as_pe32plus(f));
    EXPECT_EQ(get16(f, kOpt), 0x20B);                 // mem_magic=0x20b
    EXPECT_EQ(get16(f, kNt + 4), 0x14C);              // Machine unchanged
    EXPECT_EQ(get16(f, kNt + 20), 0xF0);              // SizeOfOptionalHeader 0xe0 -> 0xf0
    EXPECT_EQ(get16(f, kNt + 22), 0x102);
    EXPECT_EQ(get32(f, kOpt + 16), 0x245Eu);          // AddressOfEntryPoint
    EXPECT_EQ(get64(f, kOpt + 24), 0x400000ull);      // ImageBase
    EXPECT_EQ(get32(f, kOpt + 56), 0x8000u);
    EXPECT_EQ(get32(f, kOpt + 60), 0x200u);           // SizeOfHeaders unchanged
    EXPECT_EQ(get16(f, kOpt + 70), 0x8540);
    EXPECT_EQ(get64(f, kOpt + 72), 0x100000ull);      // StackReserve
    EXPECT_EQ(get64(f, kOpt + 80), 0x1000ull);        // StackCommit
    EXPECT_EQ(get64(f, kOpt + 88), 0x100000ull);      // HeapReserve
    EXPECT_EQ(get64(f, kOpt + 96), 0x1000ull);        // HeapCommit
    EXPECT_EQ(get32(f, kOpt + 108), 16u);             // NumberOfRvaAndSizes
    // Data directories and the section table: same bytes, 16 bytes later.
    EXPECT_EQ(0, std::memcmp(f.data() + kOpt + 112, before.data() + kOpt + 96, 16 * 8));
    EXPECT_EQ(0, std::memcmp(f.data() + kOpt + 0xF0, before.data() + kOpt + 0xE0, 3 * 40));
    // old_section_table_bytes_unchanged=0
    EXPECT_NE(0, std::memcmp(f.data() + kOpt + 0xE0, before.data() + kOpt + 0xE0, 40));
    // Section contents are untouched.
    EXPECT_EQ(0, std::memcmp(f.data() + 0x200, before.data() + 0x200, f.size() - 0x200));
    const auto cor = read_cor_image(f);
    ASSERT_TRUE(cor.has_value());
    EXPECT_TRUE(cor->pe32plus);
    EXPECT_EQ(cor->metadata_version, "v4.0.30319");
}

TEST(CorImage, RewriteRefusesWhatTheLoaderDoesNotConvert) {
    for (u32 flags : {0x3u, 0x20003u, 0x0u}) {
        auto f = il_image(false, flags);
        const auto before = f;
        EXPECT_FALSE(rewrite_il_only_pe32_as_pe32plus(f)) << flags;
        EXPECT_EQ(f, before);
    }
    auto plus = il_image(true, 0x1);
    EXPECT_FALSE(rewrite_il_only_pe32_as_pe32plus(plus));
    // No room for 16 more header bytes before SizeOfHeaders.
    auto tight = il_image(false, 0x1);
    put32(tight, kOpt + 60, static_cast<u32>(kOpt + 0xE0 + 3 * 40 + 8));
    const auto before = tight;
    EXPECT_FALSE(rewrite_il_only_pe32_as_pe32plus(tight));
    EXPECT_EQ(tight, before);
}

}  // namespace
