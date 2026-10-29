/// @file test_gdi32_text.cpp
/// @brief GDI text contracts measured on Windows 11.
///
/// Expected values are the Windows captures in tools/oracle_probe:
/// s3gdi_text_probe (TEXTMETRIC, extents, widths, faces), s3gdi_otm_probe
/// (OUTLINETEXTMETRIC, face mapping, failure contracts), s3gdi_textdraw_probe
/// (TextOut/ExtTextOut cells) and s3gdi_drawtext_probe (DrawText layout).
/// Metrics are compared only for faces whose files macOS ships in the same
/// version as Windows (Arial, Courier New); ABC splits come from Windows'
/// hinted outlines and are not compared.

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/graphics/dwrite/font_backend.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kMem = 0x10000;
constexpr GuestAddr kStack = 0x30000;
constexpr GuestAddr kRet = 0xDEAD0000;

class GdiTextTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        mgr_.set_guest_memory(&memory_);
        exports_ = make_gdi32_exports(mgr_);
    }

    u64 call(const std::string& name, const std::vector<u64>& args, bool is64 = true) {
        cpu_ = cpu::CpuState{};
        if (is64) {
            cpu_.mode = cpu::CpuMode::Long;
            cpu_.gpr.rsp = kStack;
            cpu_.gpr.rcx = args.size() > 0 ? args[0] : 0;
            cpu_.gpr.rdx = args.size() > 1 ? args[1] : 0;
            cpu_.gpr.r8 = args.size() > 2 ? args[2] : 0;
            cpu_.gpr.r9 = args.size() > 3 ? args[3] : 0;
            (void)memory_.write_u64(kStack, kRet);
            for (usize i = 4; i < args.size(); ++i) (void)memory_.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
        } else {
            cpu_.init_protected_mode();
            cpu_.set_sp(kStack);
            (void)memory_.write_u32(kStack, static_cast<u32>(kRet));
            for (usize i = 0; i < args.size(); ++i) (void)memory_.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i]));
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
                               [&](const auto& e) { return e.name == name; });
        EXPECT_NE(it, exports_.entries.end()) << name;
        if (it == exports_.entries.end()) return 0;
        it->handler(m);
        return is64 ? cpu_.gpr.rax : static_cast<u32>(cpu_.gpr.rax);
    }

    /// user32 DrawText[Ex]W through its marshaller.
    u64 draw_text_export(const std::vector<u64>& args, bool ex) {
        cpu_ = cpu::CpuState{};
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = kStack;
        cpu_.gpr.rcx = args[0];
        cpu_.gpr.rdx = args[1];
        cpu_.gpr.r8 = args[2];
        cpu_.gpr.r9 = args[3];
        (void)memory_.write_u64(kStack, kRet);
        for (usize i = 4; i < args.size(); ++i) (void)memory_.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        PaintingManager::marshal_draw_text(&mgr_, m, true, ex);
        return static_cast<u32>(cpu_.gpr.rax);
    }

    GuestAddr wstr(GuestAddr at, const std::u16string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(at + i * 2, s[i]);
        (void)memory_.write_u16(at + s.size() * 2, 0);
        return at;
    }

    Handle font(const std::u16string& face, i32 height, i32 weight = 0, u8 quality = 0, bool italic = false,
                u8 underline = 0, u8 strike = 0) {
        GdiManager::FontInfo f;
        f.face_name = face;
        f.height = height;
        f.lf_weight = weight;
        f.weight = static_cast<u16>(weight);
        f.quality = quality;
        f.italic = italic;
        f.underline = underline;
        f.strike_out = strike;
        return mgr_.create_font(f);
    }

    /// TEXTMETRICW of the DC's font.
    std::map<std::string, i64> metrics(Handle dc) {
        const GuestAddr tm = kMem + 0x2000;
        EXPECT_EQ(call("GetTextMetricsW", {dc, tm}), 1u);
        auto i32at = [&](u32 o) { return static_cast<i64>(static_cast<i32>(memory_.read_u32(tm + o).value())); };
        auto u16at = [&](u32 o) { return static_cast<i64>(memory_.read_u16(tm + o).value()); };
        auto u8at = [&](u32 o) { return static_cast<i64>(memory_.read_u8(tm + o).value()); };
        return {{"h", i32at(0)},   {"a", i32at(4)},     {"d", i32at(8)},     {"il", i32at(12)},
                {"el", i32at(16)}, {"ave", i32at(20)},  {"max", i32at(24)},  {"wt", i32at(28)},
                {"oh", i32at(32)}, {"ch0", u16at(44)},  {"ch1", u16at(46)},  {"ch2", u16at(48)},
                {"ch3", u16at(50)}, {"it", u8at(52)},   {"ul", u8at(53)},    {"so", u8at(54)},
                {"pf", u8at(55)},  {"cs", u8at(56)}};
    }

    struct Dib {
        Handle dc{0};
        Handle bmp{0};
        GuestAddr bits{0};
        i32 w{0}, h{0};
    };
    Dib make_dib32(i32 w, i32 h) {
        const GuestAddr bmi = kMem + 0x100;
        (void)memory_.write_u32(bmi + 0, 40);
        (void)memory_.write_u32(bmi + 4, static_cast<u32>(w));
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(-h));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        (void)memory_.write_u32(bmi + 16, 0);
        (void)memory_.write_u32(bmi + 32, 0);
        Dib d;
        d.bmp = mgr_.create_dib_section(0, bmi, 0, kMem + 0x80, false);
        d.bits = memory_.read_u32(kMem + 0x80).value_or(0);
        d.dc = mgr_.create_compatible_dc(0);
        d.w = w;
        d.h = h;
        (void)mgr_.select_object(d.dc, d.bmp);
        return d;
    }
    void reset(const Dib& d, u32 v) {
        for (i32 i = 0; i < d.w * d.h; ++i) (void)memory_.write_u32(d.bits + i * 4, v);
    }
    /// Bounding box "l,t,r,b" of pixels that differ from @p fill, as the
    /// probes print it.
    std::string box(const Dib& d, u32 fill) {
        i32 l = d.w, t = d.h, r = -1, b = -1;
        for (i32 y = 0; y < d.h; ++y) {
            for (i32 x = 0; x < d.w; ++x) {
                if (memory_.read_u32(d.bits + (y * d.w + x) * 4).value() == fill) continue;
                l = std::min(l, x);
                t = std::min(t, y);
                r = std::max(r, x);
                b = std::max(b, y);
            }
        }
        return std::to_string(l) + "," + std::to_string(t) + "," + std::to_string(r + 1) + "," + std::to_string(b + 1);
    }

    static std::vector<std::string> expected_lines(const char* file) {
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    }
    static std::string expected_line(const char* file, const std::string& prefix) {
        for (const auto& l : expected_lines(file)) {
            if (l.rfind(prefix, 0) == 0) return l;
        }
        ADD_FAILURE() << "no line '" << prefix << "' in " << file;
        return {};
    }
    /// "key=value" out of a probe line.
    static std::string field(const std::string& line, const std::string& key) {
        const auto at = line.find(" " + key + "=");
        if (at == std::string::npos) return {};
        const auto start = at + key.size() + 2;
        return line.substr(start, line.find(' ', start) - start);
    }

    HandleTable handles_;
    GdiManager mgr_{handles_};
    GuestMemory memory_;
    cpu::CpuState cpu_;
    DllExports exports_;
};

TEST_F(GdiTextTest, TextMetricsMatchWindowsForSharedFaces) {
    // s3gdi_text_probe: every size of the faces macOS ships in Windows'
    // version, including positive (cell) heights, where the tallest VDMX
    // height that fits is taken at its smallest ppem (Arial 16 -> 13), and
    // lfHeight 0, which is -16.
    const auto lines = expected_lines("s3gdi_text_probe.expected");
    int compared = 0;
    const Handle dc = mgr_.create_compatible_dc(0);
    for (const auto& line : lines) {
        std::string face;
        if (line.rfind("Arial ", 0) == 0) face = "Arial";
        else if (line.rfind("Courier New ", 0) == 0) face = "Courier New";
        else continue;
        std::istringstream words(line.substr(face.size() + 1));
        std::string height_word, next;
        words >> height_word >> next;
        if (next != "tm=1") continue;  // the weight / italic rows
        const i32 height = std::stoi(height_word);
        const Handle f = font(std::u16string(face.begin(), face.end()), height);
        (void)mgr_.select_object(dc, f);
        auto tm = metrics(dc);
        const std::string label = face + " " + height_word;
        for (const char* key : {"h", "a", "d", "il", "el", "ave", "max", "wt", "oh"}) {
            EXPECT_EQ(std::to_string(tm[key]), field(line, key)) << label << " " << key;
        }
        EXPECT_EQ(std::to_string(tm["ch0"]) + "," + std::to_string(tm["ch1"]) + "," + std::to_string(tm["ch2"]) +
                      "," + std::to_string(tm["ch3"]),
                  field(line, "ch"))
            << label;
        char pf[8];
        std::snprintf(pf, sizeof(pf), "%02llx", static_cast<unsigned long long>(tm["pf"]));
        EXPECT_EQ(std::string(pf), field(line, "pf")) << label;
        // "Hello, World!" extent and 'a'..'e' widths.
        const GuestAddr s = wstr(kMem + 0x3000, u"Hello, World!");
        EXPECT_EQ(call("GetTextExtentPoint32W", {dc, s, 13, kMem + 0x3100}), 1u);
        const std::string ext = std::to_string(memory_.read_u32(kMem + 0x3100).value()) + "x" +
                                std::to_string(memory_.read_u32(kMem + 0x3104).value());
        EXPECT_EQ("ext=" + ext, line.substr(line.find("ext="), line.find(' ', line.find("ext=")) - line.find("ext=")))
            << label;
        EXPECT_EQ(call("GetCharWidth32W", {dc, 'a', 'e', kMem + 0x3200}), 1u);
        std::string w;
        for (int i = 0; i < 5; ++i) w += (i ? "," : "") + std::to_string(memory_.read_u32(kMem + 0x3200 + i * 4).value());
        EXPECT_EQ(w, field(line, "w")) << label;
        (void)mgr_.select_object(dc, mgr_.get_stock_object(13));
        (void)mgr_.delete_object(f);
        ++compared;
    }
    EXPECT_EQ(compared, 18);
}

TEST_F(GdiTextTest, WeightAndStyleSelectTheFacesWindowsPicks) {
    // "Arial -13 weight 700/100/900": the Bold face for 700 and 900, the
    // regular one (reporting its own weight, 400) for 100.
    const Handle dc = mgr_.create_compatible_dc(0);
    for (const auto& [weight, expected] : std::vector<std::pair<i32, std::string>>{{700, "700"}, {100, "400"}, {900, "700"}}) {
        const std::string line = expected_line("s3gdi_text_probe.expected", "Arial -13 weight " + std::to_string(weight));
        (void)mgr_.select_object(dc, font(u"Arial", -13, weight));
        auto tm = metrics(dc);
        EXPECT_EQ(std::to_string(tm["wt"]), expected);
        EXPECT_EQ(std::to_string(tm["wt"]), field(line, "wt"));
        EXPECT_EQ(std::to_string(tm["h"]), field(line, "h"));
        EXPECT_EQ(std::to_string(tm["max"]), field(line, "max"));
    }
    const std::string line = expected_line("s3gdi_text_probe.expected", "Arial -13 italic+underline+strike");
    (void)mgr_.select_object(dc, font(u"Arial", -13, 0, 0, true, 1, 1));
    auto tm = metrics(dc);
    EXPECT_EQ(std::to_string(tm["it"]), field(line, "it"));
    EXPECT_EQ(std::to_string(tm["ul"]), field(line, "ul"));
    EXPECT_EQ(std::to_string(tm["so"]), field(line, "so"));
    // tmMaxCharWidth is the face's box (24 on Windows' Arial Italic 7.x, 25
    // for the 5.01 file macOS ships), so it is not compared here.
}

TEST_F(GdiTextTest, FaceNamesFollowGdiMapping) {
    // s3gdi_otm_probe "face ...": case is the face's own, aliases from
    // FontSubstitutes and raster families keep the requested name, and an
    // empty or unknown name maps to MS Sans Serif. "Helvetica" and "Times"
    // keep their names too, but Vitreo does not alias them (Microsoft
    // documents no such alias; Windows' registry content is not copied):
    // they are the host's own Helvetica and Times, so their metrics are not
    // Windows' Arial / Times New Roman and are not compared.
    const Handle dc = mgr_.create_compatible_dc(0);
    struct Case { std::u16string request; std::string label; bool metrics; };
    const Case cases[] = {{u"arial", "arial", true},         {u"ARIAL", "ARIAL", true},
                          {u"Helvetica", "Helvetica", false}, {u"MS Shell Dlg 2", "MS Shell Dlg 2", false},
                          {u"Times", "Times", false},         {u"", "empty", false},
                          {u"System", "System", false}};
    for (const auto& c : cases) {
        const std::string line = expected_line("s3gdi_otm_probe64.expected", "face " + c.label + " ->");
        (void)mgr_.select_object(dc, font(c.request, c.label == "System" ? 0 : -13));
        const u64 n = call("GetTextFaceW", {dc, 64, kMem + 0x3000});
        std::string name;
        for (u32 i = 0; i + 1 < n; ++i) name.push_back(static_cast<char>(memory_.read_u16(kMem + 0x3000 + i * 2).value()));
        const auto quote = line.find('\'');
        EXPECT_EQ(name, line.substr(quote + 1, line.find('\'', quote + 1) - quote - 1)) << c.label;
        EXPECT_EQ(std::to_string(n), line.substr(line.find("-> ") + 3, line.find(' ', line.find("-> ") + 3) - line.find("-> ") - 3));
        auto tm = metrics(dc);
        // System at lfHeight 0 is its one 16-pixel cell.
        if (c.metrics || c.label == "System") {
            EXPECT_EQ(std::to_string(tm["h"]), field(line, "h")) << c.label;
            EXPECT_EQ(std::to_string(tm["a"]), field(line, "a")) << c.label;
            EXPECT_EQ(std::to_string(tm["d"]), field(line, "d")) << c.label;
        }
    }
}

TEST_F(GdiTextTest, DocumentedLogicalFontAliasesResolveToHostFaces) {
    // Learn, "Using MS Shell Dlg and MS Shell Dlg 2" (Map the Logical Fonts):
    // with a non-Japanese machine UI language "MS Shell Dlg" maps to
    // Microsoft Sans Serif, and "MS Shell Dlg 2 simply uses the Tahoma font
    // regardless of language". Matching is case-insensitive like every face
    // name; anything Microsoft does not document as an alias is not one.
    EXPECT_EQ(GdiManager::substitute_face(u"MS Shell Dlg"), u"Microsoft Sans Serif");
    EXPECT_EQ(GdiManager::substitute_face(u"ms shell dlg"), u"Microsoft Sans Serif");
    EXPECT_EQ(GdiManager::substitute_face(u"MS Shell Dlg 2"), u"Tahoma");
    EXPECT_EQ(GdiManager::substitute_face(u"MS SHELL DLG 2"), u"Tahoma");
    for (const std::u16string other : {u"Helvetica", u"Helv", u"Tms Rmn", u"Times", u"Arial", u"",
                                       u"MS Shell Dlg 3", u"NoSuchFontXYZ"}) {
        EXPECT_EQ(GdiManager::substitute_face(other), other);
    }

    // Each alias draws with the host's face of its target: same metrics as
    // asking for the target by name, and GetTextFace keeps the alias.
    const Handle dc = mgr_.create_compatible_dc(0);
    auto face_of = [&]() {
        const u64 n = call("GetTextFaceW", {dc, 64, kMem + 0x3000});
        std::string name;
        for (u32 i = 0; i + 1 < n; ++i) name.push_back(static_cast<char>(memory_.read_u16(kMem + 0x3000 + i * 2).value()));
        return name;
    };
    struct Case { std::u16string alias; std::u16string target; std::string label; };
    for (const Case& c : {Case{u"MS Shell Dlg", u"Microsoft Sans Serif", "MS Shell Dlg"},
                          Case{u"MS Shell Dlg 2", u"Tahoma", "MS Shell Dlg 2"}}) {
        auto host = dwrite::match_system_font(c.target, 400, 5, 0);
        ASSERT_TRUE(host.has_value()) << c.label;
        ASSERT_EQ(host->family_name, c.target) << c.label << ": the host has no such family";
        for (const i32 height : {-11, -13, -16}) {
            (void)mgr_.select_object(dc, font(c.target, height));
            const auto want = metrics(dc);
            (void)mgr_.select_object(dc, font(c.alias, height));
            EXPECT_EQ(face_of(), c.label);
            const auto got = metrics(dc);
            for (const char* k : {"h", "a", "d", "il", "el", "ave", "max", "pf"}) {
                EXPECT_EQ(got.at(k), want.at(k)) << c.label << " " << height << " " << k;
            }
        }
    }
}

TEST_F(GdiTextTest, UnknownFaceNameIsNotAHostFallbackFont) {
    // s3gdi_text_probe "NoSuchFontXYZ -13": GDI treats an unknown face as no
    // name and reports MS Sans Serif (h=16 a=13 d=3); Core Text would answer
    // the same request with Helvetica.
    const Handle dc = mgr_.create_compatible_dc(0);
    const std::string line = expected_line("s3gdi_text_probe.expected", "NoSuchFontXYZ -13 ");
    (void)mgr_.select_object(dc, font(u"NoSuchFontXYZ", -13));
    const u64 n = call("GetTextFaceW", {dc, 64, kMem + 0x3000});
    std::string name;
    for (u32 i = 0; i + 1 < n; ++i) name.push_back(static_cast<char>(memory_.read_u16(kMem + 0x3000 + i * 2).value()));
    EXPECT_EQ(name, line.substr(line.rfind(")=") + 2));
    auto tm = metrics(dc);
    EXPECT_EQ(std::to_string(tm["h"]), field(line, "h"));
    EXPECT_EQ(std::to_string(tm["a"]), field(line, "a"));
    EXPECT_EQ(std::to_string(tm["d"]), field(line, "d"));
}

TEST_F(GdiTextTest, GetTextFaceCountsDifferBetweenWideAndAnsi) {
    // s3gdi_text_probe / s3gdi_otm_probe: W counts the NUL (cch4 -> 4,
    // full -> 6), A does not (cch4 -> 3, cch64 -> 5); a NULL buffer is
    // length + 1 for both; A with cch 0 is ERROR_INVALID_PARAMETER, W with
    // cch 0 is 0 with the error untouched, as is a NULL DC.
    const Handle dc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(dc, font(u"Arial", -13));
    EXPECT_EQ(call("GetTextFaceW", {dc, 4, kMem + 0x3000}), 4u);
    EXPECT_EQ(memory_.read_u16(kMem + 0x3000 + 6).value(), 0u);
    EXPECT_EQ(memory_.read_u16(kMem + 0x3000 + 4).value(), u'i');
    EXPECT_EQ(call("GetTextFaceW", {dc, 64, kMem + 0x3000}), 6u);
    EXPECT_EQ(call("GetTextFaceW", {dc, 0, 0}), 6u);
    EXPECT_EQ(call("GetTextFaceA", {dc, 4, kMem + 0x3000}), 3u);
    EXPECT_EQ(memory_.read_u8(kMem + 0x3003).value(), 0u);
    EXPECT_EQ(call("GetTextFaceA", {dc, 64, kMem + 0x3000}), 5u);
    EXPECT_EQ(call("GetTextFaceA", {dc, 0, 0}), 6u);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetTextFaceA", {dc, 0, kMem + 0x3000}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetTextFaceW", {dc, 0, kMem + 0x3000}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call("GetTextFaceW", {0, 8, kMem + 0x3000}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(GdiTextTest, TextMetricsAIsFiftyThreeBytesWithAnsiRange) {
    // s3gdi_otm_probe "TEXTMETRICA sizeof=56 bytes44..55=1eff1f200000002700cc...":
    // first 30 (default - 1), last 255, default 31, break 32, and bytes 53..55
    // untouched; TEXTMETRICW writes its padding as zero.
    const Handle dc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(dc, font(u"Arial", -13));
    for (u32 i = 0; i < 64; ++i) (void)memory_.write_u8(kMem + 0x3000 + i, 0xCC);
    EXPECT_EQ(call("GetTextMetricsA", {dc, kMem + 0x3000}), 1u);
    const u8 want_a[12] = {0x1e, 0xff, 0x1f, 0x20, 0, 0, 0, 0x27, 0, 0xcc, 0xcc, 0xcc};
    for (u32 i = 0; i < 12; ++i) EXPECT_EQ(memory_.read_u8(kMem + 0x3000 + 44 + i).value(), want_a[i]) << i;
    for (u32 i = 0; i < 64; ++i) (void)memory_.write_u8(kMem + 0x3000 + i, 0xCC);
    EXPECT_EQ(call("GetTextMetricsW", {dc, kMem + 0x3000}), 1u);
    const u8 want_w[20] = {0x20, 0, 0xfc, 0xff, 0x1f, 0, 0x20, 0, 0, 0, 0, 0x27, 0, 0, 0, 0, 0xcc, 0xcc, 0xcc, 0xcc};
    for (u32 i = 0; i < 20; ++i) EXPECT_EQ(memory_.read_u8(kMem + 0x3000 + 44 + i).value(), want_w[i]) << i;
}

TEST_F(GdiTextTest, FailureAndLastErrorContracts) {
    const Handle dc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(dc, font(u"Arial", -13));
    const GuestAddr s = wstr(kMem + 0x3000, u"ab");
    struct Case {
        const char* name;
        std::vector<u64> args;
        u64 ret;
        u32 err;
    };
    const u32 keep = 0xDEAD;
    const Case cases[] = {
        // s3gdi_text_probe / s3gdi_textdraw_probe / s3gdi_otm_probe rows.
        {"GetTextMetricsW", {0, kMem + 0x3100}, 0, keep},
        {"GetTextExtentPoint32W", {0, s, 1, kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetTextExtentPoint32W", {dc, s, static_cast<u64>(-1), kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetTextExtentPoint32W", {dc, s, 0, kMem + 0x3100}, 1, keep},
        {"GetTextExtentPoint32W", {dc, s, 2, kMem + 0x3100}, 1, keep},  // x64 leaves it
        {"GetTextExtentExPointW", {0, s, 2, 10, 0, 0, kMem + 0x3100}, 0, keep},
        {"GetTextExtentExPointW", {dc, s, static_cast<u64>(-1), 10, 0, 0, kMem + 0x3100}, 0, 0},
        {"GetTextExtentExPointW", {dc, s, 2, 100, 0, 0, kMem + 0x3100}, 1, 0},
        {"GetTextExtentPointI", {0, s, 2, kMem + 0x3100}, 0, keep},
        {"TextOutW", {0, 0, 0, s, 1}, 0, keep},
        {"TextOutW", {dc, 0, 0, s, 0}, 1, keep},
        {"TextOutW", {dc, 0, 0, s, static_cast<u64>(-1)}, 0, keep},
        {"TextOutW", {dc, 0, 0, s, 2}, 1, 0},
        {"ExtTextOutW", {0, 0, 0, 0, 0, s, 1, 0}, 0, keep},
        {"ExtTextOutW", {dc, 0, 0, 0, 0, s, 2, 0}, 1, 0},
        {"ExtTextOutW", {dc, 0, 0, 0x10, 0, s, 2, 0}, 1, keep},  // ETO_GLYPH_INDEX
        {"GetCharWidth32W", {dc, 'b', 'a', kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetCharWidth32W", {dc, 'a', 'a', 0}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetCharWidth32W", {0, 'a', 'a', kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetCharWidthW", {dc, 'a', 'c', kMem + 0x3100}, 1, keep},
        {"GetCharABCWidthsW", {0, 'a', 'a', kMem + 0x3100}, 0, error_codes::ERROR_INVALID_HANDLE},
        {"GetCharABCWidthsW", {dc, 'b', 'a', kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"GetCharABCWidthsFloatW", {0, 'a', 'a', kMem + 0x3100}, 0, error_codes::ERROR_INVALID_HANDLE},
        {"SetTextCharacterExtra", {0, 1}, 0x80000000u, keep},
        {"GetTextAlign", {0}, 0xFFFFFFFFu, keep},
        {"GetOutlineTextMetricsW", {0, 0, 0}, 0, keep},
        {"RemoveFontMemResourceEx", {0x1234}, 0, keep},
        {"RemoveFontMemResourceEx", {0}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"AddFontMemResourceEx", {0, 0, 0, kMem + 0x3100}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"AddFontMemResourceEx", {s, 4, 0, 0}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"AddFontResourceExW", {s, 0x1234, 0}, 0, error_codes::ERROR_INVALID_PARAMETER},
        {"AddFontResourceExW", {wstr(kMem + 0x3400, u"C:\\nonexistent\\nofont.ttf"), 0x10, 0}, 0,
         error_codes::ERROR_FILE_NOT_FOUND},
        {"RemoveFontResourceW", {kMem + 0x3400}, 0, error_codes::ERROR_FILE_NOT_FOUND},
    };
    for (const auto& c : cases) {
        set_last_error(keep);
        EXPECT_EQ(call(c.name, c.args), c.ret) << c.name;
        EXPECT_EQ(get_last_error(), c.err) << c.name;
    }
    // Junk bytes: NULL, error and *pNumFonts untouched.
    for (u32 i = 0; i < 64; ++i) (void)memory_.write_u8(kMem + 0x3500 + i, 0x5A);
    (void)memory_.write_u32(kMem + 0x3600, 77);
    set_last_error(keep);
    EXPECT_EQ(call("AddFontMemResourceEx", {kMem + 0x3500, 64, 0, kMem + 0x3600}), 0u);
    EXPECT_EQ(get_last_error(), keep);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3600).value(), 77u);
}

TEST_F(GdiTextTest, ExtentExPointStopsDxAtTheFit) {
    // "GetTextExtentExPointW(max20)=1 fit=3 dx=9,16,19,0,0 size=29x16",
    // "(nofit) dx=9,16,19,22,29", "char extra 2: extent=39x16".
    const Handle dc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(dc, font(u"Arial", -13));
    const GuestAddr s = wstr(kMem + 0x3000, u"Hello");
    for (u32 i = 0; i < 5; ++i) (void)memory_.write_u32(kMem + 0x3100 + i * 4, 0);
    EXPECT_EQ(call("GetTextExtentExPointW", {dc, s, 5, 20, kMem + 0x3200, kMem + 0x3100, kMem + 0x3300}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3200).value(), 3u);
    const u32 fit_dx[5] = {9, 16, 19, 0, 0};
    for (u32 i = 0; i < 5; ++i) EXPECT_EQ(memory_.read_u32(kMem + 0x3100 + i * 4).value(), fit_dx[i]);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3300).value(), 29u);
    EXPECT_EQ(call("GetTextExtentExPointW", {dc, s, 5, 0, 0, kMem + 0x3100, kMem + 0x3300}), 1u);
    const u32 all_dx[5] = {9, 16, 19, 22, 29};
    for (u32 i = 0; i < 5; ++i) EXPECT_EQ(memory_.read_u32(kMem + 0x3100 + i * 4).value(), all_dx[i]);
    EXPECT_EQ(call("SetTextCharacterExtra", {dc, 2}), 0u);
    EXPECT_EQ(call("GetTextCharacterExtra", {dc}), 2u);
    EXPECT_EQ(call("GetTextExtentPoint32W", {dc, s, 5, kMem + 0x3300}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3300).value(), 39u);
    // Glyph-index measuring ("glyphs=5 43,72,79,79,82 extentI=1 29x16").
    (void)call("SetTextCharacterExtra", {dc, 0});
    const u16 gi[5] = {43, 72, 79, 79, 82};
    for (u32 i = 0; i < 5; ++i) (void)memory_.write_u16(kMem + 0x3400 + i * 2, gi[i]);
    EXPECT_EQ(call("GetTextExtentPointI", {dc, kMem + 0x3400, 5, kMem + 0x3300}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3300).value(), 29u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3304).value(), 16u);
}

TEST_F(GdiTextTest, TextOutCellsMatchWindows) {
    // s3gdi_textdraw_probe: the opaque cell of "Hi" (Arial -13,
    // NONANTIALIASED) under each alignment, TA_UPDATECP, lpDx, ETO_OPAQUE,
    // ETO_CLIPPED and SetTextCharacterExtra; background pixels carry a zero
    // reserved byte.
    const Dib d = make_dib32(96, 40);
    const u32 fill = 0x80336699;
    (void)mgr_.select_object(d.dc, font(u"Arial", -13, 0, 3));
    (void)mgr_.set_bk_color(d.dc, 0x00FF00);
    (void)mgr_.set_text_color(d.dc, 0x0000FF);
    const GuestAddr s = wstr(kMem + 0x3000, u"Hi");
    auto expect_box = [&](const std::string& label) {
        const std::string line = expected_line("s3gdi_textdraw_probe.expected", label + " box=");
        EXPECT_EQ(box(d, fill), field(line, "box")) << label;
    };
    reset(d, fill);
    (void)call("TextOutW", {d.dc, 5, 3, s, 2});
    expect_box("opaque mono");
    EXPECT_EQ(memory_.read_u32(d.bits + (3 * 96 + 5) * 4).value(), 0x0000FF00u);  // bk, reserved 0
    const std::pair<u32, const char*> aligns[] = {{0, "left|top"}, {6, "center|top"}, {2, "right|top"},
                                                  {24, "left|baseline"}, {8, "left|bottom"}};
    for (const auto& [align, name] : aligns) {
        (void)call("SetTextAlign", {d.dc, align});
        reset(d, fill);
        (void)call("TextOutW", {d.dc, 40, 20, s, 2});
        expect_box(std::string("align ") + name + " at 40,20");
    }
    (void)call("SetTextAlign", {d.dc, 1});  // TA_UPDATECP
    i32 px = 0, py = 0;
    (void)mgr_.move_to_ex(d.dc, 10, 4, &px, &py);
    reset(d, fill);
    (void)call("TextOutW", {d.dc, 70, 30, s, 2});
    expect_box("updatecp from 10,4");
    EXPECT_EQ(call("GetCurrentPositionEx", {d.dc, kMem + 0x3100}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3100).value(), 22u);
    (void)call("SetTextAlign", {d.dc, 3});  // TA_UPDATECP | TA_RIGHT
    (void)mgr_.move_to_ex(d.dc, 60, 4, &px, &py);
    reset(d, fill);
    (void)call("TextOutW", {d.dc, 0, 0, s, 2});
    expect_box("updatecp|right from 60,4");
    (void)call("GetCurrentPositionEx", {d.dc, kMem + 0x3100});
    EXPECT_EQ(memory_.read_u32(kMem + 0x3100).value(), 48u);
    (void)call("SetTextAlign", {d.dc, 0});
    (void)memory_.write_u32(kMem + 0x3200, 20);
    (void)memory_.write_u32(kMem + 0x3204, 15);
    reset(d, fill);
    (void)call("ExtTextOutW", {d.dc, 5, 3, 0, 0, s, 2, kMem + 0x3200});
    expect_box("exttextout dx 20,15");
    const i32 rc[4] = {2, 2, 30, 12};
    (void)memory_.write_bytes(kMem + 0x3300, reinterpret_cast<const u8*>(rc), 16);
    reset(d, fill);
    (void)call("ExtTextOutW", {d.dc, 5, 3, 2, kMem + 0x3300, s, 2, 0});
    expect_box("eto_opaque rect 2,2,30,12");
    reset(d, fill);
    (void)call("ExtTextOutW", {d.dc, 5, 3, 2, kMem + 0x3300, 0, 0, 0});
    expect_box("eto_opaque no text");
    reset(d, fill);
    (void)call("ExtTextOutW", {d.dc, 5, 3, 4, kMem + 0x3300, s, 2, 0});
    expect_box("eto_clipped");
    (void)mgr_.set_bk_mode(d.dc, 1);
    reset(d, fill);
    (void)call("ExtTextOutW", {d.dc, 5, 3, 2, kMem + 0x3300, s, 2, 0});
    expect_box("eto_opaque transparent-mode");
    (void)mgr_.set_bk_mode(d.dc, 2);
    (void)call("SetTextCharacterExtra", {d.dc, 3});
    reset(d, fill);
    (void)call("TextOutW", {d.dc, 5, 3, s, 2});
    expect_box("charextra 3");
}

TEST_F(GdiTextTest, DrawTextLayoutMatchesWindows) {
    // s3gdi_drawtext_probe: DT_CALCRECT rectangles and return values.
    const Dib d = make_dib32(160, 80);
    (void)mgr_.select_object(d.dc, font(u"Arial", -13, 0, 3));
    struct Case {
        const char* label;
        std::u16string text;
        std::array<i32, 4> rect;
        u32 format;
    };
    const std::array<i32, 4> r1{10, 5, 90, 30}, r2{10, 5, 60, 70}, r3{10, 10, 110, 50}, r4{10, 5, 40, 30};
    const Case cases[] = {
        {"calcrect", u"Hello", r1, 0x400},
        {"calcrect multiline", u"ab\ncd", r1, 0x400},
        {"calcrect wordbreak", u"The quick brown fox", r2, 0x410},
        {"calcrect wordbreak longword", u"Supercalifragilistic x", r2, 0x410},
        {"calcrect wordbreak spaces", u"ab    cd", {10, 5, 25, 70}, 0x410},
        {"center|vcenter|single", u"Hello", r3, 0x25},
        {"right|bottom|single", u"Hello", r3, 0x2A},
        {"calcrect prefix", u"&File", r1, 0x400},
        {"calcrect noprefix", u"&File", r1, 0xC00},
        {"double amp", u"A&&B", r1, 0x400},
        {"calcrect tab", u"a\tb", r1, 0x400},
        {"calcrect expandtabs", u"a\tb", r1, 0x440},
        {"calcrect end ellipsis", u"Hello World", r4, 0x8420},
        {"calcrect negative rect", u"Hello", {50, 40, 10, 5}, 0x400},
        {"calcrect trailing newline", u"ab\n", r1, 0x400},
    };
    for (const auto& c : cases) {
        const std::string line = expected_line("s3gdi_drawtext_probe.expected", std::string(c.label) + " ret=");
        i32 l = c.rect[0], t = c.rect[1], r = c.rect[2], b = c.rect[3];
        const i32 ret = mgr_.draw_text_ex(d.dc, c.text, l, t, r, b, c.format, nullptr);
        EXPECT_EQ(std::to_string(ret), field(line, "ret")) << c.label;
        EXPECT_EQ(std::to_string(l) + "," + std::to_string(t) + "," + std::to_string(r) + "," + std::to_string(b),
                  field(line, "rc"))
            << c.label;
    }
    // Drawn boxes: the cell is filled with the background (OPAQUE).
    const u32 fill = 0x80336699;
    struct Drawn {
        const char* label;
        std::u16string text;
        std::array<i32, 4> rect;
        u32 format;
    };
    const Drawn drawn[] = {
        {"left|top", u"Hello", r1, 0},
        {"multiline", u"ab\ncd", r1, 0},
        {"center|vcenter|single", u"Hello", r3, 0x25},
        {"right|bottom|single", u"Hello", r3, 0x2A},
        {"prefixonly", u"&File", r1, 0x200000},
        {"calcrect tabstop 4", u"a\tb", r1, 0x4C0 | 0x80},
        {"clip", u"Hello World", r4, 0x20},
        {"noclip", u"Hello World", r4, 0x120},
        {"end ellipsis", u"Hello World", r4, 0x8020},
    };
    for (const auto& c : drawn) {
        const std::string line = expected_line("s3gdi_drawtext_probe.expected", std::string(c.label) + " ret=");
        reset(d, fill);
        i32 l = c.rect[0], t = c.rect[1], r = c.rect[2], b = c.rect[3];
        (void)mgr_.draw_text_ex(d.dc, c.text, l, t, r, b, c.format, nullptr);
        EXPECT_EQ(box(d, fill), field(line, "box")) << c.label;
    }
    // External leading adds to each line ("calcrect 2 lines -16 [externalleading]" 36/38).
    (void)mgr_.select_object(d.dc, font(u"Arial", -16, 0, 3));
    i32 l = 10, t = 5, r = 90, b = 30;
    EXPECT_EQ(mgr_.draw_text_ex(d.dc, u"a\nb", l, t, r, b, 0x400, nullptr), 36);
    l = 10, t = 5, r = 90, b = 30;
    EXPECT_EQ(mgr_.draw_text_ex(d.dc, u"a\nb", l, t, r, b, 0x600, nullptr), 38);
}

TEST_F(GdiTextTest, DrawTextExportRulesAndMargins) {
    // "empty ret=1 err=57005", "calcrect empty rc=10,5,10,5", "left|top ret=16
    // err=0", "NULL dc ret=0 err=57005", "ex calcrect margins ret=16
    // rc=10,5,51,21 drawn=5", "ex bad cbSize ret=0 err=87".
    const Dib d = make_dib32(160, 80);
    (void)mgr_.select_object(d.dc, font(u"Arial", -13, 0, 3));
    const GuestAddr rect = kMem + 0x3000;
    auto set_rect = [&](i32 l, i32 t, i32 r, i32 b) {
        const i32 v[4] = {l, t, r, b};
        (void)memory_.write_bytes(rect, reinterpret_cast<const u8*>(v), 16);
    };
    auto rect_str = [&] {
        std::string out;
        for (u32 i = 0; i < 4; ++i) out += (i ? "," : "") + std::to_string(static_cast<i32>(memory_.read_u32(rect + i * 4).value()));
        return out;
    };
    const GuestAddr hello = wstr(kMem + 0x3100, u"Hello");
    const GuestAddr empty = wstr(kMem + 0x3200, u"");
    set_rect(10, 5, 90, 30);
    set_last_error(0xDEAD);
    EXPECT_EQ(draw_text_export({d.dc, empty, static_cast<u64>(-1), rect, 0}, false), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(draw_text_export({d.dc, empty, static_cast<u64>(-1), rect, 0x400}, false), 1u);
    EXPECT_EQ(rect_str(), "10,5,10,5");
    set_rect(10, 5, 90, 30);
    EXPECT_EQ(draw_text_export({d.dc, hello, 0, rect, 0}, false), 1u);
    EXPECT_EQ(draw_text_export({d.dc, hello, static_cast<u64>(-1), rect, 0}, false), 16u);
    EXPECT_EQ(get_last_error(), 0u);
    set_last_error(0xDEAD);
    EXPECT_EQ(draw_text_export({0, hello, static_cast<u64>(-1), rect, 0}, false), 0u);
    EXPECT_EQ(draw_text_export({d.dc, 0, static_cast<u64>(-1), rect, 0}, false), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const GuestAddr params = kMem + 0x3300;
    const u32 p[5] = {20, 4, 5, 7, 0};
    (void)memory_.write_bytes(params, reinterpret_cast<const u8*>(p), 20);
    EXPECT_EQ(draw_text_export({d.dc, hello, static_cast<u64>(-1), rect, 0x400, params}, true), 16u);
    EXPECT_EQ(rect_str(), "10,5,51,21");
    EXPECT_EQ(memory_.read_u32(params + 16).value(), 5u);
    (void)memory_.write_u32(params, 7);
    set_last_error(0xDEAD);
    EXPECT_EQ(draw_text_export({d.dc, hello, static_cast<u64>(-1), rect, 0, params}, true), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(GdiTextTest, OutlineTextMetricsFromFontTables) {
    // s3gdi_otm_probe64 "Arial -13": typographic metrics, em square, cap/x
    // heights (half and a quarter em), hhea metrics, sub/superscript,
    // strike-out and underscore; names follow the 232-byte x64 struct. The
    // full name (name id 3) and the font box differ between Windows' Arial
    // 7.x and macOS' 5.01 and are not compared.
    const Handle dc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(dc, font(u"Arial", -13));
    const std::string line = expected_line("s3gdi_otm_probe64.expected", "Arial -13 ");
    const u64 need = call("GetOutlineTextMetricsW", {dc, 0, 0});
    ASSERT_GT(need, 232u);
    const GuestAddr o = kMem + 0x4000;
    EXPECT_EQ(call("GetOutlineTextMetricsW", {dc, need, o}), need);
    const GuestAddr m = o + 76;
    auto i = [&](u32 off) { return std::to_string(static_cast<i32>(memory_.read_u32(m + off).value())); };
    EXPECT_EQ(i(0), field(line, "fs"));
    EXPECT_EQ(i(4), field(line, "ff"));
    EXPECT_EQ(i(8) + "," + i(12), field(line, "slope"));
    EXPECT_EQ(i(16), field(line, "ia"));
    EXPECT_EQ(i(20), field(line, "em"));
    EXPECT_EQ(i(24), field(line, "asc"));
    EXPECT_EQ(i(28), field(line, "desc"));
    EXPECT_EQ(i(32), field(line, "gap"));
    EXPECT_EQ(i(36), field(line, "cap"));
    EXPECT_EQ(i(40), field(line, "x"));
    EXPECT_EQ(i(60), field(line, "macasc"));
    EXPECT_EQ(i(64), field(line, "macdesc"));
    EXPECT_EQ(i(68), field(line, "macgap"));
    EXPECT_EQ(i(72), field(line, "minppem"));
    EXPECT_EQ(i(76) + "," + i(80) + "," + i(84) + "," + i(88), field(line, "sub"));
    EXPECT_EQ(i(92) + "," + i(96) + "," + i(100) + "," + i(104), field(line, "sup"));
    EXPECT_EQ(i(108) + "," + i(112), field(line, "strike"));
    EXPECT_EQ(i(116) + "," + i(120), field(line, "us"));
    std::string offs;
    for (u32 k = 0; k < 4; ++k) offs += (k ? "," : "") + std::to_string(memory_.read_u32(o + 200 + k * 8).value());
    EXPECT_EQ(offs, field(line, "offs"));
    // A short buffer receives cjCopy bytes and the call returns cjCopy.
    for (u32 k = 0; k < 120; ++k) (void)memory_.write_u8(o + k, 0xCC);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetOutlineTextMetricsW", {dc, 100, o}), 100u);
    EXPECT_EQ(memory_.read_u8(o + 100).value(), 0xCCu);
    EXPECT_EQ(memory_.read_u32(o).value(), need);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(GdiTextTest, MemoryFontIsSelectableAndRemovable) {
    // AddFontMemResourceEx: a real font file's bytes add its faces for this
    // process and report their count; RemoveFontMemResourceEx takes them away.
    std::ifstream in("/System/Library/Fonts/Supplemental/Tahoma.ttf", std::ios::binary);
    if (!in) GTEST_SKIP() << "Tahoma.ttf not on this host";
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(memory_.map_region(0x100000, (bytes.size() + 0xFFF) & ~0xFFFull, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory_.write_bytes(0x100000, bytes.data(), bytes.size()).is_ok());
    (void)memory_.write_u32(kMem + 0x3000, 0);
    const u64 h = call("AddFontMemResourceEx", {0x100000, bytes.size(), 0, kMem + 0x3000});
    ASSERT_NE(h, 0u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x3000).value(), 1u);
    EXPECT_EQ(call("RemoveFontMemResourceEx", {h}), 1u);
    EXPECT_EQ(call("RemoveFontMemResourceEx", {h}), 0u);
}

TEST_F(GdiTextTest, EnumFontFamiliesExCallsBackLikeWindows) {
    // s3gdi_enumfont_probe: Arial's four styles sorted by full name at 32
    // ppem (lfHeight 36, lfWidth 14 for the regular), NTM_* flags, "Western";
    // 34 entries for DEFAULT_CHARSET; the callback's 0 stops and its value is
    // returned; a missing family makes no calls and returns 1.
    struct Entry {
        i32 height, width, weight;
        u8 italic, charset, out_precision, clip_precision, quality, pitch;
        std::string face, full, style, script;
        u32 type, flags, em;
    };
    std::vector<Entry> seen;
    u64 answer = 1;
    auto wide_at = [&](GuestAddr a, usize max) {
        std::string out;
        for (usize i = 0; i < max; ++i) {
            const u16 c = memory_.read_u16(a + i * 2).value();
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    };
    mgr_.set_guest_call([&](GuestAddr, const u64* args, usize n, bool) -> u64 {
        EXPECT_EQ(n, 4u);
        const GuestAddr elf = args[0], ntm = args[1];
        Entry e;
        e.height = static_cast<i32>(memory_.read_u32(elf).value());
        e.width = static_cast<i32>(memory_.read_u32(elf + 4).value());
        e.weight = static_cast<i32>(memory_.read_u32(elf + 16).value());
        e.italic = memory_.read_u8(elf + 20).value();
        e.charset = memory_.read_u8(elf + 23).value();
        e.out_precision = memory_.read_u8(elf + 24).value();
        e.clip_precision = memory_.read_u8(elf + 25).value();
        e.quality = memory_.read_u8(elf + 26).value();
        e.pitch = memory_.read_u8(elf + 27).value();
        e.face = wide_at(elf + 28, 32);
        e.full = wide_at(elf + 92, 64);
        e.style = wide_at(elf + 92 + 128, 32);
        e.script = wide_at(elf + 92 + 192, 32);
        e.type = static_cast<u32>(args[2]);
        e.flags = memory_.read_u32(ntm + 60).value();
        e.em = memory_.read_u32(ntm + 64).value();
        seen.push_back(e);
        return answer;
    });
    const Handle dc = mgr_.create_compatible_dc(0);
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"Arial", 0, 0x1000, 0, true, true), 1);
    ASSERT_EQ(seen.size(), 4u);
    const char* fulls[4] = {"Arial", "Arial Bold", "Arial Bold Italic", "Arial Italic"};
    const u32 flags[4] = {0x40, 0x20, 0x21, 0x01};
    for (usize i = 0; i < 4; ++i) {
        EXPECT_EQ(seen[i].full, fulls[i]);
        EXPECT_EQ(seen[i].flags, flags[i]);
        EXPECT_EQ(seen[i].face, "Arial");
        EXPECT_EQ(seen[i].script, "Western");
        EXPECT_EQ(seen[i].type, 4u);
        EXPECT_EQ(seen[i].em, 2048u);
        EXPECT_EQ(seen[i].out_precision, 3);
        EXPECT_EQ(seen[i].clip_precision, 2);
        EXPECT_EQ(seen[i].quality, 1);
        EXPECT_EQ(seen[i].pitch, 0x22);
    }
    EXPECT_EQ(seen[0].height, 36);
    EXPECT_EQ(seen[0].width, 14);
    EXPECT_EQ(seen[0].style, "Regular");
    seen.clear();
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"Arial", 1, 0x1000, 0, true, true), 1);
    EXPECT_EQ(seen.size(), 34u);
    seen.clear();
    answer = 0;
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"Arial", 0, 0x1000, 0, true, true), 0);
    EXPECT_EQ(seen.size(), 1u);
    seen.clear();
    answer = 77;
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"Arial", 0, 0x1000, 0, true, true), 77);
    EXPECT_EQ(seen.size(), 4u);
    seen.clear();
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"NoSuchFontXYZ", 1, 0x1000, 0, true, true), 1);
    EXPECT_TRUE(seen.empty());
    // "MS Shell Dlg" enumerates Microsoft Sans Serif under the alias.
    answer = 1;
    EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"MS Shell Dlg", 0, 0x1000, 0, true, true), 1);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].face, "MS Shell Dlg");
    EXPECT_EQ(seen[0].full, "Microsoft Sans Serif");
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr_.enum_font_families_ex(0, u"Arial", 0, 0x1000, 0, true, true), 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}


TEST_F(GdiTextTest, FontEnumerationAllowsIndependentGdiWorkBothBitnesses) {
    using namespace std::chrono_literals;
    const Handle dc = mgr_.create_compatible_dc(0);
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        std::future<bool> task;
        bool completed = false;
        unsigned calls = 0;
        mgr_.set_guest_call([&](GuestAddr proc, const u64* args, usize n, bool callback64) -> u64 {
            EXPECT_EQ(proc, 0x1000u);
            EXPECT_EQ(n, 4u);
            EXPECT_EQ(callback64, is64);
            EXPECT_EQ(args[3], 0x1234u);
            ++calls;
            task = std::async(std::launch::async, [&] {
                const Handle brush = mgr_.create_solid_brush(0x123456);
                u32 color = 0;
                const bool matches = mgr_.lookup_brush_color(brush, &color) && color == 0x123456;
                return mgr_.delete_object(brush) && matches;
            });
            completed = task.wait_for(500ms) == std::future_status::ready;
            return 0; // Learn: zero callback result stops and is returned.
        });
        {
            GdiManager::ApiLock outer(mgr_);
            EXPECT_EQ(mgr_.enum_font_families_ex(dc, u"Arial", 0, 0x1000, 0x1234, true, is64), 0);
        }
        EXPECT_EQ(calls, 1u);
        EXPECT_TRUE(completed);
        EXPECT_TRUE(task.get());
    }
    EXPECT_TRUE(mgr_.delete_dc(dc));
}

TEST_F(GdiTextTest, EnumFontsUsesLogfontPrefixAndExactAbi) {
    for (const char* name : {"EnumFontsW", "EnumFontsA"}) {
        const auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
            [&](const auto& e) { return e.name == name; });
        ASSERT_NE(it, exports_.entries.end()) << name;
        EXPECT_EQ(it->stdcall_bytes, 16u) << name;
    }
    const Handle dc = mgr_.create_compatible_dc(0);
    for (const bool is64 : {false, true}) {
        set_last_error(0x1111);
        EXPECT_EQ(call("EnumFontsW", {0, 0, 0x1000, 0}, is64), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
        set_last_error(0x1111);
        EXPECT_EQ(call("EnumFontsA", {0, 0, 0x1000, 0}, is64), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    }

    const GuestAddr face = wstr(kMem + 0x5000, u"Arial");
    unsigned calls = 0;
    mgr_.set_guest_call([&](GuestAddr proc, const u64* args, usize n, bool) -> u64 {
        EXPECT_EQ(proc, 0x1000u);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(args[3], 0x55u);
        std::string name;
        for (usize i = 0; i < 32; ++i) {
            const u16 c = memory_.read_u16(args[0] + 28 + i * 2).value();
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
        EXPECT_EQ(name, "Arial");
        // TEXTMETRICW is the prefix of the metrics buffer. A height of 0
        // would be an empty structure, not the realized face.
        EXPECT_NE(static_cast<i32>(memory_.read_u32(args[1]).value()), 0);
        EXPECT_EQ(args[2], 4u);
        ++calls;
        return 0;
    });
    EXPECT_EQ(call("EnumFontsW", {dc, face, 0x1000, 0x55}, true), 0u);
    EXPECT_EQ(calls, 1u);

    (void)memory_.write_bytes(kMem + 0x5100, reinterpret_cast<const u8*>("Arial"), 6);
    calls = 0;
    mgr_.set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        std::string name;
        for (usize i = 0; i < 32; ++i) {
            const u8 c = memory_.read_u8(args[0] + 28 + i).value();
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
        EXPECT_EQ(name, "Arial");
        ++calls;
        return 9;
    });
    EXPECT_EQ(call("EnumFontsA", {dc, kMem + 0x5100, 0x1000, 0}, false), 9u);
    EXPECT_GT(calls, 1u);
    EXPECT_TRUE(mgr_.delete_dc(dc));
}

TEST_F(GdiTextTest, CreatePenIndirectReadsLogpenAndPreservesAbi) {
    const auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
        [](const auto& e) { return e.name == "CreatePenIndirect"; });
    ASSERT_NE(it, exports_.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
    const GuestAddr logpen = kMem + 0x6000;
    const GuestAddr out = kMem + 0x6100;
    for (const bool is64 : {false, true}) {
        set_last_error(0x2222);
        EXPECT_EQ(call("CreatePenIndirect", {0}, is64), 0u);
        EXPECT_EQ(get_last_error(), 0x2222u);
        (void)memory_.write_u32(logpen + 0, 1u);          // PS_DASH
        (void)memory_.write_u32(logpen + 4, static_cast<u32>(-4)); // width.x
        (void)memory_.write_u32(logpen + 8, 99u);         // width.y, unused
        (void)memory_.write_u32(logpen + 12, 0x00AABBCCu);
        const u64 pen = call("CreatePenIndirect", {logpen}, is64);
        EXPECT_NE(pen, 0u);
        for (u32 i = 0; i < 16; i += 4) (void)memory_.write_u32(out + i, 0xFFFFFFFFu);
        EXPECT_EQ(call("GetObjectW", {pen, 16, out}, is64), 16u);
        EXPECT_EQ(memory_.read_u32(out + 0).value(), 1u);
        EXPECT_EQ(memory_.read_u32(out + 4).value(), 4u);
        EXPECT_EQ(memory_.read_u32(out + 12).value(), 0x00AABBCCu);
        EXPECT_EQ(call("DeleteObject", {pen}, is64), 1u);
    }
}

}  // namespace
