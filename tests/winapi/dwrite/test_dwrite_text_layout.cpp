// SPDX-License-Identifier: LGPL-2.1-only
// IDWriteTextFormat / IDWriteTextLayout against the Windows measurements in
// tools/oracle_probe/dwrite_text_layout_probe{,32}.expected (Arial, 20 DIP,
// en-us). Every expected number below is quoted from that file.
#include "dwrite_manager_harness.hpp"

#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace vitreo::winapi::dwrite_test {
namespace {

using namespace dwrite_text_contract;
constexpr u32 S_OK_ = 0, E_INVALIDARG_ = 0x80070057u, E_FAIL_ = 0x80004005u,
              E_NOT_SUFFICIENT_BUFFER_ = 0x8007007Au;

bool host_has_arial() {
    return dwrite::match_system_font(u"Arial", 400, 5, 0).has_value() &&
           dwrite::match_system_font(u"Arial", 400, 5, 0)->family_name == u"Arial";
}

u32 bits(float value) {
    u32 out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}
float as_float(u32 value) {
    float out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

/// A DWriteManager driven like a guest of either width.
struct TextHarness {
    GuestMemory memory;
    cpu::CpuState cpu{};
    DWriteManager manager;
    bool wide;
    static constexpr GuestAddr BASE = 0x20000, STACK = 0x2F000;
    // Fake guest renderer: an object whose vtable points at host-handled
    // "functions" 0xA000 + slot.
    static constexpr GuestAddr RENDERER = BASE + 0x8000, RENDERER_VTABLE = BASE + 0x8100;
    std::vector<std::pair<u32, std::vector<u64>>> calls;
    std::map<u32, u32> fail_slot;  // slot -> HRESULT to return
    std::vector<u32> glyph_counts;  // per DrawGlyphRun

    explicit TextHarness(bool x64) : manager(memory, x64), wide(x64) {
        EXPECT_TRUE(memory.map_region(BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu.mode = x64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        cpu.gpr.rsp = STACK;
        manager.set_method_thunks(DWriteManagerTest::method_thunks());
        for (u32 slot = 0; slot < 10; ++slot) put_ptr(RENDERER_VTABLE + slot * ptr(), 0xA000 + slot);
        put_ptr(RENDERER, RENDERER_VTABLE);
        manager.set_guest_call([this](GuestAddr function, const u64* args, usize count, bool) -> u64 {
            // Only the fake renderer's slots are guest code here; the
            // manager's own objects (a transient face's Release) are not.
            if (function < 0xA000 || function >= 0xA010) return 0;
            const u32 slot = static_cast<u32>(function - 0xA000);
            calls.push_back({slot, std::vector<u64>(args, args + count)});
            if (slot == 6) {  // DrawGlyphRun: the run lives only during the call
                glyph_counts.push_back(memory.read_u32(args[5] + offset_GLYPH_RUN_glyphCount[wide ? 1 : 0]).value_or(~0u));
            }
            if (fail_slot.contains(slot)) return fail_slot[slot];
            if (slot == 0) return 0x80004002u;  // QueryInterface: E_NOINTERFACE
            if (slot == 1 || slot == 2) return 1;
            if (slot == 3) (void)memory.write_u32(args[2], 0);         // snapping enabled
            if (slot == 5) (void)memory.write_u32(args[2], bits(1));   // ppd
            if (slot == 4) {                                           // identity
                const u32 m[6]{bits(1), 0, 0, bits(1), 0, 0};
                (void)memory.write_bytes(args[2], reinterpret_cast<const Byte*>(m), sizeof(m));
            }
            return 0;
        });
    }
    usize ptr() const { return wide ? 8 : 4; }
    void put_ptr(GuestAddr at, GuestAddr value) {
        if (wide) (void)memory.write_u64(at, value);
        else (void)memory.write_u32(at, static_cast<u32>(value));
    }
    GuestAddr get_ptr(GuestAddr at) {
        return wide ? memory.read_u64(at).value_or(0) : memory.read_u32(at).value_or(0);
    }
    /// Arguments as a guest pushes them; values tagged float also go to XMM
    /// on x64. A DWRITE_TEXT_RANGE is passed as two words via range().
    struct Arg {
        u64 value;
        bool is_float{false};
        bool is_range{false};
        u32 length{0};
    };
    static Arg f(float v) { return {bits(v), true}; }
    static Arg range(u32 start, u32 length) { return {start, false, true, length}; }
    u32 call(const std::function<void(Marshaler&)>& method, std::initializer_list<Arg> args) {
        std::vector<std::pair<u64, bool>> words;
        for (const Arg& a : args) {
            if (a.is_range && wide) words.push_back({a.value | (u64{a.length} << 32), false});
            else if (a.is_range) {
                words.push_back({a.value, false});
                words.push_back({a.length, false});
            } else words.push_back({a.value, a.is_float});
        }
        cpu.gpr.rax = 0;
        cpu.pending_raise.active = false;
        for (usize i = 0; i < words.size(); ++i) {
            const u64 v = words[i].first;
            if (wide && i < 4) {
                u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
                *regs[i] = v;
                cpu.sse.xmm[i].u64_[0] = v;
            } else if (wide) {
                (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, v);
            } else {
                (void)memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(v));
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        method(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    GuestAddr text(const std::u16string& value, GuestAddr at) {
        (void)memory.write_bytes(at, reinterpret_cast<const Byte*>(value.data()), value.size() * 2);
        (void)memory.write_u16(at + value.size() * 2, 0);
        return at;
    }
    GuestAddr factory() {
        const GuestAddr iid = BASE + 0x100, out = BASE + 0x200;
        (void)memory.write_bytes(iid, reinterpret_cast<const Byte*>(&IID_IDWRITE_FACTORY), 16);
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_DWriteCreateFactory(m); }, {{0}, {iid}, {out}}), S_OK_);
        return get_ptr(out);
    }
    u32 create_format(GuestAddr factory, GuestAddr family, float size, GuestAddr out,
                      u32 weight = 400, u32 style = 0, u32 stretch = 5, bool locale = true) {
        const GuestAddr loc = locale ? text(u"en-us", BASE + 0x400) : 0;
        return call([&](Marshaler& m) { manager.impl_CreateTextFormat(m); },
                    {{factory}, {family}, {0}, {weight}, {style}, {stretch}, f(size), {loc}, {out}});
    }
    GuestAddr arial_format(GuestAddr factory) {
        const GuestAddr out = BASE + 0x500;
        EXPECT_EQ(create_format(factory, text(u"Arial", BASE + 0x300), 20, out), S_OK_);
        return get_ptr(out);
    }
    GuestAddr layout(GuestAddr factory, GuestAddr format, const std::u16string& value,
                     float width = 1000, float height = 1000) {
        const GuestAddr string = text(value, BASE + 0x1000), out = BASE + 0x600;
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_CreateTextLayout(m); },
                       {{factory}, {string}, {value.size()}, {format}, f(width), f(height), {out}}),
                  S_OK_);
        return get_ptr(out);
    }
    u32 layout_method(GuestAddr object, u32 slot, std::initializer_list<Arg> rest) {
        std::vector<Arg> all{{object}};
        all.insert(all.end(), rest.begin(), rest.end());
        return call_vector([&](Marshaler& m) { manager.text_method(m, DWriteManager::TextInterface::Layout, slot); }, all);
    }
    u32 format_method(GuestAddr object, u32 slot, std::initializer_list<Arg> rest) {
        std::vector<Arg> all{{object}};
        all.insert(all.end(), rest.begin(), rest.end());
        return call_vector([&](Marshaler& m) { manager.text_method(m, DWriteManager::TextInterface::Format, slot); }, all);
    }
    u32 call_vector(const std::function<void(Marshaler&)>& method, const std::vector<Arg>& args) {
        switch (args.size()) {
        case 1: return call(method, {args[0]});
        case 2: return call(method, {args[0], args[1]});
        case 3: return call(method, {args[0], args[1], args[2]});
        case 4: return call(method, {args[0], args[1], args[2], args[3]});
        case 5: return call(method, {args[0], args[1], args[2], args[3], args[4]});
        case 6: return call(method, {args[0], args[1], args[2], args[3], args[4], args[5]});
        default: return call(method, {args[0], args[1], args[2], args[3], args[4], args[5], args[6]});
        }
    }
    float metric(GuestAddr at, usize index) { return as_float(memory.read_u32(at + index * 4).value_or(0)); }
};

class DWriteTextLayoutTest : public ::testing::TestWithParam<bool> {};

TEST_P(DWriteTextLayoutTest, CreateTextFormatArgumentRulesMatchWindows) {
    TextHarness h(GetParam());
    const GuestAddr factory = h.factory();
    const GuestAddr arial = h.text(u"Arial", TextHarness::BASE + 0x300);
    const GuestAddr out = TextHarness::BASE + 0x500;
    struct Case {
        const char* name;
        GuestAddr family;
        float size;
        u32 weight, style, stretch;
        bool locale;
        u32 hr;
    };
    const float nan = std::nanf("");
    // probe "format <name> hr=..."
    const Case cases[] = {
        {"valid", arial, 20, 400, 0, 5, true, S_OK_},
        {"emptyFamily", h.text(u"", TextHarness::BASE + 0x380), 20, 400, 0, 5, true, S_OK_},
        {"nullFamily", 0, 20, 400, 0, 5, true, E_INVALIDARG_},
        {"size0", arial, 0, 400, 0, 5, true, E_INVALIDARG_},
        {"sizeNeg", arial, -1, 400, 0, 5, true, E_INVALIDARG_},
        {"sizeNaN", arial, nan, 400, 0, 5, true, E_INVALIDARG_},
        {"sizeInf", arial, INFINITY, 400, 0, 5, true, S_OK_},
        {"weight0", arial, 20, 0, 0, 5, true, E_INVALIDARG_},
        {"weight999", arial, 20, 999, 0, 5, true, S_OK_},
        {"weight1000", arial, 20, 1000, 0, 5, true, E_INVALIDARG_},
        {"style3", arial, 20, 400, 3, 5, true, E_INVALIDARG_},
        {"stretch0", arial, 20, 400, 0, 0, true, E_INVALIDARG_},
        {"stretch10", arial, 20, 400, 0, 10, true, E_INVALIDARG_},
        {"nullLocale", arial, 20, 400, 0, 5, false, E_INVALIDARG_},
    };
    for (const Case& c : cases) {
        h.put_ptr(out, 0x1234);
        EXPECT_EQ(h.create_format(factory, c.family, c.size, out, c.weight, c.style, c.stretch, c.locale), c.hr)
            << c.name;
        if (c.hr != S_OK_) EXPECT_EQ(h.get_ptr(out), 0u) << c.name << " clears the out pointer";
        else EXPECT_NE(h.get_ptr(out), 0u) << c.name;
    }
}

TEST_P(DWriteTextLayoutTest, FormatGettersSettersAndShortBuffers) {
    TextHarness h(GetParam());
    const GuestAddr format = h.arial_format(h.factory());
    const GuestAddr buffer = TextHarness::BASE + 0x700;
    // probe: GetFontFamilyName(size=5) hr=8007007a buf='Aria\0'; size 6 is S_OK.
    EXPECT_EQ(h.format_method(format, text_format_GetFontFamilyName, {{buffer}, {5}}), E_NOT_SUFFICIENT_BUFFER_);
    EXPECT_EQ(h.memory.read_u16(buffer + 6).value(), u'a');
    EXPECT_EQ(h.memory.read_u16(buffer + 8).value(), 0u);
    EXPECT_EQ(h.format_method(format, text_format_GetFontFamilyName, {{buffer}, {6}}), S_OK_);
    // probe: the default tab stop is 80 (4 x em); 0 / -1 / NaN are rejected.
    h.format_method(format, text_format_GetIncrementalTabStop, {});
    EXPECT_FLOAT_EQ(as_float(GetParam() ? h.cpu.sse.xmm[0].u32_[0] : 0), GetParam() ? 80.f : 0.f);
    EXPECT_EQ(h.format_method(format, text_format_SetIncrementalTabStop, {TextHarness::f(0)}), E_INVALIDARG_);
    EXPECT_EQ(h.format_method(format, text_format_SetIncrementalTabStop, {TextHarness::f(-1)}), E_INVALIDARG_);
    EXPECT_EQ(h.format_method(format, text_format_SetIncrementalTabStop, {TextHarness::f(1)}), S_OK_);
    // probe: SetLineSpacing(1,-1,24) fails, (1,30,-1) succeeds, method 3 fails.
    EXPECT_EQ(h.format_method(format, text_format_SetLineSpacing, {{1}, TextHarness::f(-1), TextHarness::f(24)}),
              E_INVALIDARG_);
    EXPECT_EQ(h.format_method(format, text_format_SetLineSpacing, {{1}, TextHarness::f(30), TextHarness::f(-1)}),
              S_OK_);
    EXPECT_EQ(h.format_method(format, text_format_SetLineSpacing, {{3}, TextHarness::f(30), TextHarness::f(24)}),
              E_INVALIDARG_);
    // Every out-of-range enum setter fails and leaves the value.
    EXPECT_EQ(h.format_method(format, text_format_SetTextAlignment, {{4}}), E_INVALIDARG_);
    EXPECT_EQ(h.format_method(format, text_format_SetWordWrapping, {{5}}), E_INVALIDARG_);
    EXPECT_EQ(h.format_method(format, text_format_GetWordWrapping, {}), 0u);
}

TEST_P(DWriteTextLayoutTest, MetricsLinesAndClustersMatchWindows) {
    if (!host_has_arial()) GTEST_SKIP() << "host has no Arial";
    TextHarness h(GetParam());
    const GuestAddr factory = h.factory();
    const GuestAddr format = h.arial_format(factory);
    const GuestAddr metrics = TextHarness::BASE + 0x2000, actual = TextHarness::BASE + 0x2F00;
    // probe ['Hello'] GetMetrics width=45.5762 height=22.998 lineCount=1;
    // line len=5 height=22.998 baseline=18.7598; clusters 14.4434, 11.123.
    GuestAddr layout = h.layout(factory, format, u"Hello");
    ASSERT_EQ(h.layout_method(layout, text_layout_GetMetrics, {{metrics}}), S_OK_);
    EXPECT_NEAR(h.metric(metrics, 2), 45.5762, 1e-3);
    EXPECT_NEAR(h.metric(metrics, 3), 45.5762, 1e-3);
    EXPECT_NEAR(h.metric(metrics, 4), 22.998, 1e-3);
    EXPECT_EQ(h.memory.read_u32(metrics + 32).value(), 1u);
    EXPECT_EQ(h.layout_method(layout, text_layout_GetLineMetrics, {{0}, {0}, {actual}}), E_NOT_SUFFICIENT_BUFFER_);
    EXPECT_EQ(h.memory.read_u32(actual).value(), 1u);
    ASSERT_EQ(h.layout_method(layout, text_layout_GetLineMetrics, {{metrics}, {4}, {actual}}), S_OK_);
    EXPECT_EQ(h.memory.read_u32(metrics).value(), 5u);
    EXPECT_NEAR(h.metric(metrics, 3), 22.998, 1e-3);
    EXPECT_NEAR(h.metric(metrics, 4), 18.7598, 1e-3);
    ASSERT_EQ(h.layout_method(layout, text_layout_GetClusterMetrics, {{metrics}, {16}, {actual}}), S_OK_);
    EXPECT_EQ(h.memory.read_u32(actual).value(), 5u);
    EXPECT_NEAR(h.metric(metrics, 0), 14.4434, 1e-3);
    EXPECT_NEAR(h.metric(metrics, 2), 11.123, 1e-3);

    // probe ['Hello   ']: width 45.5762, widthIncl 62.2461.
    layout = h.layout(factory, format, u"Hello   ");
    ASSERT_EQ(h.layout_method(layout, text_layout_GetMetrics, {{metrics}}), S_OK_);
    EXPECT_NEAR(h.metric(metrics, 2), 45.5762, 1e-3);
    EXPECT_NEAR(h.metric(metrics, 3), 62.2461, 1e-3);
    // probe: "Hello\n" has 2 lines and height 45.9961; CRLF is one line of
    // length 7 with trailing whitespace 2 and newline 2.
    layout = h.layout(factory, format, u"Hello\n");
    ASSERT_EQ(h.layout_method(layout, text_layout_GetMetrics, {{metrics}}), S_OK_);
    EXPECT_NEAR(h.metric(metrics, 4), 45.9961, 1e-3);
    EXPECT_EQ(h.memory.read_u32(metrics + 32).value(), 2u);
    layout = h.layout(factory, format, u"Hello\r\nworld");
    ASSERT_EQ(h.layout_method(layout, text_layout_GetLineMetrics, {{metrics}, {4}, {actual}}), S_OK_);
    EXPECT_EQ(h.memory.read_u32(metrics + 0).value(), 7u);
    EXPECT_EQ(h.memory.read_u32(metrics + 4).value(), 2u);
    EXPECT_EQ(h.memory.read_u32(metrics + 8).value(), 2u);
}

TEST_P(DWriteTextLayoutTest, RangesAreStoredPastTheTextAndReportTheirRun) {
    TextHarness h(GetParam());
    const GuestAddr factory = h.factory();
    const GuestAddr layout = h.layout(factory, h.arial_format(factory), u"Hello world");
    const GuestAddr out = TextHarness::BASE + 0x700, range = TextHarness::BASE + 0x780;
    // probe: SetFontSize(30,{6,5}); SetFontFamilyName({8,100}) -> runs
    // {0,6} {6,2} {8,3} {11,97} {108, 4294967187}.
    ASSERT_EQ(h.layout_method(layout, text_layout_SetFontSize, {TextHarness::f(30), TextHarness::range(6, 5)}), S_OK_);
    const GuestAddr family = h.text(u"NoSuchFamilyVi", TextHarness::BASE + 0x800);
    ASSERT_EQ(h.layout_method(layout, text_layout_SetFontFamilyName, {{family}, TextHarness::range(8, 100)}), S_OK_);
    const auto run_at = [&](u32 position) {
        EXPECT_EQ(h.layout_method(layout, text_layout_GetFontFamilyNameLength_45, {{position}, {out}, {range}}), S_OK_);
        return std::pair{h.memory.read_u32(range).value(), h.memory.read_u32(range + 4).value()};
    };
    EXPECT_EQ(run_at(0), (std::pair<u32, u32>{0, 6}));
    EXPECT_EQ(run_at(7), (std::pair<u32, u32>{6, 2}));
    EXPECT_EQ(run_at(8), (std::pair<u32, u32>{8, 3}));
    EXPECT_EQ(run_at(50), (std::pair<u32, u32>{11, 97}));
    EXPECT_EQ(run_at(200), (std::pair<u32, u32>{108, 4294967187u}));
    EXPECT_EQ(h.memory.read_u32(out).value(), 5u);
    // probe: a range that overflows UINT32 fails; an empty one succeeds.
    EXPECT_EQ(h.layout_method(layout, text_layout_SetFontWeight, {{700}, TextHarness::range(0xFFFFFFF0u, 0x20)}),
              E_INVALIDARG_);
    EXPECT_EQ(h.layout_method(layout, text_layout_SetFontWeight, {{700}, TextHarness::range(3, 0)}), S_OK_);
}

TEST_P(DWriteTextLayoutTest, DrawFollowsTheWindowsCallbackProtocol) {
    if (!host_has_arial()) GTEST_SKIP() << "host has no Arial";
    TextHarness h(GetParam());
    const GuestAddr factory = h.factory();
    const GuestAddr layout = h.layout(factory, h.arial_format(factory), u"Hello\nworld");
    const GuestAddr context = 0x5150;
    // probe ['Hello\nworld']: QI(IDWriteTextRenderer1), IsPixelSnappingDisabled,
    // GetPixelsPerDip, GetCurrentTransform, then DrawGlyphRun 'Hello' at
    // (0,19) [baseline snapped], the newline (0 glyphs) at (45.5762,19),
    // 'world' at (0,42).
    ASSERT_EQ(h.layout_method(layout, text_layout_Draw, {{context}, {TextHarness::RENDERER}, TextHarness::f(0),
                                                         TextHarness::f(0)}),
              S_OK_);
    std::vector<u32> slots;
    for (const auto& [slot, args] : h.calls) slots.push_back(slot);
    EXPECT_EQ(slots, (std::vector<u32>{0, 3, 5, 4, 6, 6, 6}));
    ASSERT_EQ(h.calls.size(), 7u);
    for (usize i = 1; i < h.calls.size(); ++i) EXPECT_EQ(h.calls[i].second[1], context);
    const auto origin = [&](usize call) {
        return std::pair{as_float(static_cast<u32>(h.calls[call].second[2])),
                         as_float(static_cast<u32>(h.calls[call].second[3]))};
    };
    EXPECT_FLOAT_EQ(origin(4).second, 19.f);
    EXPECT_NEAR(origin(5).first, 45.5762, 1e-3);
    EXPECT_FLOAT_EQ(origin(6).second, 42.f);
    EXPECT_EQ(h.glyph_counts, (std::vector<u32>{5, 0, 5}));
    // probe fail0: a failing IsPixelSnappingDisabled aborts Draw with it;
    // fail3: a failing DrawGlyphRun is ignored.
    h.calls.clear();
    h.fail_slot = {{3, E_FAIL_}};
    EXPECT_EQ(h.layout_method(layout, text_layout_Draw, {{context}, {TextHarness::RENDERER}, TextHarness::f(0),
                                                         TextHarness::f(0)}),
              E_FAIL_);
    h.fail_slot = {{6, E_FAIL_}};
    EXPECT_EQ(h.layout_method(layout, text_layout_Draw, {{context}, {TextHarness::RENDERER}, TextHarness::f(0),
                                                         TextHarness::f(0)}),
              S_OK_);
    EXPECT_EQ(h.layout_method(layout, text_layout_Draw, {{context}, {0}, TextHarness::f(0), TextHarness::f(0)}),
              E_INVALIDARG_);
}

INSTANTIATE_TEST_SUITE_P(BothWidths, DWriteTextLayoutTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

}  // namespace
}  // namespace vitreo::winapi::dwrite_test
