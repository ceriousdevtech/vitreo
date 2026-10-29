// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "capsule/vfs/host_filesystem.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, FactoryCreatesGdiInteropWithCompleteVtable) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr factory_vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(factory_vtable + 17 * 8).value(),
              GET_GDI_INTEROP_THUNK);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_GetGdiInterop(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr interop =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    ASSERT_NE(interop, 0u);
    const GuestAddr vtable = memory.read_u64(interop).value_or(0);
    ASSERT_NE(vtable, 0u);
    EXPECT_EQ(memory.read_u64(vtable + 0 * 8).value(), QUERY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 1 * 8).value(), ADDREF_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 2 * 8).value(), RELEASE_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(),
              GDI_CREATE_FONT_FROM_LOGFONT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 4 * 8).value(),
              GDI_CONVERT_FONT_TO_LOGFONT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 5 * 8).value(),
              GDI_CONVERT_FONT_FACE_TO_LOGFONT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 6 * 8).value(),
              GDI_CREATE_FONT_FACE_FROM_HDC_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(),
              NOTIMPL_THUNK_BASE + 5 * 0x10);
}

TEST_F(DWriteManagerTest, GdiInteropCreatesFaceForSelectedHdcFont) {
    const GuestAddr factory = create_factory();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_GetGdiInterop(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr interop =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    ASSERT_NE(interop, 0u);

    const Handle hdc = gdi.get_dc(0);
    ASSERT_NE(hdc, Handle{0});
    cpu.gpr.rcx = interop;
    cpu.gpr.rdx = hdc;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_GdiCreateFontFaceFromHdc(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr face =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    ASSERT_NE(face, 0u);
    const GuestAddr vtable = memory.read_u64(face).value_or(0);
    EXPECT_EQ(memory.read_u64(vtable + 9 * 8).value(),
              FACE_GET_GLYPH_COUNT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 10 * 8).value(),
              FACE_GET_DESIGN_GLYPH_METRICS_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 17 * 8).value(),
              FACE_GET_GDI_COMPATIBLE_GLYPH_METRICS_THUNK);
}

namespace {

// LOGFONTW (wingdi.h): the same 92 bytes on x86 and x64.
struct GuestLogfont {
    i32 height;
    i32 width;
    i32 escapement;
    i32 orientation;
    i32 weight;
    u8 italic;
    u8 underline;
    u8 strike_out;
    u8 char_set;
    u8 out_precision;
    u8 clip_precision;
    u8 quality;
    u8 pitch_and_family;
    char16_t face_name[32];
};
static_assert(sizeof(GuestLogfont) == 92);

GuestLogfont make_logfont(std::u16string_view face, i32 weight, u8 italic,
                          u8 char_set = 1) {
    GuestLogfont logfont{};
    logfont.weight = weight;
    logfont.italic = italic;
    logfont.char_set = char_set;
    std::copy(face.begin(), face.end(), logfont.face_name);
    return logfont;
}

GuestLogfont read_logfont(GuestMemory& memory, GuestAddr address) {
    GuestLogfont logfont{};
    EXPECT_TRUE(memory.read_bytes(address, reinterpret_cast<Byte*>(&logfont),
                                  sizeof(logfont)).is_ok());
    return logfont;
}

std::u16string read_guest_string(GuestMemory& memory, GuestAddr address) {
    std::u16string text;
    for (GuestAddr at = address;; at += 2) {
        const u16 unit = memory.read_u16(at).value_or(0);
        if (unit == 0) return text;
        text.push_back(static_cast<char16_t>(unit));
    }
}

// What ConvertFontToLOGFONT / ConvertFontFaceToLOGFONT wrote on Windows
// (dwrite_gdi_interop_probe): every field zero but these five, and nothing
// after the face name's terminator.
void expect_gdi_logfont(const GuestLogfont& logfont, i32 weight, u8 italic,
                        std::u16string_view face) {
    GuestLogfont expected{};
    expected.weight = weight;
    expected.italic = italic;
    expected.char_set = 1;       // DEFAULT_CHARSET
    expected.out_precision = 8;  // OUT_OUTLINE_PRECIS
    std::copy(face.begin(), face.end(), expected.face_name);
    EXPECT_EQ(std::memcmp(&logfont, &expected, sizeof(expected)), 0)
        << "weight " << logfont.weight << " italic " << u32{logfont.italic}
        << " charset " << u32{logfont.char_set} << " precision "
        << u32{logfont.out_precision};
}

} // namespace

// Steam's vgui2 Win32Font: CreateFontFromLOGFONT -> GetFontFamily ->
// GetFamilyNames -> CreateFontFace -> GetStringLength(0), all as vtable calls.
TEST_F(DWriteManagerTest, GdiInteropCreateFontFromLogfontServesVguiFontChain) {
    constexpr GuestAddr LOGFONT = DATA_BASE + 0x400;
    constexpr GuestAddr OUT = DATA_BASE + 0x600;
    constexpr GuestAddr LENGTH = DATA_BASE + 0x608;
    constexpr GuestAddr TEXT = DATA_BASE + 0x700;
    const GuestAddr interop = create_gdi_interop();
    ASSERT_NE(interop, 0u);
    const GuestLogfont logfont = make_logfont(u"Tahoma", 400, 0);
    ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&logfont),
                                   sizeof(logfont)).is_ok());

    ASSERT_EQ(call_vtable(interop, 3, LOGFONT, OUT), 0u);
    const GuestAddr font = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(font, 0u);
    EXPECT_EQ(call_vtable(font, 4), 400u);  // GetWeight
    EXPECT_EQ(call_vtable(font, 6), 0u);    // GetStyle
    EXPECT_EQ(call_vtable(font, 10), 0u);   // GetSimulations

    ASSERT_EQ(call_vtable(font, 3, OUT), 0u);  // GetFontFamily
    const GuestAddr family = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(family, 0u);
    ASSERT_EQ(call_vtable(family, 6, OUT), 0u);  // GetFamilyNames
    const GuestAddr names = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(names, 0u);
    ASSERT_EQ(call_vtable(names, 7, 0, LENGTH), 0u);  // GetStringLength(0)
    EXPECT_EQ(memory.read_u32(LENGTH).value_or(0), 6u);
    ASSERT_EQ(call_vtable(names, 8, 0, TEXT, 32), 0u);  // GetString
    EXPECT_EQ(read_guest_string(memory, TEXT), u"Tahoma");

    ASSERT_EQ(call_vtable(font, 13, OUT), 0u);  // CreateFontFace
    const GuestAddr face = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(face, 0u);
    EXPECT_EQ(call_vtable(face, 6), 0u);  // GetSimulations
}

// Expected values: tools/oracle_probe/dwrite_gdi_interop_probe.expected, for
// the families macOS ships with the same faces Windows 11 has (Arial, Tahoma,
// Microsoft Sans Serif) and Vitreo's Segoe UI (Regular/Bold/Italic).
TEST_F(DWriteManagerTest, GdiInteropCreateFontFromLogfontMatchesWindowsSelection) {
    constexpr GuestAddr LOGFONT = DATA_BASE + 0x400;
    constexpr GuestAddr CONVERTED = DATA_BASE + 0x480;
    constexpr GuestAddr IS_SYSTEM = DATA_BASE + 0x4F0;
    constexpr GuestAddr OUT = DATA_BASE + 0x600;
    constexpr GuestAddr TEXT = DATA_BASE + 0x700;
    struct Case {
        std::u16string_view face;
        i32 weight;
        u8 italic;
        u8 char_set;
        u32 font_weight;
        u32 style;
        u32 simulations;
        std::u16string_view face_name;
        i32 logfont_weight;
        std::u16string_view logfont_face;
    };
    const Case cases[]{
        {u"Arial", 0, 0, 1, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"arial", 400, 0, 1, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"ARIAL", 400, 0, 1, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"Arial", 400, 0, 2, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"Arial", 400, 0, 128, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"Arial", 400, 0, 255, 400, 0, 0, u"Regular", 400, u"Arial"},
        {u"Arial", 100, 1, 1, 400, 2, 0, u"Italic", 400, u"Arial"},
        {u"Arial", 400, 255, 1, 400, 2, 0, u"Italic", 400, u"Arial"},
        {u"Arial", 700, 1, 1, 700, 2, 0, u"Bold Italic", 700, u"Arial"},
        {u"Arial", 900, 0, 1, 700, 0, 1, u"Bold", 851, u"Arial"},
        {u"Arial", 900, 1, 1, 700, 2, 1, u"Italic Bold", 851, u"Arial"},
        {u"Tahoma", 400, 1, 1, 400, 1, 2, u"Oblique", 400, u"Tahoma"},
        {u"Tahoma", 700, 1, 1, 700, 1, 2, u"Bold Oblique", 700, u"Tahoma"},
        {u"Tahoma", 900, 1, 1, 700, 1, 3, u"Bold Oblique", 851, u"Tahoma"},
        {u"Microsoft Sans Serif", 700, 1, 1, 700, 1, 3, u"Bold Oblique", 551,
         u"Microsoft Sans Serif"},
        {u"Segoe UI", -1, 0, 1, 400, 0, 0, u"Regular", 400, u"Segoe UI"},
        {u"Segoe UI", 550, 0, 1, 400, 0, 0, u"Regular", 400, u"Segoe UI"},
        {u"Segoe UI", 551, 0, 1, 700, 0, 1, u"Bold", 551, u"Segoe UI"},
        {u"Segoe UI", 649, 0, 1, 700, 0, 0, u"Bold", 700, u"Segoe UI"},
        {u"Segoe UI", 850, 0, 1, 700, 0, 0, u"Bold", 700, u"Segoe UI"},
        {u"Segoe UI", 851, 0, 1, 700, 0, 1, u"Bold", 851, u"Segoe UI"},
        {u"Segoe UI", 1001, 0, 1, 700, 0, 1, u"Bold", 851, u"Segoe UI"},
        // A full name pins the face; weight and italic only simulate.
        {u"Arial Bold", 100, 0, 1, 700, 0, 0, u"Bold", 700, u"Arial"},
        {u"Arial Bold", 900, 0, 1, 700, 0, 1, u"Bold", 851, u"Arial"},
        {u"Arial Bold", 400, 1, 1, 700, 1, 2, u"Bold Oblique", 700, u"Arial"},
        {u"Arial Italic", 700, 0, 1, 700, 2, 1, u"Italic Bold", 551, u"Arial"},
        {u"Arial Bold Italic", 400, 0, 1, 700, 2, 0, u"Bold Italic", 700, u"Arial"},
        {u"Tahoma Bold", 400, 0, 1, 700, 0, 0, u"Bold", 700, u"Tahoma"},
    };
    const GuestAddr interop = create_gdi_interop();
    ASSERT_NE(interop, 0u);
    for (const Case& test : cases) {
        SCOPED_TRACE(std::string(test.face.begin(), test.face.end()) + " w=" +
                     std::to_string(test.weight) + " i=" +
                     std::to_string(test.italic) + " cs=" +
                     std::to_string(test.char_set));
        GuestLogfont logfont = make_logfont(test.face, test.weight, test.italic,
                                            test.char_set);
        ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&logfont),
                                       sizeof(logfont)).is_ok());
        ASSERT_EQ(call_vtable(interop, 3, LOGFONT, OUT), 0u);
        const GuestAddr font = memory.read_u64(OUT).value_or(0);
        ASSERT_NE(font, 0u);
        EXPECT_EQ(call_vtable(font, 4), test.font_weight);
        EXPECT_EQ(call_vtable(font, 6), test.style);
        EXPECT_EQ(call_vtable(font, 10), test.simulations);
        ASSERT_EQ(call_vtable(font, 8, OUT), 0u);  // GetFaceNames
        const GuestAddr face_names = memory.read_u64(OUT).value_or(0);
        ASSERT_EQ(call_vtable(face_names, 8, 0, TEXT, 64), 0u);
        EXPECT_EQ(read_guest_string(memory, TEXT), test.face_name);

        std::vector<u8> filler(sizeof(GuestLogfont), 0xCC);
        ASSERT_TRUE(memory.write_bytes(CONVERTED, filler.data(), filler.size()).is_ok());
        ASSERT_TRUE(memory.write_u32(IS_SYSTEM, 0xCCCCCCCCu).is_ok());
        ASSERT_EQ(call_vtable(interop, 4, font, CONVERTED, IS_SYSTEM), 0u);
        expect_gdi_logfont(read_logfont(memory, CONVERTED), test.logfont_weight,
                           test.style != 0 ? 1 : 0, test.logfont_face);
        EXPECT_EQ(memory.read_u32(IS_SYSTEM).value_or(0), 1u);

        ASSERT_EQ(call_vtable(font, 13, OUT), 0u);  // CreateFontFace
        const GuestAddr face = memory.read_u64(OUT).value_or(0);
        ASSERT_NE(face, 0u);
        EXPECT_EQ(call_vtable(face, 6), test.simulations);
        EXPECT_EQ(call_vtable(face, 37), test.font_weight);  // IDWriteFontFace3
        EXPECT_EQ(call_vtable(face, 39), test.style);
        ASSERT_TRUE(memory.write_bytes(CONVERTED, filler.data(), filler.size()).is_ok());
        ASSERT_EQ(call_vtable(interop, 5, face, CONVERTED), 0u);
        expect_gdi_logfont(read_logfont(memory, CONVERTED), test.logfont_weight,
                           test.style != 0 ? 1 : 0, test.logfont_face);
        (void)call_vtable(face, 2);
        (void)call_vtable(face_names, 2);
        (void)call_vtable(font, 2);
    }
}

TEST_F(DWriteManagerTest, GdiInteropCreateFontFromLogfontRejectsWhatWindowsCannotResolve) {
    constexpr GuestAddr LOGFONT = DATA_BASE + 0x400;
    constexpr GuestAddr OUT = DATA_BASE + 0x600;
    constexpr GuestAddr SENTINEL = 0x1234;
    const GuestAddr interop = create_gdi_interop();
    ASSERT_NE(interop, 0u);
    // dwrite_gdi_interop_probe: no FontSubstitutes alias, raster family,
    // trimming, vertical '@' prefix or empty name resolves.
    const std::u16string_view unresolved[]{
        u"", u"MS Shell Dlg", u"MS Shell Dlg 2", u"Helv", u"MS Sans Serif",
        u"System", u" Arial", u"Arial ", u"@Arial", u"NoSuchFontXYZ",
    };
    for (const auto face : unresolved) {
        SCOPED_TRACE(std::string(face.begin(), face.end()));
        const GuestLogfont logfont = make_logfont(face, 400, 0);
        ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&logfont),
                                       sizeof(logfont)).is_ok());
        ASSERT_TRUE(memory.write_u64(OUT, SENTINEL).is_ok());
        EXPECT_EQ(call_vtable(interop, 3, LOGFONT, OUT), 0x88985002u);  // DWRITE_E_NOFONT
        EXPECT_EQ(memory.read_u64(OUT).value_or(SENTINEL), 0u);
    }
    // A face name filling all 32 WCHARs, with no terminator.
    GuestLogfont unterminated = make_logfont(u"", 400, 0);
    std::fill(std::begin(unterminated.face_name), std::end(unterminated.face_name), u'A');
    ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&unterminated),
                                   sizeof(unterminated)).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT, SENTINEL).is_ok());
    EXPECT_EQ(call_vtable(interop, 3, LOGFONT, OUT), 0x88985002u);
    EXPECT_EQ(memory.read_u64(OUT).value_or(SENTINEL), 0u);

    // A NULL LOGFONT: E_INVALIDARG with *font cleared.
    ASSERT_TRUE(memory.write_u64(OUT, SENTINEL).is_ok());
    EXPECT_EQ(call_vtable(interop, 3, 0, OUT), 0x80070057u);
    EXPECT_EQ(memory.read_u64(OUT).value_or(SENTINEL), 0u);
}

TEST_F(DWriteManagerTest, GdiInteropConvertZeroesLogfontBeforeCheckingTheFont) {
    constexpr GuestAddr CONVERTED = DATA_BASE + 0x480;
    constexpr GuestAddr IS_SYSTEM = DATA_BASE + 0x4F0;
    const GuestAddr interop = create_gdi_interop();
    ASSERT_NE(interop, 0u);
    // dwrite_gdi_interop_probe: a NULL font answers E_INVALIDARG with the
    // LOGFONTW zeroed and *isSystemFont FALSE; a NULL face likewise.
    const std::vector<u8> filler(sizeof(GuestLogfont), 0xCC);
    ASSERT_TRUE(memory.write_bytes(CONVERTED, filler.data(), filler.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(IS_SYSTEM, 0xCCCCCCCCu).is_ok());
    EXPECT_EQ(call_vtable(interop, 4, 0, CONVERTED, IS_SYSTEM), 0x80070057u);
    const GuestLogfont zero{};
    GuestLogfont converted = read_logfont(memory, CONVERTED);
    EXPECT_EQ(std::memcmp(&converted, &zero, sizeof(zero)), 0);
    EXPECT_EQ(memory.read_u32(IS_SYSTEM).value_or(1), 0u);

    ASSERT_TRUE(memory.write_bytes(CONVERTED, filler.data(), filler.size()).is_ok());
    EXPECT_EQ(call_vtable(interop, 5, 0, CONVERTED), 0x80070057u);
    converted = read_logfont(memory, CONVERTED);
    EXPECT_EQ(std::memcmp(&converted, &zero, sizeof(zero)), 0);
}

// The probe's arial.ttf through CreateCustomFontCollection converts to the
// same LOGFONT as the installed Arial, but with isSystemFont FALSE.
TEST_F(DWriteManagerTest, GdiInteropConvertReportsCustomCollectionFontsAsNotSystem) {
    char pattern[] = "/tmp/vitreo-dwinterop-XXXXXX";
    char* dir = ::mkdtemp(pattern);
    ASSERT_NE(dir, nullptr);
    const std::filesystem::path root(dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    std::filesystem::create_directories(root / "Windows/Fonts");
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())));

    constexpr GuestAddr LOGFONT = DATA_BASE + 0x400;
    constexpr GuestAddr CONVERTED = DATA_BASE + 0x480;
    constexpr GuestAddr IS_SYSTEM = DATA_BASE + 0x4F0;
    constexpr GuestAddr OUT = DATA_BASE + 0x600;
    constexpr GuestAddr COUNT = DATA_BASE + 0x608;
    constexpr GuestAddr FILES = DATA_BASE + 0x610;
    constexpr GuestAddr LOADER = DATA_BASE + 0x800;
    constexpr GuestAddr LOADER_VTABLE = DATA_BASE + 0x820;
    constexpr GuestAddr ENUMERATOR = DATA_BASE + 0x880;
    constexpr GuestAddr ENUMERATOR_VTABLE = DATA_BASE + 0x8A0;
    constexpr GuestAddr GUEST_ADDREF = 0x71000010;
    constexpr GuestAddr GUEST_RELEASE = 0x71000020;
    constexpr GuestAddr CREATE_ENUMERATOR = 0x71000030;
    constexpr GuestAddr MOVE_NEXT = 0x71000040;
    constexpr GuestAddr GET_CURRENT_FILE = 0x71000050;
    ASSERT_TRUE(memory.write_u64(LOADER, LOADER_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(ENUMERATOR, ENUMERATOR_VTABLE).is_ok());
    for (const GuestAddr vtable : {LOADER_VTABLE, ENUMERATOR_VTABLE}) {
        ASSERT_TRUE(memory.write_u64(vtable + 1 * 8, GUEST_ADDREF).is_ok());
        ASSERT_TRUE(memory.write_u64(vtable + 2 * 8, GUEST_RELEASE).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(LOADER_VTABLE + 3 * 8, CREATE_ENUMERATOR).is_ok());
    ASSERT_TRUE(memory.write_u64(ENUMERATOR_VTABLE + 3 * 8, MOVE_NEXT).is_ok());
    ASSERT_TRUE(memory.write_u64(ENUMERATOR_VTABLE + 4 * 8, GET_CURRENT_FILE).is_ok());

    // Arial's own bytes, as a font file object (FaceGetFiles publishes them).
    const GuestAddr interop = create_gdi_interop();
    ASSERT_NE(interop, 0u);
    const GuestAddr factory = memory.read_u64(OUTPUT_ADDRESS).value_or(0);
    const GuestLogfont logfont = make_logfont(u"Arial", 400, 0);
    ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&logfont),
                                   sizeof(logfont)).is_ok());
    ASSERT_EQ(call_vtable(interop, 3, LOGFONT, OUT), 0u);
    const GuestAddr system_font = memory.read_u64(OUT).value_or(0);
    ASSERT_EQ(call_vtable(system_font, 13, OUT), 0u);
    const GuestAddr system_face = memory.read_u64(OUT).value_or(0);
    ASSERT_TRUE(memory.write_u32(COUNT, 1).is_ok());
    ASSERT_EQ(call_vtable(system_face, 4, COUNT, FILES), 0u);
    const GuestAddr file = memory.read_u64(FILES).value_or(0);
    ASSERT_NE(file, 0u);

    bool delivered = false;
    manager.set_guest_call([&](GuestAddr function, const u64* args, usize count,
                               bool) -> u64 {
        if (function == GUEST_ADDREF || function == GUEST_RELEASE) return 1;
        if (function == RELEASE_THUNK) return 1;  // the file stays owned here
        if (function == CREATE_ENUMERATOR && count == 5) {
            EXPECT_TRUE(memory.write_u64(args[4], ENUMERATOR).is_ok());
            return 0;
        }
        if (function == MOVE_NEXT && count == 2) {
            EXPECT_TRUE(memory.write_u32(args[1], delivered ? 0u : 1u).is_ok());
            delivered = true;
            return 0;
        }
        if (function == GET_CURRENT_FILE && count == 2) {
            EXPECT_TRUE(memory.write_u64(args[1], file).is_ok());
            return 0;
        }
        ADD_FAILURE() << "unexpected guest call 0x" << std::hex << function;
        return 0x80004005u;
    });
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = LOADER;
    manager.impl_RegisterFontCollectionLoader(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = LOADER;
    cpu.gpr.r8 = LOGFONT;  // any non-empty key
    cpu.gpr.r9 = 4;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, OUT).is_ok());
    manager.impl_CreateCustomFontCollection(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr collection = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(collection, 0u);

    ASSERT_EQ(call_vtable(collection, 4, 0, OUT), 0u);  // GetFontFamily(0)
    const GuestAddr family = memory.read_u64(OUT).value_or(0);
    ASSERT_EQ(call_vtable(family, 5, 0, OUT), 0u);  // GetFont(0)
    const GuestAddr custom_font = memory.read_u64(OUT).value_or(0);
    ASSERT_NE(custom_font, 0u);
    ASSERT_TRUE(memory.write_u32(IS_SYSTEM, 0xCCCCCCCCu).is_ok());
    ASSERT_EQ(call_vtable(interop, 4, custom_font, CONVERTED, IS_SYSTEM), 0u);
    expect_gdi_logfont(read_logfont(memory, CONVERTED), 400, 0, u"Arial");
    EXPECT_EQ(memory.read_u32(IS_SYSTEM).value_or(1), 0u);
}

// x86: stdcall arguments on the stack, 4-byte interface pointers, same
// LOGFONTW.
TEST_F(DWriteManagerTest, GdiInteropLogfontConversionsUseX86StackArguments) {
    DWriteManager manager32{memory, false, &vfs, &gdi};
    manager32.set_method_thunks(method_thunks());
    cpu.mode = cpu::CpuMode::Protected;
    constexpr GuestAddr LOGFONT = DATA_BASE + 0x400;
    constexpr GuestAddr CONVERTED = DATA_BASE + 0x480;
    constexpr GuestAddr IS_SYSTEM = DATA_BASE + 0x4F0;
    constexpr GuestAddr OUT = DATA_BASE + 0x600;
    const GuestAddr esp = STACK_BASE + 0x800;
    cpu.gpr.rsp = esp;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto call = [&](auto impl, std::initializer_list<u32> args) {
        GuestAddr at = esp + 4;
        for (const u32 arg : args) {
            EXPECT_TRUE(memory.write_u32(at, arg).is_ok());
            at += 4;
        }
        (manager32.*impl)(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(call(&DWriteManager::impl_DWriteCreateFactory,
                   {0, static_cast<u32>(IID_ADDRESS), static_cast<u32>(OUT)}), 0u);
    const u32 factory = memory.read_u32(OUT).value_or(0);
    ASSERT_NE(factory, 0u);
    const u32 factory_vtable = memory.read_u32(factory).value_or(0);
    EXPECT_EQ(memory.read_u32(factory_vtable + 17 * 4).value_or(0), GET_GDI_INTEROP_THUNK);
    ASSERT_EQ(call(&DWriteManager::impl_GetGdiInterop, {factory, static_cast<u32>(OUT)}), 0u);
    const u32 interop = memory.read_u32(OUT).value_or(0);
    ASSERT_NE(interop, 0u);
    const u32 vtable = memory.read_u32(interop).value_or(0);
    EXPECT_EQ(memory.read_u32(vtable + 3 * 4).value_or(0), GDI_CREATE_FONT_FROM_LOGFONT_THUNK);
    EXPECT_EQ(memory.read_u32(vtable + 4 * 4).value_or(0), GDI_CONVERT_FONT_TO_LOGFONT_THUNK);
    EXPECT_EQ(memory.read_u32(vtable + 5 * 4).value_or(0),
              GDI_CONVERT_FONT_FACE_TO_LOGFONT_THUNK);

    const GuestLogfont logfont = make_logfont(u"Tahoma", 900, 1);
    ASSERT_TRUE(memory.write_bytes(LOGFONT, reinterpret_cast<const Byte*>(&logfont),
                                   sizeof(logfont)).is_ok());
    ASSERT_EQ(call(&DWriteManager::impl_GdiCreateFontFromLogfont,
                   {interop, static_cast<u32>(LOGFONT), static_cast<u32>(OUT)}), 0u);
    const u32 font = memory.read_u32(OUT).value_or(0);
    ASSERT_NE(font, 0u);
    EXPECT_EQ(call(&DWriteManager::impl_FontGetSimulations, {font}), 3u);
    ASSERT_TRUE(memory.write_u32(IS_SYSTEM, 0xCCCCCCCCu).is_ok());
    ASSERT_EQ(call(&DWriteManager::impl_GdiConvertFontToLogfont,
                   {interop, font, static_cast<u32>(CONVERTED),
                    static_cast<u32>(IS_SYSTEM)}), 0u);
    expect_gdi_logfont(read_logfont(memory, CONVERTED), 851, 1, u"Tahoma");
    EXPECT_EQ(memory.read_u32(IS_SYSTEM).value_or(0), 1u);
    ASSERT_EQ(call(&DWriteManager::impl_FontCreateFontFace, {font, static_cast<u32>(OUT)}), 0u);
    const u32 face = memory.read_u32(OUT).value_or(0);
    ASSERT_NE(face, 0u);
    ASSERT_EQ(call(&DWriteManager::impl_GdiConvertFontFaceToLogfont,
                   {interop, face, static_cast<u32>(CONVERTED)}), 0u);
    expect_gdi_logfont(read_logfont(memory, CONVERTED), 851, 1, u"Tahoma");
}
