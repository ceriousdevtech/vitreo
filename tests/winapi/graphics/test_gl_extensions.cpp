/// @file test_gl_extensions.cpp
/// @brief The extensions GDI Generic advertises, through the registered
///        exports and the entry points wglGetProcAddress serves, in both
///        bitnesses: GL_EXT_bgra through glTexImage / glTexSubImage /
///        glGetTexImage, GL_EXT_paletted_texture (colour tables, index
///        textures), glDrawRangeElementsWIN's errors and glAddSwapHintRectWIN's
///        (what a hint does to SwapBuffers, and drawing, are
///        test_gl_extensions_metal.cpp's).
///
/// Expected values are the registry specs' and, where they leave a value
/// open, GDI Generic's (tools/oracle_probe/gl11_state_probe64.expected,
/// ext-* tags cited at each assertion). Contexts here have no drawable.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

u32 fbits(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct I { u32 v; };
struct F { float v; };
struct P { GuestAddr v; };
using Arg = std::variant<I, F, P>;

class Harness {
public:
    static constexpr GuestAddr MEM = 0x100000;
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr OUT = MEM + 0x100;
    static constexpr GuestAddr DATA = MEM + 0x1000;
    static constexpr GuestAddr DST = MEM + 0x8000;
    static constexpr GuestAddr NAME = MEM + 0xF000;

    explicit Harness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(MEM, 0x40000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(STACK, 0x2000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        registry = std::make_unique<ThunkRegistry>(0x70000000);
        gl.set_thunk_registry(registry.get());
        EXPECT_TRUE(registry->register_dll("opengl32.dll", create_opengl32_thunks(gl)).is_ok());
    }

    /// The thunk wglGetProcAddress returns for @p name.
    GuestAddr proc(const char* name) {
        (void)memory.write_bytes(NAME, reinterpret_cast<const u8*>(name), std::strlen(name) + 1);
        return static_cast<GuestAddr>(call_at(address_of("opengl32.dll", "wglGetProcAddress"), {P{NAME}}));
    }

    GuestAddr address_of(const char* dll, const char* name) {
        auto addr = registry->get_thunk_address(dll, name);
        EXPECT_TRUE(addr.is_ok()) << name;
        return addr.is_ok() ? addr.value() : 0;
    }

    /// An opengl32 export, or an extension entry point (by its wglGetProcAddress pointer).
    u64 call(std::string_view name, std::vector<Arg> args) {
        const std::string n(name);
        const bool ext = n.find("EXT") != std::string::npos || n.find("WIN") != std::string::npos;
        const GuestAddr addr = ext ? proc(n.c_str()) : address_of("opengl32.dll", n.c_str());
        EXPECT_NE(addr, 0u) << n;
        return call_at(addr, std::move(args));
    }

    u64 call_at(GuestAddr addr, std::vector<Arg> args) {
        auto handler = registry->resolve_handler(addr);
        EXPECT_TRUE(handler.is_ok());
        if (!handler.is_ok()) return ~0ull;
        const GuestAddr frame = STACK + 0x1000;
        cpu.set_sp(frame);
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        cpu.gpr.rcx = cpu.gpr.rdx = cpu.gpr.r8 = cpu.gpr.r9 = 0xDDDDDDDDDDDDDDDDull;
        for (auto& x : cpu.sse.xmm) x.u64_[0] = x.u64_[1] = 0xEEEEEEEEEEEEEEEEull;
        if (m_x64) {
            (void)memory.write_u64(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                u64 bits = 0;
                bool fp = false;
                if (auto* a = std::get_if<I>(&args[i])) bits = 0xABCD000000000000ull | a->v;
                else if (auto* f = std::get_if<F>(&args[i])) { bits = 0x7777777700000000ull | fbits(f->v); fp = true; }
                else bits = std::get<P>(args[i]).v;
                if (i < 4) {
                    if (fp) cpu.sse.xmm[i].u64_[0] = bits;
                    else (i == 0 ? cpu.gpr.rcx : i == 1 ? cpu.gpr.rdx : i == 2 ? cpu.gpr.r8 : cpu.gpr.r9) = bits;
                } else {
                    (void)memory.write_u64(frame + 0x28 + (i - 4) * 8, bits);
                }
            }
        } else {
            (void)memory.write_u32(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                u32 v = 0;
                if (auto* a = std::get_if<I>(&args[i])) v = a->v;
                else if (auto* f = std::get_if<F>(&args[i])) v = fbits(f->v);
                else v = static_cast<u32>(std::get<P>(args[i]).v);
                (void)memory.write_u32(frame + 4 + static_cast<GuestAddr>(i * 4), v);
            }
            EXPECT_EQ(registry->resolve_stdcall_bytes(addr), args.size() * 4);
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (*handler.value())(m);
        return m_x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    u32 err() { return static_cast<u32>(call("glGetError", {})); }
    i32 tex_level(u32 target, i32 level, u32 pname) {
        (void)memory.write_u32(OUT, 0xFFFFFFFFu);
        call("glGetTexLevelParameteriv", {I{target}, I{static_cast<u32>(level)}, I{pname}, P{OUT}});
        return static_cast<i32>(memory.read_u32(OUT).value());
    }
    i32 table_param(u32 target, u32 pname) {
        (void)memory.write_u32(OUT, 0xFFFFFFF9u);   // -7, the probe's sentinel
        call("glGetColorTableParameterivEXT", {I{target}, I{pname}, P{OUT}});
        return static_cast<i32>(memory.read_u32(OUT).value());
    }
    float table_param_f(u32 target, u32 pname) {
        (void)memory.write_u32(OUT, fbits(-7.0f));
        call("glGetColorTableParameterfvEXT", {I{target}, I{pname}, P{OUT}});
        float f;
        const u32 bits = memory.read_u32(OUT).value();
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void put(GuestAddr at, const std::vector<u8>& bytes) { (void)memory.write_bytes(at, bytes.data(), bytes.size()); }
    void fill(GuestAddr at, usize n, u8 v) { put(at, std::vector<u8>(n, v)); }
    std::vector<u8> bytes(GuestAddr at, usize n) {
        std::vector<u8> v(n);
        EXPECT_TRUE(memory.read_bytes(at, v.data(), n).is_ok());
        return v;
    }
    /// glGetTexImage into DST (pre-filled with 0xCC), first @p n bytes.
    std::vector<u8> tex_image(u32 target, u32 format, u32 type, usize n) {
        fill(DST, 256, 0xCC);
        call("glGetTexImage", {I{target}, I{0}, I{format}, I{type}, P{DST}});
        return bytes(DST, n);
    }
    std::vector<u8> table(u32 format, u32 type, usize n) {
        fill(DST, 512, 0xCC);
        call("glGetColorTableEXT", {I{GL_TEXTURE_2D}, I{format}, I{type}, P{DST}});
        return bytes(DST, n);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager gl;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

struct CurrentContext {
    CurrentContext() : ctx(GlFramebufferConfig{}, nullptr) {
        ctx.on_first_make_current(320, 200);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

std::vector<u8> hex(std::string_view s) {
    std::vector<u8> out;
    std::istringstream in{std::string(s)};
    std::string tok;
    while (in >> tok) out.push_back(static_cast<u8>(std::stoul(tok, nullptr, 16)));
    return out;
}

/// The probe's source images: src[i] = i * 13 + 7 (bgra), pal[i] = i * 7 + 3 (paletted).
std::vector<u8> ramp(usize n, u32 mul, u32 add) {
    std::vector<u8> v(n);
    for (usize i = 0; i < n; ++i) v[i] = static_cast<u8>(i * mul + add);
    return v;
}

class GlExtensionsTest : public ::testing::TestWithParam<bool> {
protected:
    Harness h{GetParam()};
    CurrentContext current;

    void SetUp() override {
        h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
    }
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

constexpr u32 kTex = 1;

// ---- GL_EXT_bgra -------------------------------------------------------------------------

TEST_P(GlExtensionsTest, BgraFormatsThroughTextureImages) {
    h.put(Harness::DATA, ramp(64, 13, 7));
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    struct Fmt {
        u32 format;
        const char* tag;
        const char* as_rgba;   // ext-bgra-*-texels-as-rgba
        const char* as_self;   // ext-bgra-gettexImage-as-*
        usize self_bytes;
    };
    const Fmt fmts[] = {
        {GL_BGR_EXT, "BGR", "21 14 07 ff 48 3b 2e ff 6f 62 55 ff 96 89 7c ff",
         "07 14 21 2e 3b 48 55 62 6f 7c 89 96 cc cc cc cc cc cc", 18},
        {GL_BGRA_EXT, "BGRA", "21 14 07 2e 55 48 3b 62 89 7c 6f 96 bd b0 a3 ca",
         "07 14 21 2e 3b 48 55 62 6f 7c 89 96 a3 b0 bd ca cc cc", 18},
    };
    for (const Fmt& f : fmts) {
        h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{4}, I{2}, I{2}, I{0}, I{f.format}, I{GL_UNSIGNED_BYTE},
                                P{Harness::DATA}});
        EXPECT_EQ(h.err(), GL_NO_ERROR) << f.tag;   // ext-bgra-teximage2d-*-ubyte-2x2
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 2) << f.tag;
        EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 16), hex(f.as_rgba)) << f.tag;
        EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, f.format, GL_UNSIGNED_BYTE, f.self_bytes), hex(f.as_self)) << f.tag;
        // ext-bgra-teximage2d-*-float: (0.25, 0.5, 0.75, 1) as B, G, R(, A).
        const float fl[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        h.put(Harness::DATA + 0x100, std::vector<u8>(reinterpret_cast<const u8*>(fl), reinterpret_cast<const u8*>(fl) + 16));
        h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{4}, I{1}, I{1}, I{0}, I{f.format}, I{GL_FLOAT},
                                P{Harness::DATA + 0x100}});
        EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 4), hex("bf 80 40 ff")) << f.tag;
        // ext-bgra-teximage2d-*-bitmap: INVALID_ENUM.
        h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{4}, I{1}, I{1}, I{0}, I{f.format}, I{GL_BITMAP},
                                P{Harness::DATA}});
        EXPECT_EQ(h.err(), GL_INVALID_ENUM) << f.tag;
        // ext-bgra-drawpixels-*-bitmap INVALID_ENUM, -readpixels-*-bitmap INVALID_OPERATION.
        h.call("glDrawPixels", {I{1}, I{1}, I{f.format}, I{GL_BITMAP}, P{Harness::DATA}});
        EXPECT_EQ(h.err(), GL_INVALID_ENUM) << f.tag;
        h.call("glReadPixels", {I{0}, I{0}, I{1}, I{1}, I{f.format}, I{GL_BITMAP}, P{Harness::DST}});
        EXPECT_EQ(h.err(), GL_INVALID_OPERATION) << f.tag;
    }
    // ext-bgra-teximage1d-*-ushort.
    h.call("glBindTexture", {I{GL_TEXTURE_1D}, I{kTex + 1}});
    h.put(Harness::DATA, ramp(64, 13, 7));
    h.call("glTexImage1D", {I{GL_TEXTURE_1D}, I{0}, I{3}, I{2}, I{0}, I{GL_BGR_EXT}, I{GL_UNSIGNED_SHORT},
                            P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_1D, GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("48 2e 14 ff 96 7c 62 ff"));
    // ext-bgra-teximage1d-BGRA-ushort reads back 48 2e 14 62 b0 96 7c ca: GDI
    // Generic keeps the source alpha in a 3-component texture. The spec's
    // RGB base format has alpha 1 (Table 6.1), which Vitreo returns.
    h.call("glTexImage1D", {I{GL_TEXTURE_1D}, I{0}, I{3}, I{2}, I{0}, I{GL_BGRA_EXT}, I{GL_UNSIGNED_SHORT},
                            P{Harness::DATA}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_1D, GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("48 2e 14 ff b0 96 7c ff"));
    h.call("glTexImage1D", {I{GL_TEXTURE_1D}, I{0}, I{4}, I{2}, I{0}, I{GL_BGRA_EXT}, I{GL_UNSIGNED_SHORT},
                            P{Harness::DATA}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_1D, GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("48 2e 14 62 b0 96 7c ca"));
    // Not internal formats (ext-bgra-internalformat-*, -copyteximage-internalformat-BGRA).
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_BGRA_EXT}, I{1}, I{1}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_BGR_EXT}, I{1}, I{1}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    h.call("glCopyTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_BGRA_EXT}, I{0}, I{0}, I{1}, I{1}, I{0}});
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    // glTexSubImage2D with BGRA then BGR (ext-bgra-texsubimage2d-*).
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{4}, I{2}, I{2}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{Harness::DATA}});
    h.put(Harness::DATA + 0x200, {1, 2, 3, 4});
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{1}, I{0}, I{1}, I{1}, I{GL_BGRA_EXT}, I{GL_UNSIGNED_BYTE},
                               P{Harness::DATA + 0x200}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 16),
              hex("07 14 21 2e 03 02 01 04 6f 7c 89 96 a3 b0 bd ca"));
    h.put(Harness::DATA + 0x200, {10, 20, 30});
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{0}, I{1}, I{1}, I{1}, I{GL_BGR_EXT}, I{GL_UNSIGNED_BYTE},
                               P{Harness::DATA + 0x200}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 16),
              hex("07 14 21 2e 03 02 01 04 1e 14 0a ff a3 b0 bd ca"));
}

// ---- GL_EXT_paletted_texture: the colour table -------------------------------------------

TEST_P(GlExtensionsTest, ColorTableStateAndErrors) {
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    // ext-pal-initial-*: one entry, GL_RGBA, 8-bit components; the luminance
    // and intensity sizes and unknown pnames are INVALID_ENUM.
    for (u32 target : {GL_TEXTURE_2D, GL_TEXTURE_1D, GL_PROXY_TEXTURE_2D}) {
        EXPECT_EQ(h.table_param(target, GL_COLOR_TABLE_FORMAT_EXT), static_cast<i32>(GL_RGBA));
        EXPECT_EQ(h.table_param(target, GL_COLOR_TABLE_WIDTH_EXT), 1);
        EXPECT_EQ(h.table_param_f(target, GL_COLOR_TABLE_WIDTH_EXT), 1.0f);
        for (u32 p : {GL_COLOR_TABLE_RED_SIZE_EXT, GL_COLOR_TABLE_GREEN_SIZE_EXT, GL_COLOR_TABLE_BLUE_SIZE_EXT,
                      GL_COLOR_TABLE_ALPHA_SIZE_EXT}) {
            EXPECT_EQ(h.table_param(target, p), 8);
        }
        EXPECT_EQ(h.err(), GL_NO_ERROR);
        for (u32 p : {GL_COLOR_TABLE_LUMINANCE_SIZE_EXT, GL_COLOR_TABLE_INTENSITY_SIZE_EXT, 0x1234u}) {
            EXPECT_EQ(h.table_param(target, p), -7);
            EXPECT_EQ(h.err(), GL_INVALID_ENUM);
        }
    }
    h.put(Harness::DATA, ramp(4096, 7, 3));
    auto table = [&](u32 target, u32 internal, i32 width, u32 format, u32 type) {
        h.call("glColorTableEXT", {I{target}, I{internal}, I{static_cast<u32>(width)}, I{format}, I{type},
                                   P{Harness::DATA}});
        return h.err();
    };
    // ext-pal-colortable-internal-*.
    struct Internal {
        u32 f;
        u32 err;
    };
    const Internal internals[] = {{GL_RGBA, 0}, {GL_RGB, 0}, {4, 0}, {3, 0}, {GL_RGBA8, 0}, {GL_RGB8, 0},
                                  {GL_RGBA4, 0}, {GL_LUMINANCE, GL_INVALID_ENUM}, {GL_ALPHA, GL_INVALID_ENUM},
                                  {GL_INTENSITY, GL_INVALID_ENUM}, {GL_BGRA_EXT, GL_INVALID_ENUM}, {1, GL_INVALID_ENUM},
                                  {0x1234, GL_INVALID_ENUM}};
    u32 last_ok = GL_RGBA;
    for (const Internal& in : internals) {
        EXPECT_EQ(table(GL_TEXTURE_2D, in.f, 4, GL_RGBA, GL_UNSIGNED_BYTE), in.err) << in.f;
        if (in.err == 0) last_ok = in.f;
        EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_FORMAT_EXT), static_cast<i32>(last_ok)) << in.f;
        EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_RED_SIZE_EXT), 8);
        EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_ALPHA_SIZE_EXT), 8);   // even for RGB tables
    }
    // ext-pal-colortable-width-*: powers of two.
    struct Width {
        i32 w;
        u32 err;
        i32 after;
    };
    const Width widths[] = {{0, GL_INVALID_VALUE, 4}, {1, 0, 1},   {2, 0, 2},     {3, GL_INVALID_VALUE, 2},
                            {4, 0, 4},                {5, GL_INVALID_VALUE, 4}, {16, 0, 16}, {255, GL_INVALID_VALUE, 16},
                            {256, 0, 256},            {257, GL_INVALID_VALUE, 256}, {512, 0, 512}, {1024, 0, 1024},
                            {-1, GL_INVALID_VALUE, 1024}};
    for (const Width& w : widths) {
        EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, w.w, GL_RGBA, GL_UNSIGNED_BYTE), w.err) << w.w;
        EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), w.after) << w.w;
    }
    // Source formats and types (ext-pal-colortable-<format>-<type>, read back as RGBA ubyte).
    struct Src {
        u32 format, type;
        u32 err;
        const char* table;
    };
    const float fpal[16] = {0.0f, 0.25f, 0.5f, 1.0f, 1.0f, 0.75f, 0.5f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.9f, 0.8f, 0.7f, 0.6f};
    const Src srcs[] = {
        {GL_RGB, GL_UNSIGNED_BYTE, 0, "03 0a 11 ff 18 1f 26 ff 2d 34 3b ff 42 49 50 ff"},
        {GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, "11 0a 03 18 2d 26 1f 34 49 42 3b 50 65 5e 57 6c"},
        {GL_BGR_EXT, GL_UNSIGNED_BYTE, 0, "11 0a 03 ff 26 1f 18 ff 3b 34 2d ff 50 49 42 ff"},
        {GL_LUMINANCE, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "11 0a 03 ff 26 1f 18 ff 3b 34 2d ff 50 49 42 ff"},
        {GL_RGBA, GL_FLOAT, 0, "00 40 80 ff ff bf 80 00 1a 33 4d 66 e6 cc b3 99"},
        {GL_RGBA, GL_UNSIGNED_SHORT, 0, "0a 18 26 34 42 50 5e 6c 7a 88 96 a4 b2 c0 ce dc"},
        {GL_RGBA, GL_BYTE, 0, "07 15 23 31 3f 4d 5b 69 77 85 93 a1 af bd cb d9"},
        {GL_RGBA, GL_BITMAP, GL_INVALID_ENUM, "07 15 23 31 3f 4d 5b 69 77 85 93 a1 af bd cb d9"},
        {GL_COLOR_INDEX, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "07 15 23 31 3f 4d 5b 69 77 85 93 a1 af bd cb d9"},
        {GL_RED, GL_UNSIGNED_BYTE, 0, "03 00 00 ff 0a 00 00 ff 11 00 00 ff 18 00 00 ff"},
        {0x1234, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "03 00 00 ff 0a 00 00 ff 11 00 00 ff 18 00 00 ff"},
        {GL_RGBA, 0x1234, GL_INVALID_ENUM, "03 00 00 ff 0a 00 00 ff 11 00 00 ff 18 00 00 ff"},
        {GL_GREEN, GL_UNSIGNED_BYTE, 0, "00 03 00 ff 00 0a 00 ff 00 11 00 ff 00 18 00 ff"},
        {GL_BLUE, GL_UNSIGNED_BYTE, 0, "00 00 03 ff 00 00 0a ff 00 00 11 ff 00 00 18 ff"},
        {GL_ALPHA, GL_UNSIGNED_BYTE, 0, "00 00 00 03 00 00 00 0a 00 00 00 11 00 00 00 18"},
        {GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "00 00 00 03 00 00 00 0a 00 00 00 11 00 00 00 18"},
        {GL_RGBA, GL_UNSIGNED_INT, 0, "18 34 50 6c 88 a4 c0 dc f8 14 30 4c 68 84 a0 bc"},
        {GL_RGBA, GL_SHORT, 0, "14 30 4c 68 84 a0 bc d8 f4 00 00 00 00 00 00 00"},
    };
    for (const Src& s : srcs) {
        if (s.type == GL_FLOAT) {
            h.put(Harness::DATA, std::vector<u8>(reinterpret_cast<const u8*>(fpal), reinterpret_cast<const u8*>(fpal) + 64));
        } else {
            h.put(Harness::DATA, ramp(4096, 7, 3));
        }
        EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, 4, s.format, s.type), s.err) << s.format << " " << s.type;
        EXPECT_EQ(h.table(GL_RGBA, GL_UNSIGNED_BYTE, 16), hex(s.table)) << s.format << " " << s.type;
    }
    h.put(Harness::DATA, ramp(4096, 7, 3));
    // Targets (ext-pal-colortable-target-*, -getcolortable-target-*).
    EXPECT_EQ(table(GL_TEXTURE_1D, GL_RGBA, 8, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    EXPECT_EQ(h.table_param(GL_TEXTURE_1D, GL_COLOR_TABLE_WIDTH_EXT), 8);
    h.fill(Harness::DST, 16, 0xCC);
    h.call("glGetColorTableEXT", {I{GL_TEXTURE_1D}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 8), hex("03 0a 11 18 1f 26 2d 34"));
    for (u32 proxy : {GL_PROXY_TEXTURE_2D, GL_PROXY_TEXTURE_1D}) {
        EXPECT_EQ(table(proxy, GL_RGBA, 8, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
        EXPECT_EQ(h.table_param(proxy, GL_COLOR_TABLE_WIDTH_EXT), 8);
        h.fill(Harness::DST, 16, 0xCC);
        h.call("glGetColorTableEXT", {I{proxy}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}});
        EXPECT_EQ(h.err(), GL_INVALID_ENUM);
        EXPECT_EQ(h.bytes(Harness::DST, 8), hex("cc cc cc cc cc cc cc cc"));
    }
    for (u32 bad : {0x1234u, GL_TEXTURE_ENV}) {
        EXPECT_EQ(table(bad, GL_RGBA, 8, GL_RGBA, GL_UNSIGNED_BYTE), GL_INVALID_ENUM);
        EXPECT_EQ(h.table_param(bad, GL_COLOR_TABLE_WIDTH_EXT), -7);
        EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    }
    // ext-pal-proxy-rgb-512 / -proxy-width3.
    EXPECT_EQ(table(GL_PROXY_TEXTURE_2D, GL_RGB, 512, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    EXPECT_EQ(h.table_param(GL_PROXY_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 512);
    EXPECT_EQ(h.table_param(GL_PROXY_TEXTURE_2D, GL_COLOR_TABLE_FORMAT_EXT), static_cast<i32>(GL_RGB));
    EXPECT_EQ(table(GL_PROXY_TEXTURE_2D, GL_RGB, 3, GL_RGBA, GL_UNSIGNED_BYTE), GL_INVALID_VALUE);
    EXPECT_EQ(h.table_param(GL_PROXY_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 512);
    // glGetColorTableEXT formats (ext-pal-getcolortable-*): a 4-entry RGBA table.
    EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, 4, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    struct Get {
        u32 format, type;
        u32 err;
        const char* bytes;
    };
    const Get gets[] = {
        {GL_RGBA, GL_UNSIGNED_BYTE, 0, "03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c cc cc cc cc"},
        {GL_RGB, GL_UNSIGNED_BYTE, 0, "03 0a 11 1f 26 2d 3b 42 49 57 5e 65 cc cc cc cc"},
        {GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, "11 0a 03 18 2d 26 1f 34 49 42 3b 50 65 5e 57 6c cc cc cc cc"},
        {GL_ALPHA, GL_UNSIGNED_BYTE, 0, "18 34 50 6c cc cc cc cc"},
        {GL_RED, GL_UNSIGNED_BYTE, 0, "03 1f 3b 57 cc cc cc cc"},
        {GL_BLUE, GL_UNSIGNED_BYTE, 0, "11 2d 49 65 cc cc cc cc"},
        {GL_BGR_EXT, GL_UNSIGNED_BYTE, 0, "11 0a 03 2d 26 1f 49 42 3b 65 5e 57 cc cc cc cc"},
        {GL_LUMINANCE, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "cc cc cc cc cc cc cc cc"},
        {GL_COLOR_INDEX, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "cc cc cc cc cc cc cc cc"},
        {GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "cc cc cc cc cc cc cc cc cc cc cc cc"},
        {0x1234, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "cc cc cc cc cc cc cc cc"},
        {GL_RGBA, GL_BITMAP, GL_INVALID_ENUM, "cc cc cc cc cc cc cc cc"},
        {GL_RGBA, GL_FLOAT, 0,
         "c2 c0 40 3c a1 a0 20 3d 89 88 88 3d c2 c0 c0 3d fa f8 f8 3d 99 98 18 3e b5 b4 34 3e d2 d0 50 3e "
         "ee ec 6c 3e 85 84 84 3e 93 92 92 3e a1 a0 a0 3e af ae ae 3e bd bc bc 3e cc ca ca 3e da d8 d8 3e "
         "cc cc cc cc"},
        {GL_RGBA, GL_SHORT, 0,
         "81 01 05 05 88 08 0c 0c 8f 0f 13 13 96 16 1a 1a 9d 1d 21 21 a4 24 28 28 ab 2b 2f 2f b2 32 36 36 "
         "cc cc cc cc"},
    };
    for (const Get& g : gets) {
        const auto want = hex(g.bytes);
        EXPECT_EQ(h.table(g.format, g.type, want.size()), want) << g.format << " " << g.type;
        EXPECT_EQ(h.err(), g.err) << g.format;
    }
    // An RGB table keeps the source alpha (ext-pal-colortable-RGB-16, getcolortable-RGBA).
    EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGB, 16, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_FORMAT_EXT), static_cast<i32>(GL_RGB));
    EXPECT_EQ(h.table(GL_RGBA, GL_UNSIGNED_BYTE, 4), hex("03 0a 11 18"));
    // glColorSubTableEXT (ext-pal-colorsubtable-*).
    EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, 8, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    h.put(Harness::DATA + 0x2000, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    h.call("glColorSubTableEXT", {I{GL_TEXTURE_2D}, I{2}, I{3}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA + 0x2000}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.table(GL_RGBA, GL_UNSIGNED_BYTE, 32),
              hex("03 0a 11 18 1f 26 2d 34 01 02 03 04 05 06 07 08 09 0a 0b 0c 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc"));
    struct Sub {
        i32 start, count;
        u32 err;
    };
    for (const Sub& s : {Sub{6, 2, 0}, Sub{6, 3, GL_INVALID_VALUE}, Sub{8, 0, 0}, Sub{9, 0, GL_INVALID_VALUE},
                         Sub{7, 1, 0}, Sub{-1, 1, GL_INVALID_VALUE}, Sub{0, -1, GL_INVALID_VALUE}}) {
        h.call("glColorSubTableEXT", {I{GL_TEXTURE_2D}, I{static_cast<u32>(s.start)}, I{static_cast<u32>(s.count)},
                                      I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
        EXPECT_EQ(h.err(), s.err) << s.start << " " << s.count;
    }
    h.call("glColorSubTableEXT", {I{GL_TEXTURE_1D}, I{0}, I{1}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glColorSubTableEXT", {I{GL_PROXY_TEXTURE_2D}, I{0}, I{1}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, 2, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    h.put(Harness::DATA + 0x2000, {200, 201, 202});
    h.call("glColorSubTableEXT", {I{GL_TEXTURE_2D}, I{0}, I{1}, I{GL_RGB}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA + 0x2000}});
    EXPECT_EQ(h.table(GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("c8 c9 ca ff 1f 26 2d 34"));
    // Each texture object has its own table; the default object too
    // (ext-pal-second-object-table-width, -first-object-...-after-rebind, -default-object-width).
    EXPECT_EQ(table(GL_TEXTURE_2D, GL_RGBA, 16, GL_RGBA, GL_UNSIGNED_BYTE), 0u);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex + 5}});
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 1);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 16);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{0}});
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 1);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    // Compiled into display lists; refused inside glBegin (ext-pal-colortable-compiled,
    // -colortable-inside-begin, -getparam-inside-begin).
    h.put(Harness::DATA + 0x2000, {9, 9, 9, 9, 8, 8, 8, 8});
    h.call("glNewList", {I{311}, I{GL_COMPILE}});
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{2}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA + 0x2000}});
    h.call("glEndList", {});
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 16);
    h.call("glCallList", {I{311}});
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 2);
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glBegin", {I{GL_POINTS}});
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{2}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    h.call("glBegin", {I{GL_POINTS}});
    const i32 inside = h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT);
    h.call("glEnd", {});
    EXPECT_EQ(inside, -7);
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
}

// ---- GL_EXT_paletted_texture: index textures ---------------------------------------------

TEST_P(GlExtensionsTest, IndexTexturesLookUpTheTable) {
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    h.put(Harness::DATA, ramp(4096, 7, 3));
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{256}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    ASSERT_EQ(h.err(), GL_NO_ERROR);
    const GuestAddr idx = Harness::DATA + 0x2000;
    h.put(idx, {0, 1, 2, 3, 3, 2, 1, 0, 4, 5, 6, 7, 255, 128, 9, 1});
    // ext-pal-teximage-COLOR_INDEXn: accepted; the internal format is the
    // components value; colour sizes 0; GL_TEXTURE_INDEX_SIZE_EXT is INVALID_ENUM.
    for (u32 f : {GL_COLOR_INDEX1_EXT, GL_COLOR_INDEX2_EXT, GL_COLOR_INDEX4_EXT, GL_COLOR_INDEX8_EXT,
                  GL_COLOR_INDEX12_EXT, GL_COLOR_INDEX16_EXT}) {
        h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{f}, I{2}, I{2}, I{0}, I{GL_COLOR_INDEX}, I{GL_UNSIGNED_BYTE},
                                P{idx}});
        EXPECT_EQ(h.err(), GL_NO_ERROR) << f;
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_INDEX_SIZE_EXT), -1) << f;
        EXPECT_EQ(h.err(), GL_INVALID_ENUM) << f;
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT), static_cast<i32>(f));
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 2);
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE), 0);
        EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 16),
                  hex("03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c")) << f;
        EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 4), hex("00 01 02 03")) << f;
    }
    for (u32 bad : {GL_COLOR_INDEX, 0x80E8u}) {
        h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{bad}, I{2}, I{2}, I{0}, I{GL_COLOR_INDEX},
                                I{GL_UNSIGNED_BYTE}, P{idx}});
        EXPECT_EQ(h.err(), GL_INVALID_ENUM) << bad;   // ext-pal-teximage-COLOR_INDEX / -0x80E8
    }
    // Colour data into an index format (-COLOR_INDEX8-from-rgba): INVALID_OPERATION.
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX8_EXT}, I{1}, I{1}, I{0}, I{GL_RGBA},
                            I{GL_UNSIGNED_BYTE}, P{idx}});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    // A float index is truncated (-COLOR_INDEX8-float-3.6).
    const float f36 = 3.6f;
    h.put(idx + 0x100, std::vector<u8>(reinterpret_cast<const u8*>(&f36), reinterpret_cast<const u8*>(&f36) + 4));
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX8_EXT}, I{1}, I{1}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_FLOAT}, P{idx + 0x100}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 1), hex("03"));
    // Index data into an RGBA texture: the usual index-to-RGBA path (-RGBA-from-index).
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{1}, I{1}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_BYTE}, P{idx}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    // INDEX_OFFSET applies to the stored indices (-index-offset1-get-index).
    h.call("glPixelTransferi", {I{GL_INDEX_OFFSET}, I{1}});
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX8_EXT}, I{2}, I{1}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_BYTE}, P{idx}});
    h.call("glPixelTransferi", {I{GL_INDEX_OFFSET}, I{0}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 2), hex("01 02"));
    // Stored resolution: 8 bits for INDEX1-8, 16 for INDEX16 (-index1-ushort-3-300, -index16-ushort-3-300).
    const u16 us[2] = {3, 300};
    h.put(idx + 0x200, std::vector<u8>(reinterpret_cast<const u8*>(us), reinterpret_cast<const u8*>(us) + 4));
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX1_EXT}, I{2}, I{1}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_SHORT}, P{idx + 0x200}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_SHORT, 4), hex("03 00 2c 00"));
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX16_EXT}, I{2}, I{1}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_SHORT}, P{idx + 0x200}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_SHORT, 4), hex("03 00 2c 01"));
    // glTexSubImage2D: index data into an index level; colour data is
    // INVALID_ENUM there (-texsubimage-index, -texsubimage-rgba-into-index).
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX8_EXT}, I{2}, I{2}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_BYTE}, P{idx}});
    h.put(idx + 0x300, {77, 1, 2, 3});
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{1}, I{1}, I{1}, I{1}, I{GL_COLOR_INDEX}, I{GL_UNSIGNED_BYTE},
                               P{idx + 0x300}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 4), hex("00 01 02 4d"));
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{0}, I{0}, I{1}, I{1}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                               P{idx + 0x300}});
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    // Changing the table changes what the texture holds (ext-pal-draw-after-subtable-left).
    h.put(idx + 0x400, {0x11, 0x22, 0x33, 0xFF});
    h.call("glColorSubTableEXT", {I{GL_TEXTURE_2D}, I{1}, I{1}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{idx + 0x400}});
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("03 0a 11 18 11 22 33 ff"));
    // An RGBA texture's sub-image from index data is fine (-texsubimage-index-into-rgba).
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{1}, I{1}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{idx}});
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{0}, I{0}, I{1}, I{1}, I{GL_COLOR_INDEX}, I{GL_UNSIGNED_BYTE},
                               P{idx}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    // COLOR_INDEX from a colour level stays INVALID_ENUM (spec 6.1.4).
    h.tex_image(GL_TEXTURE_2D, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 1);
    EXPECT_EQ(h.err(), GL_INVALID_ENUM);
    // glCopyTexImage2D with an index format: INVALID_OPERATION (-copyteximage-index8).
    h.call("glCopyTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_COLOR_INDEX8_EXT}, I{0}, I{0}, I{2}, I{2}, I{0}});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    // 1D (-tex1d-index2): a 2-entry RGB table, texels 1, 0.
    h.call("glBindTexture", {I{GL_TEXTURE_1D}, I{kTex + 1}});
    h.put(idx + 0x500, {10, 20, 30, 40, 50, 60});
    h.call("glColorTableEXT", {I{GL_TEXTURE_1D}, I{GL_RGB}, I{2}, I{GL_RGB}, I{GL_UNSIGNED_BYTE}, P{idx + 0x500}});
    h.put(idx + 0x600, {1, 0});
    h.call("glTexImage1D", {I{GL_TEXTURE_1D}, I{0}, I{GL_COLOR_INDEX2_EXT}, I{2}, I{0}, I{GL_COLOR_INDEX},
                            I{GL_UNSIGNED_BYTE}, P{idx + 0x600}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_1D, GL_RGBA, GL_UNSIGNED_BYTE, 8), hex("28 32 3c ff 0a 14 1e ff"));
}

// ---- glDrawRangeElementsWIN and glAddSwapHintRectWIN: errors -------------------------------

TEST_P(GlExtensionsTest, DrawRangeElementsAndSwapHintErrors) {
    h.put(Harness::DATA, {1, 0});
    auto dre = [&](u32 mode, u32 start, u32 end, i32 count, u32 type) {
        h.call("glDrawRangeElementsWIN", {I{mode}, I{start}, I{end}, I{static_cast<u32>(count)}, I{type},
                                          P{Harness::DATA}});
        return h.err();
    };
    EXPECT_EQ(dre(GL_POINTS, 0, 1, 2, GL_UNSIGNED_BYTE), GL_NO_ERROR);         // ext-dre-points
    EXPECT_EQ(dre(GL_POINTS, 1, 0, 2, GL_UNSIGNED_BYTE), GL_INVALID_VALUE);    // -end-lt-start
    EXPECT_EQ(dre(GL_POINTS, 0, 1, -1, GL_UNSIGNED_BYTE), GL_INVALID_VALUE);   // -count-negative
    EXPECT_EQ(dre(0x1234, 0, 1, 2, GL_UNSIGNED_BYTE), GL_INVALID_ENUM);        // -bad-mode
    EXPECT_EQ(dre(GL_POINTS, 0, 1, 2, GL_FLOAT), GL_INVALID_ENUM);             // -bad-type
    EXPECT_EQ(dre(GL_POINTS, 0, 0, 2, GL_UNSIGNED_BYTE), GL_NO_ERROR);         // -index-outside-range
    EXPECT_EQ(dre(GL_POINTS, 0, 1, 0, GL_UNSIGNED_BYTE), GL_NO_ERROR);         // -count0
    h.call("glBegin", {I{GL_POINTS}});
    h.call("glDrawRangeElementsWIN", {I{GL_POINTS}, I{0}, I{1}, I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);   // ext-dre-inside-begin
    // Compiled (ext-dre-compiled): the list holds the draw.
    h.call("glNewList", {I{313}, I{GL_COMPILE}});
    h.call("glDrawRangeElementsWIN", {I{GL_POINTS}, I{1}, I{0}, I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    h.call("glEndList", {});
    EXPECT_EQ(h.err(), GL_NO_ERROR);                 // compiled, not executed
    h.call("glCallList", {I{313}});
    EXPECT_EQ(h.err(), GL_INVALID_VALUE);            // the error appears when it executes

    // glAddSwapHintRectWIN: INVALID_OPERATION inside glBegin, nothing for a
    // zero or negative size (hint-inside-begin, hint-zero-size-add,
    // hint-negative-width-add); never an error in a list (hint-in-list).
    h.call("glAddSwapHintRectWIN", {I{0}, I{0}, I{1}, I{1}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glAddSwapHintRectWIN", {I{50}, I{50}, I{0}, I{0}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glAddSwapHintRectWIN", {I{50}, I{50}, I{static_cast<u32>(-5)}, I{10}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glBegin", {I{GL_POINTS}});
    h.call("glAddSwapHintRectWIN", {I{0}, I{0}, I{1}, I{1}});
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    h.call("glNewList", {I{314}, I{GL_COMPILE}});
    h.call("glAddSwapHintRectWIN", {I{50}, I{50}, I{30}, I{20}});
    h.call("glEndList", {});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
}

// ---- Client memory (final review Findings 1 and 3; gl_client_memory.hpp) ------------------

/// The final review's wrapping modes on the UNPACK_* (@p pack false) or PACK_*
/// side: ALIGNMENT 4, ROW_LENGTH 2^31 - 1, SKIP_ROWS 2^30, SKIP_PIXELS
/// 2^30 - 100. For RGBA FLOAT they wrapped first_byte to -1600.
void wrap_modes(Harness& h, bool pack) {
    h.call("glPixelStorei", {I{pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT}, I{4}});
    h.call("glPixelStorei", {I{pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH}, I{0x7FFFFFFF}});
    h.call("glPixelStorei", {I{pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS}, I{1u << 30}});
    h.call("glPixelStorei", {I{pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS}, I{(1u << 30) - 100}});
}

void reset_modes(Harness& h) {
    for (u32 p : {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_PACK_ROW_LENGTH,
                  GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS}) {
        h.call("glPixelStorei", {I{p}, I{0}});
    }
    h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
    h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
}

// Unpacking entry points: no address range can hold the image, so the
// command reads nothing and has no effect (what Vitreo does where Windows
// faults in the caller), with no error. Before the fix each read host heap
// memory before its copy of the client bytes.
TEST_P(GlExtensionsTest, WrappingUnpackLayoutReadsNothing) {
    h.fill(Harness::DATA, 0x400, 0x3F);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    wrap_modes(h, false);
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{128}, I{1}, I{0}, I{GL_RGBA}, I{GL_FLOAT},
                            P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);   // not defined
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{128}, I{GL_RGBA}, I{GL_FLOAT}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 1);   // the initial table, unchanged

    // Defined with ordinary modes, then sub-images under the wrapping ones.
    reset_modes(h);
    h.fill(Harness::DATA, 8, 0x11);
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{2}, I{1}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{Harness::DATA}});
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{2}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    ASSERT_EQ(h.err(), GL_NO_ERROR);
    wrap_modes(h, false);
    h.fill(Harness::DATA, 0x400, 0x3F);
    h.call("glTexSubImage2D", {I{GL_TEXTURE_2D}, I{0}, I{0}, I{0}, I{2}, I{1}, I{GL_RGBA}, I{GL_FLOAT},
                               P{Harness::DATA}});
    h.call("glColorSubTableEXT", {I{GL_TEXTURE_2D}, I{0}, I{2}, I{GL_RGBA}, I{GL_FLOAT}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    reset_modes(h);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, 8), std::vector<u8>(8, 0x11));
    EXPECT_EQ(h.table(GL_RGBA, GL_UNSIGNED_BYTE, 8), std::vector<u8>(8, 0x11));

    // glPolygonStipple: a 32 x 32 BITMAP whose SKIP_ROWS * row stride is past
    // any user address space.
    h.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{0x7FFFFFFF}});
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_ROWS}, I{0x7FFFFFFF}});
    h.call("glPolygonStipple", {P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    reset_modes(h);
    h.fill(Harness::DST, 128, 0xCC);
    h.call("glGetPolygonStipple", {P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 128), std::vector<u8>(128, 0xFF));   // the initial all-ones pattern
}

// Packing entry points: nothing is written through a wrapping layout. Before
// the fix each wrote about 1.6 KB before its host buffer.
TEST_P(GlExtensionsTest, WrappingPackLayoutWritesNothing) {
    h.fill(Harness::DATA, 128 * 4, 0x5A);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{128}, I{1}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                            P{Harness::DATA}});
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{128}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    ASSERT_EQ(h.err(), GL_NO_ERROR);
    wrap_modes(h, true);
    EXPECT_EQ(h.tex_image(GL_TEXTURE_2D, GL_RGBA, GL_FLOAT, 256), std::vector<u8>(256, 0xCC));
    EXPECT_EQ(h.table(GL_RGBA, GL_FLOAT, 512), std::vector<u8>(512, 0xCC));
    h.call("glPixelStorei", {I{GL_PACK_SKIP_ROWS}, I{0x7FFFFFFF}});
    h.fill(Harness::DST, 128, 0xCC);
    h.call("glGetPolygonStipple", {P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 128), std::vector<u8>(128, 0xCC));
    EXPECT_EQ(h.err(), GL_NO_ERROR);
}

// Finding 3 / Task 12 M1: guest arguments never size a host allocation before
// the guest range is known to hold the data. Each of these named a range of
// terabytes (or 8 GiB) that is not mapped; before the fix each resized a
// host buffer to it first, which aborts the process (Vitreo has no
// exceptions) or commits it. Now nothing is allocated and nothing happens.
TEST_P(GlExtensionsTest, UnmappedHugeClientRangesAllocateNothing) {
    h.fill(Harness::DATA, 0x400, 0x3F);
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{kTex}});
    h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{4}});
    h.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{0x7FFFFFFF}});   // 1024 rows: about 8.8 TB
    h.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{1024}, I{1024}, I{0}, I{GL_RGBA},
                            I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);
    h.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{1u << 24}});
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_ROWS}, I{1u << 20}});   // 2^20 rows of 64 MiB: 64 TiB
    h.call("glColorTableEXT", {I{GL_TEXTURE_2D}, I{GL_RGBA}, I{256}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.table_param(GL_TEXTURE_2D, GL_COLOR_TABLE_WIDTH_EXT), 1);   // the initial table, unchanged
    reset_modes(h);
    // glCallLists: 2^31 - 1 four-byte offsets (8 GiB) at a 256 KiB mapping.
    const u32 base = static_cast<u32>(h.call("glGenLists", {I{1}}));
    h.call("glNewList", {I{base}, I{GL_COMPILE}});
    h.call("glColor4f", {F{0.25f}, F{0.5f}, F{0.75f}, F{1.0f}});
    h.call("glEndList", {});
    h.call("glColor4f", {F{1.0f}, F{1.0f}, F{1.0f}, F{1.0f}});
    h.call("glListBase", {I{base}});
    h.call("glCallLists", {I{0x7FFFFFFF}, I{GL_4_BYTES}, P{Harness::DATA}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    (void)h.memory.write_u32(Harness::OUT, 0);
    h.call("glGetFloatv", {I{GL_CURRENT_COLOR}, P{Harness::OUT}});
    u32 red = h.memory.read_u32(Harness::OUT).value();
    float r;
    std::memcpy(&r, &red, 4);
    EXPECT_EQ(r, 1.0f);   // no list ran
    // glDeleteTextures / glAreTexturesResident / glPrioritizeTextures /
    // glGenTextures with 2^31 - 1 names at a 256 KiB mapping.
    h.call("glDeleteTextures", {I{0x7FFFFFFF}, P{Harness::DATA}});
    h.call("glPrioritizeTextures", {I{0x7FFFFFFF}, P{Harness::DATA}, P{Harness::DATA}});
    EXPECT_EQ(h.call("glAreTexturesResident", {I{0x7FFFFFFF}, P{Harness::DATA}, P{Harness::DST}}), 0u);
    h.call("glGenTextures", {I{0x7FFFFFFF}, P{Harness::DST}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    EXPECT_EQ(h.call("glIsTexture", {I{kTex}}), 1u);   // the bound texture was not deleted
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlExtensionsTest, ::testing::Values(false, true), bitness_name);

}  // namespace
