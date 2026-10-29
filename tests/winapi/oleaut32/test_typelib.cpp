// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_typelib.cpp
/// @brief Unit tests for the MSFT type-library reader behind LoadTypeLib.
///
/// The reader is what decides whether a .tlb can be trusted, so these tests
/// pin both halves of that contract: a well-formed image yields exactly the
/// attributes stored in it, and every way an image can be wrong is reported
/// as a distinct failure rather than as plausible-looking data.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/com/oleaut32/typelib.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr usize HEADER_SIZE   = 0x54;
constexpr usize SEGMENT_COUNT = 15;
constexpr usize TYPE_INFO_COUNT = 0; // Metadata-only library, no type records.

// Layout of the synthetic image the tests build.
constexpr usize SEGMENT_DIR   = HEADER_SIZE + TYPE_INFO_COUNT * 4;
constexpr usize GUID_TAB_OFF  = SEGMENT_DIR + SEGMENT_COUNT * 16;
constexpr usize GUID_TAB_LEN  = 24;
constexpr usize NAME_TAB_OFF  = GUID_TAB_OFF + GUID_TAB_LEN;
constexpr usize NAME_TAB_LEN  = 32;
constexpr usize STR_TAB_OFF   = NAME_TAB_OFF + NAME_TAB_LEN;
constexpr usize STR_TAB_LEN   = 32;
constexpr usize IMAGE_SIZE    = STR_TAB_OFF + STR_TAB_LEN;

constexpr usize SEGMENT_GUID   = 5;
constexpr usize SEGMENT_NAME   = 7;
constexpr usize SEGMENT_STRING = 8;

// {94636247-BC39-4B8B-A728-2D1FBEBFA76A} as it is stored on disk.
constexpr std::array<u8, 16> kLibId{
    0x47, 0x62, 0x63, 0x94, 0x39, 0xBC, 0x8B, 0x4B,
    0xA7, 0x28, 0x2D, 0x1F, 0xBE, 0xBF, 0xA7, 0x6A};

void put_u32(std::vector<u8>& image, usize offset, u32 value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

void put_u16(std::vector<u8>& image, usize offset, u16 value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

void put_segment(std::vector<u8>& image, usize index, u32 offset, u32 length) {
    const usize entry = SEGMENT_DIR + index * 16;
    put_u32(image, entry, offset);
    put_u32(image, entry + 4, length);
}

/// A well-formed MSFT image declaring version 2.3, LCID 0x409, SYS_WIN32,
/// library flags 0x2, name "ProEngine" and help string "Setup Kernel".
std::vector<u8> make_typelib() {
    std::vector<u8> image(IMAGE_SIZE, 0);

    put_u32(image, 0x00, 0x5446534Du);      // 'MSFT'
    put_u32(image, 0x04, 0x00010002u);      // format version
    put_u32(image, 0x08, 0);                // posguid: start of the GUID table
    put_u32(image, 0x0C, 0x409);            // lcid
    put_u32(image, 0x14, 0x41);             // varflags: SYS_WIN32 in the low nibble
    put_u32(image, 0x18, 0x00030002u);      // version: major 2, minor 3
    put_u32(image, 0x1C, 0x0002);           // flags -> wLibFlags
    put_u32(image, 0x20, TYPE_INFO_COUNT);
    put_u32(image, 0x24, 0);                // helpstring offset
    put_u32(image, 0x38, 0);                // NameOffset

    put_segment(image, SEGMENT_GUID, GUID_TAB_OFF, GUID_TAB_LEN);
    put_segment(image, SEGMENT_NAME, NAME_TAB_OFF, NAME_TAB_LEN);
    put_segment(image, SEGMENT_STRING, STR_TAB_OFF, STR_TAB_LEN);

    std::memcpy(image.data() + GUID_TAB_OFF, kLibId.data(), kLibId.size());

    // Name entry: { hreftype, next_hash, namelen } then the ANSI characters.
    put_u32(image, NAME_TAB_OFF, 0);
    put_u32(image, NAME_TAB_OFF + 4, 0xFFFFFFFFu);
    put_u32(image, NAME_TAB_OFF + 8, 9);
    std::memcpy(image.data() + NAME_TAB_OFF + 12, "ProEngine", 9);

    // String entry: a 16-bit length then the characters.
    put_u16(image, STR_TAB_OFF, 12);
    std::memcpy(image.data() + STR_TAB_OFF + 2, "Setup Kernel", 12);

    return image;
}

std::span<const u8> as_span(const std::vector<u8>& image) {
    return std::span<const u8>(image.data(), image.size());
}

} // namespace

// =============================================================================
// Successful parse
// =============================================================================

TEST(TypeLibParse, ReadsLibraryAttributesFromHeader) {
    const auto image = make_typelib();
    const auto result = parse_typelib(as_span(image));

    ASSERT_TRUE(result.ok());
    const auto& attributes = *result.attributes;

    EXPECT_EQ(format_guid_braced(attributes.libid),
              "{94636247-BC39-4B8B-A728-2D1FBEBFA76A}");
    EXPECT_EQ(attributes.lcid, 0x409u);
    EXPECT_EQ(attributes.syskind, SysKind::Win32);
    EXPECT_EQ(attributes.major, 2);
    EXPECT_EQ(attributes.minor, 3);
    EXPECT_EQ(attributes.lib_flags, 2);
    EXPECT_EQ(attributes.type_info_count, TYPE_INFO_COUNT);
    EXPECT_EQ(attributes.name, "ProEngine");
    EXPECT_EQ(attributes.doc, "Setup Kernel");
}

TEST(TypeLibParse, MissingNameAndHelpStringAreNotErrors) {
    // -1 in either offset means the library simply did not supply the field.
    auto image = make_typelib();
    put_u32(image, 0x24, 0xFFFFFFFFu); // helpstring
    put_u32(image, 0x38, 0xFFFFFFFFu); // NameOffset

    const auto result = parse_typelib(as_span(image));
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.attributes->name.empty());
    EXPECT_TRUE(result.attributes->doc.empty());
    // The LIBID still has to come through — it is the part callers rely on.
    EXPECT_EQ(format_guid_braced(result.attributes->libid),
              "{94636247-BC39-4B8B-A728-2D1FBEBFA76A}");
}

TEST(TypeLibParse, Win64SysKindIsReported) {
    auto image = make_typelib();
    put_u32(image, 0x14, 0x43); // SYS_WIN64 in the low nibble

    const auto result = parse_typelib(as_span(image));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.attributes->syskind, SysKind::Win64);
}

TEST(TypeLibParse, IsProBeNtResolvesKnownInterfaceGuid) {
    // Oracle: IsProBENT.tlb from the RCT3 InstallShield runtime. GUID
    // {F4817E4B-04B6-11D3-8862-00C04F72F303} is ISetupServiceProvider at
    // typeinfo index 50 (hreftype 5000 / 0x64).
    const char* path = "/tmp/rct3-analysis/IsProBENT.tlb";
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        GTEST_SKIP() << "IsProBENT.tlb not present at " << path;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    ASSERT_GT(size, 0);
    std::vector<u8> image(static_cast<usize>(size));
    ASSERT_EQ(std::fread(image.data(), 1, image.size(), f), image.size());
    std::fclose(f);

    const auto result = parse_typelib(as_span(image));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.attributes->type_info_count, 162u);
    EXPECT_EQ(result.typeinfos.size(), 162u);
    EXPECT_EQ(format_guid_braced(result.attributes->libid),
              "{94636247-BC39-4B8B-A728-2D1FBEBFA76A}");

    const TypeLibGuid iid{
        0x4B, 0x7E, 0x81, 0xF4, 0xB6, 0x04, 0xD3, 0x11,
        0x88, 0x62, 0x00, 0xC0, 0x4F, 0x72, 0xF3, 0x03};
    auto index = result.find_typeinfo_by_guid(iid);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index.value(), 50u);
    EXPECT_EQ(result.typeinfos[50].name, "ISetupServiceProvider");
    EXPECT_EQ(result.typeinfos[50].typekind & 0xFu, 3u); // TKIND_INTERFACE

    // ISetupComponents — the GUID Setup.dll looks up after LoadRegTypeLib.
    const TypeLibGuid components{
        0xC5, 0x4E, 0x81, 0x91, 0xF0, 0xB5, 0xD2, 0x11,
        0x80, 0xB9, 0x00, 0x10, 0x4B, 0x1F, 0x6C, 0xEA};
    auto comp = result.find_typeinfo_by_guid(components);
    ASSERT_TRUE(comp.has_value());
    EXPECT_EQ(comp.value(), 31u);
    EXPECT_EQ(result.typeinfos[31].name, "ISetupComponents");
    EXPECT_EQ(result.typeinfos[31].typekind & 0xFu, 4u); // TKIND_DISPATCH
    EXPECT_EQ(result.typeinfos[31].c_funcs, 3u);
    EXPECT_EQ(result.typeinfos[31].c_vars, 0u);
    EXPECT_EQ(result.typeinfos[31].c_impl_types, 1u);
    ASSERT_EQ(result.typeinfos[31].funcs.size(), 3u);
    EXPECT_EQ(result.typeinfos[31].funcs[0].name, "Count");
    EXPECT_EQ(result.typeinfos[31].funcs[0].memid, 1);
    EXPECT_EQ(result.typeinfos[31].funcs[1].name, "Item");
    EXPECT_EQ(result.typeinfos[31].funcs[1].memid, 0);
    EXPECT_EQ(result.typeinfos[31].funcs[2].name, "_NewEnum");
    EXPECT_EQ(result.typeinfos[31].funcs[2].memid, -4);
    EXPECT_EQ(result.typeinfos[31].funcs[2].o_vft, 0x24u);
    EXPECT_EQ(result.typeinfos[31].funcs[2].invkind, 2u); // PROPERTYGET
}

// =============================================================================
// Honest failures
// =============================================================================

TEST(TypeLibParse, RejectsNonTypeLibrary) {
    // Bare parse_typelib rejects PE bytes; LoadTypeLib separately extracts
    // TYPELIB resources via extract_typelib_resource_from_pe.
    std::vector<u8> pe(512, 0);
    pe[0] = 'M'; pe[1] = 'Z';

    const auto result = parse_typelib(as_span(pe));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::NotTypeLib);
}

TEST(TypeLibParse, ExtractsTypeLibResourceFromIsSetupDll) {
    // Real InstallShield engine DLL embeds MSFT typelib as TYPELIB resource.
    const char* path =
        std::getenv("VITREO_TEST_ISSETUP_DLL");
    std::filesystem::path dll =
        path ? std::filesystem::path(path)
             : std::filesystem::path(
                   std::getenv("HOME")
                       ? std::string(std::getenv("HOME")) +
                             "/.vitreo/capsules/Sims/drives/c/InstallMedia/"
                             "Sims3/ISSetup.dll"
                       : "");
    if (dll.empty() || !std::filesystem::exists(dll)) {
        GTEST_SKIP() << "ISSetup.dll not available for TYPELIB extract test";
    }
    std::ifstream in(dll, std::ios::binary);
    ASSERT_TRUE(in);
    std::vector<u8> pe((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    ASSERT_GT(pe.size(), 0x1000u);

    auto blob = extract_typelib_resource_from_pe(as_span(pe));
    ASSERT_GE(blob.size(), 4u);
    EXPECT_EQ(blob[0], 'M');
    EXPECT_EQ(blob[1], 'S');
    EXPECT_EQ(blob[2], 'F');
    EXPECT_EQ(blob[3], 'T');

    const auto parsed = parse_typelib(as_span(blob));
    ASSERT_TRUE(parsed.ok()) << typelib_parse_error_name(parsed.error);
    EXPECT_GT(parsed.attributes->type_info_count, 0u);
}

TEST(TypeLibParse, RejectsEmptyImage) {
    const std::vector<u8> empty;
    const auto result = parse_typelib(as_span(empty));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::NotTypeLib);
}

TEST(TypeLibParse, RecognisesButRejectsSltgContainer) {
    std::vector<u8> sltg(512, 0);
    put_u32(sltg, 0, 0x47544C53u); // 'SLTG'

    const auto result = parse_typelib(as_span(sltg));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::UnsupportedFormat);
}

TEST(TypeLibParse, RejectsHeaderTruncatedBeforeSegmentDirectory) {
    auto image = make_typelib();
    image.resize(SEGMENT_DIR + 16); // one segment entry, not fifteen

    const auto result = parse_typelib(as_span(image));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::Truncated);
}

TEST(TypeLibParse, RejectsGuidOffsetOutsideItsSegment) {
    // A GUID offset past the end of the GUID table would otherwise read
    // whatever bytes follow and report them as the LIBID.
    auto image = make_typelib();
    put_u32(image, 0x08, static_cast<u32>(GUID_TAB_LEN));

    const auto result = parse_typelib(as_span(image));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::Truncated);
}

TEST(TypeLibParse, RejectsGuidSegmentRunningPastEndOfImage) {
    auto image = make_typelib();
    put_segment(image, SEGMENT_GUID, static_cast<u32>(GUID_TAB_OFF),
                static_cast<u32>(image.size()));

    const auto result = parse_typelib(as_span(image));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::Truncated);
}

TEST(TypeLibParse, RejectsAbsurdTypeInfoCount) {
    // nrtypeinfos sizes the offset array, so a bogus value must not be used
    // to index far outside the image.
    auto image = make_typelib();
    put_u32(image, 0x20, 0x40000000u);

    const auto result = parse_typelib(as_span(image));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, TypeLibParseError::Truncated);
}

TEST(TypeLibParse, OverlongNameLengthYieldsNoNameRatherThanGarbage) {
    // The name entry claims more characters than its segment holds; the parse
    // still succeeds (the name is optional) but must not read past the table.
    auto image = make_typelib();
    put_u32(image, NAME_TAB_OFF + 8, 0xFFu);

    const auto result = parse_typelib(as_span(image));
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.attributes->name.empty());
}

// =============================================================================
// GUID formatting
// =============================================================================

TEST(TypeLibGuidFormat, MatchesRegistryCanonicalForm) {
    const TypeLibGuid nil{};
    EXPECT_EQ(format_guid_braced(nil), "{00000000-0000-0000-0000-000000000000}");

    const TypeLibGuid all_ff{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    EXPECT_EQ(format_guid_braced(all_ff), "{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}");
}

namespace {
std::filesystem::path native_fixture_root() {
    return std::filesystem::absolute(__FILE__).parent_path().parent_path()
        .parent_path().parent_path();
}
std::vector<u8> native_fixture(unsigned variant, unsigned bits) {
    auto path = native_fixture_root() / "tests/fixtures/typelib-native" /
        ("vitreo-layout-" + std::to_string(variant) + "-" + std::to_string(bits) + ".tlb");
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), {}};
}
}

TEST(TypeLibParse, MatchesNativeWindowsDescriptionsForBothFileArchitectures) {
    unsigned checked = 0;
    for (unsigned bits : {32u, 64u}) {
        std::ifstream oracle(native_fixture_root() / "tests/oracle_data" /
            (bits == 32 ? "typelib_fixture_probe.x86.expected" :
                          "typelib_fixture_probe.x64.expected"));
        ASSERT_TRUE(oracle.is_open());
        std::string line;
        TypeLibParseResult parsed;
        unsigned type_index = 0, function_index = 0;
        bool selected = false;
        while (std::getline(oracle, line)) {
            unsigned variant, target, reader;
            if (std::sscanf(line.c_str(), "fixture=%u-%u reader=%u", &variant, &target, &reader) == 3) {
                selected = reader == bits;
                if (selected) {
                    auto bytes = native_fixture(variant, target);
                    ASSERT_FALSE(bytes.empty());
                    parsed = parse_typelib(bytes);
                    ASSERT_TRUE(parsed.ok()) << variant << ": " << typelib_parse_error_name(parsed.error);
                    EXPECT_EQ(parsed.typeinfos.size(), parsed.attributes->type_info_count);
                    ++checked;
                }
                continue;
            }
            if (!selected) continue;
            SCOPED_TRACE(line);
            unsigned kind, major, minor, locale, count;
            if (std::sscanf(line.c_str(), "library syskind=%u version=%u.%u locale=%x count=%u",
                            &kind, &major, &minor, &locale, &count) == 5) {
                EXPECT_EQ(static_cast<unsigned>(parsed.attributes->syskind), kind);
                EXPECT_EQ(parsed.attributes->major, major);
                EXPECT_EQ(parsed.attributes->minor, minor);
                EXPECT_EQ(parsed.attributes->lcid, locale);
                EXPECT_EQ(parsed.typeinfos.size(), count);
                continue;
            }
            unsigned flags, instance, funcs, vars, impl, vft;
            if (std::sscanf(line.c_str(), "type index=%u kind=%u flags=%x version=%u.%u instance=%u funcs=%u vars=%u impl=%u vft=%u",
                            &type_index, &kind, &flags, &major, &minor, &instance,
                            &funcs, &vars, &impl, &vft) == 10) {
                ASSERT_LT(type_index, parsed.typeinfos.size());
                const auto& info = parsed.typeinfos[type_index];
                EXPECT_EQ(info.typekind & 15u, kind);
                EXPECT_EQ(info.type_flags, flags);
                EXPECT_EQ(info.major, major); EXPECT_EQ(info.minor, minor);
                EXPECT_EQ(info.reader_instance_size(bits / 8), instance);
                EXPECT_EQ(info.c_funcs, funcs); EXPECT_EQ(info.c_vars, vars);
                EXPECT_EQ(info.c_impl_types, impl); EXPECT_EQ(info.impl_hrefs.size(), impl);
                EXPECT_EQ(reader_vtable_bytes(info.cb_size_vft, parsed.attributes->syskind, bits / 8), vft);
                continue;
            }
            unsigned memid, invoke, call, params, optional, result;
            if (std::sscanf(line.c_str(), "func index=%u memid=%x kind=%u invoke=%u call=%u vft=%u params=%u optional=%u result=%u",
                            &function_index, &memid, &kind, &invoke, &call, &vft,
                            &params, &optional, &result) == 9) {
                ASSERT_LT(function_index, parsed.typeinfos[type_index].funcs.size());
                const auto& function = parsed.typeinfos[type_index].funcs[function_index];
                EXPECT_EQ(static_cast<u32>(function.memid), memid);
                EXPECT_EQ(function.funckind, kind); EXPECT_EQ(function.invkind, invoke);
                EXPECT_EQ(function.callconv, call);
                EXPECT_EQ(function.optional_params, optional);
                ASSERT_FALSE(function.result_description.tags.empty());
                EXPECT_EQ(function.result_description.tags.front(), result);
                EXPECT_EQ(reader_vtable_bytes(function.o_vft, parsed.attributes->syskind, bits / 8), vft); EXPECT_EQ(function.c_params, params);
                continue;
            }
            unsigned parameter_index, vartype;
            if (std::sscanf(line.c_str(), "param index=%u flags=%x type=%u", &parameter_index, &flags, &vartype) == 3) {
                const auto& function = parsed.typeinfos[type_index].funcs[function_index];
                ASSERT_LT(parameter_index, function.param_vts.size());
                EXPECT_EQ(function.param_flags[parameter_index], flags);
                ASSERT_EQ(function.param_descriptions.size(), function.param_vts.size());
                const auto& description = function.param_descriptions[parameter_index];
                ASSERT_FALSE(description.tags.empty());
                EXPECT_EQ(description.tags.front(), vartype);
                if (vartype == 26 || vartype == 27) {
                    ASSERT_EQ(description.tags.size(), 2u);
                    EXPECT_EQ(description.tags.back(), std::stoul(line.substr(line.find('(') + 1)));
                }

                // FuncSummary exposes the value VARTYPE rather than TYPEDESC.
                // Pointer and array descriptors come directly from native output.
                if (vartype == 26 || vartype == 27) {
                    auto paren = line.find('(');
                    ASSERT_NE(paren, std::string::npos);
                    unsigned element = static_cast<unsigned>(std::stoul(line.substr(paren + 1)));
                    vartype = vartype == 27 ? (element | 0x2000) : element;
                }
                if (vartype == 22 || vartype == 29) vartype = 3; // INT / fixture enum
                if (vartype == 23) vartype = 19; // UINT
                if (vartype == 25) vartype = 10; // HRESULT
                EXPECT_EQ(function.param_vts[parameter_index], vartype);
                if (flags & 8) EXPECT_EQ(function.retval_vt, vartype);
            }
        }
    }
    EXPECT_EQ(checked, 40u);
}

TEST(TypeLibParse, RejectsBrokenNativeTypeAndDescriptorLinks) {
    auto original = native_fixture(0, 32);
    ASSERT_FALSE(original.empty());
    auto read = [](const std::vector<u8>& data, usize at) {
        u32 word; std::memcpy(&word, data.data() + at, 4); return word;
    };
    const usize directory = 0x58; // native variant 0: one type, no help DLL
    const usize type = read(original, directory);
    const usize member = read(original, type + 4);
    const usize descriptors = read(original, directory + 9 * 16);
    for (usize link : {usize(0x54), type + 4, member + 4, descriptors + 4}) {
        auto malformed = original;
        put_u32(malformed, link, link == descriptors + 4 ? 0u : 0x7fffffffu);
        EXPECT_FALSE(parse_typelib(malformed).ok()) << link;
    }
}
