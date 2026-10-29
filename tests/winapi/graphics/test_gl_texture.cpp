/// @file test_gl_texture.cpp
/// @brief OpenGL 1.1 texture images and objects on the CPU: unpacking under
///        the pixel-store modes and pixel transfer (spec 3.6), the internal
///        formats (Tables 3.14-3.15), glGetTexImage packing (6.1.4, 4.3.2),
///        sub-images, proxies (3.8.3), texture objects and their sharing
///        (3.8.8), mipmap completeness (3.8.2), the border refusal (Design 11)
///        and the error rules, in both bitnesses where an export is called.
///
/// Expected texels come from the specification's formulas, written out in
/// the test: eq. 3.8-3.10 for where a group lies in memory, Table 2.6 for
/// the conversion to floating point, 3.6.4 for pixel transfer, Table 3.15 for
/// the selection of components, rounding to the stored resolution (3.8.1) and
/// Table 6.1 / Table 4.7 for what glGetTexImage returns. GPU sampling is
/// test_gl_texture_metal.cpp's.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_pixels.hpp"
#include "winapi/graphics/opengl32/gl_texture.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;
using test::Caller;
using cmd::GlClientPixels;
using cmd::GlResidency;

namespace {

constexpr GuestAddr kMem = 0x10000;   // 0x10000 bytes, read/write

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

/// A context current on this thread, with no drawable.
struct CurrentContext {
    explicit CurrentContext(std::shared_ptr<GlShareGroup> group = nullptr) : ctx(GlFramebufferConfig{}, group) {
        ctx.on_first_make_current(64, 64);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

GlClientPixels bytes_of(const std::vector<u8>& data) {
    GlClientPixels px;
    px.bytes = data;
    return px;
}

/// glGetTexImage into a buffer pre-filled with @p fill.
struct Readback {
    std::vector<u8> bytes;
    u8 fill = 0xEE;
};
bool get_tex_image(GlContext& ctx, u32 target, i32 level, u32 format, u32 type, Readback& rb) {
    return cmd::GetTexImage(
        ctx, target, level, format, type,
        [](void* user, usize size) -> std::span<u8> {
            auto& r = *static_cast<Readback*>(user);
            r.bytes.assign(size, r.fill);
            return r.bytes;
        },
        &rb);
}

std::vector<float> as_floats(const std::vector<u8>& bytes) {
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}

i32 level_param(GlContext& ctx, u32 target, i32 level, u32 pname) {
    GlQuery q;
    EXPECT_TRUE(cmd::GetTexLevelParameter(ctx, target, level, pname, q));
    return static_cast<i32>(q.v[0]);
}

/// Table 2.6 for GLubyte: c / (2^8 - 1); 3.8.1 then rounds to the stored
/// 8 bits, which gives the same value back.
float ub(u32 c) { return static_cast<float>(c / 255.0); }
/// The stored 8-bit value of a [0, 1] component (3.8.1: round to nearest).
float q8(double c) { return static_cast<float>(std::floor(std::clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0); }
float q16(double c) { return static_cast<float>(std::floor(std::clamp(c, 0.0, 1.0) * 65535.0 + 0.5) / 65535.0); }

class GlTextureTest : public ::testing::Test {
protected:
    CurrentContext current;
    GlContext& ctx = current.ctx;
    u32 error() { return ctx.take_error(); }
};

/// The export-level fixture: the registered thunks called with real x86 and
/// x64 frames.
class GlTextureExportTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(Caller::kStackBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_ = std::make_unique<Opengl32Manager>();
        mgr_->set_thunk_registry(registry_.get());
        ASSERT_TRUE(registry_->register_dll("opengl32.dll", create_opengl32_thunks(*mgr_)).is_ok());
        caller_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
    }
    u64 call(const char* name, std::vector<u64> args = {}) { return caller_->call(name, std::move(args)); }
    u32 error() { return static_cast<u32>(call("glGetError")); }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> caller_;
    CurrentContext current_;
};

// ---- The five texel-transfer assertions Task 3 removed (test_opengl32.cpp) ----

TEST_P(GlTextureExportTest, RestoredTexelTransferAssertions) {
    // glTexImage1D on a 2D target: INVALID_ENUM (3.8.1: TEXTURE_1D or PROXY_TEXTURE_1D).
    call("glTexImage1D", {GL_TEXTURE_2D, 0, GL_RGBA, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    // glTexImage1D with border 2: INVALID_VALUE (bs > bt = 1).
    call("glTexImage1D", {GL_TEXTURE_1D, 0, GL_RGBA, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // glCopyTexImage2D with width -1: INVALID_VALUE.
    call("glCopyTexImage2D", {GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, static_cast<u64>(static_cast<u32>(-1)), 4, 0});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // glCopyTexSubImage1D on a 2D target: INVALID_ENUM.
    call("glCopyTexSubImage1D", {GL_TEXTURE_2D, 0, 0, 0, 0, 4});
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    // glGetTexImage with level -1: INVALID_VALUE (6.1.4).
    call("glGetTexImage", {GL_TEXTURE_2D, static_cast<u64>(static_cast<u32>(-1)), GL_RGBA, GL_UNSIGNED_BYTE, kMem});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
}

// The exports marshal every argument: an image through glTexImage2D, read
// back through glGetTexImage, with the object commands around it.
TEST_P(GlTextureExportTest, ExportsRoundTripAnImage) {
    call("glGenTextures", {2, kMem});
    const u32 a = memory_.read_u32(kMem).value(), b = memory_.read_u32(kMem + 4).value();
    EXPECT_NE(a, 0u);
    EXPECT_NE(a, b);
    EXPECT_EQ(call("glIsTexture", {a}), 0u);   // generated, not yet bound
    call("glBindTexture", {GL_TEXTURE_2D, a});
    EXPECT_EQ(call("glIsTexture", {a}), 1u);
    const std::array<u8, 16> texels{10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160};
    ASSERT_TRUE(memory_.write_bytes(kMem + 0x100, texels.data(), texels.size()).is_ok());
    call("glTexImage2D", {GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, kMem + 0x100});
    EXPECT_EQ(error(), GL_NO_ERROR);
    call("glGetTexImage", {GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, kMem + 0x200});
    std::array<u8, 16> back{};
    ASSERT_TRUE(memory_.read_bytes(kMem + 0x200, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, texels);
    // A 1D object to TEXTURE_2D and back: INVALID_OPERATION (3.8.8).
    call("glBindTexture", {GL_TEXTURE_1D, b});
    call("glBindTexture", {GL_TEXTURE_2D, b});
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    // Residency (aretexturesresident-after-teximage, -bound-no-image): a has
    // an image with data, b none, so FALSE with both residences written.
    ASSERT_TRUE(memory_.write_u32(kMem, a).is_ok());
    ASSERT_TRUE(memory_.write_u32(kMem + 4, b).is_ok());
    ASSERT_TRUE(memory_.write_u32(kMem + 0x300, 0x5A5A5A5A).is_ok());
    EXPECT_EQ(call("glAreTexturesResident", {2, kMem, kMem + 0x300}), 0u);
    EXPECT_EQ(memory_.read_u8(kMem + 0x300).value(), 1u);
    EXPECT_EQ(memory_.read_u8(kMem + 0x301).value(), 0u);
    EXPECT_EQ(error(), GL_NO_ERROR);
    // a alone: TRUE, and its residence written too.
    ASSERT_TRUE(memory_.write_u32(kMem + 0x300, 0x5A5A5A5A).is_ok());
    EXPECT_EQ(call("glAreTexturesResident", {1, kMem, kMem + 0x300}), 1u);
    EXPECT_EQ(memory_.read_u8(kMem + 0x300).value(), 1u);
    EXPECT_EQ(memory_.read_u8(kMem + 0x301).value(), 0x5Au);
    call("glDeleteTextures", {2, kMem});
    EXPECT_EQ(call("glIsTexture", {a}), 0u);
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Every image export's argument positions in both bitnesses: each argument
// is made to matter (a size, an offset, a format) and checked.
TEST_P(GlTextureExportTest, ImageExportsReadEveryArgument) {
    const auto level_int = [&](u32 target, u32 level, u32 pname) {
        call("glGetTexLevelParameteriv", {target, level, pname, kMem + 0x800});
        return static_cast<i32>(memory_.read_u32(kMem + 0x800).value());
    };
    call("glPixelStorei", {GL_UNPACK_ALIGNMENT, 1});
    call("glBindTexture", {GL_TEXTURE_1D, 3});
    const std::array<u8, 4> lum{11, 22, 33, 44};
    ASSERT_TRUE(memory_.write_bytes(kMem + 0x100, lum.data(), lum.size()).is_ok());
    call("glTexImage1D", {GL_TEXTURE_1D, 1, GL_LUMINANCE8, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, kMem + 0x100});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_int(GL_TEXTURE_1D, 1, GL_TEXTURE_WIDTH), 4);
    EXPECT_EQ(level_int(GL_TEXTURE_1D, 1, GL_TEXTURE_INTERNAL_FORMAT), static_cast<i32>(GL_LUMINANCE8));
    const u8 patch = 99;
    ASSERT_TRUE(memory_.write_bytes(kMem + 0x180, &patch, 1).is_ok());
    call("glTexSubImage1D", {GL_TEXTURE_1D, 1, 2, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, kMem + 0x180});
    EXPECT_EQ(error(), GL_NO_ERROR);
    call("glGetTexImage", {GL_TEXTURE_1D, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, kMem + 0x200});
    std::array<u8, 4> back{};
    ASSERT_TRUE(memory_.read_bytes(kMem + 0x200, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, (std::array<u8, 4>{11, 22, 99, 44}));
    call("glBindTexture", {GL_TEXTURE_2D, 4});
    call("glTexImage2D", {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    const std::array<u8, 4> red{255, 0, 0, 255};
    ASSERT_TRUE(memory_.write_bytes(kMem + 0x180, red.data(), red.size()).is_ok());
    call("glTexSubImage2D", {GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, kMem + 0x180});
    EXPECT_EQ(error(), GL_NO_ERROR);
    call("glGetTexImage", {GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, kMem + 0x200});
    std::array<u8, 16> rgba{};
    ASSERT_TRUE(memory_.read_bytes(kMem + 0x200, rgba.data(), rgba.size()).is_ok());
    EXPECT_EQ(rgba[12], 255u);   // texel (1, 1)
    // Copies (no drawable here, so the contents are undefined): sizes and format land.
    call("glCopyTexImage2D", {GL_TEXTURE_2D, 0, GL_RGB5, 3, 5, 4, 2, 0});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_int(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 4);
    EXPECT_EQ(level_int(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT), 2);
    EXPECT_EQ(level_int(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT), static_cast<i32>(GL_RGB5));
    call("glCopyTexImage1D", {GL_TEXTURE_1D, 2, GL_ALPHA4, 0, 0, 8, 0});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_int(GL_TEXTURE_1D, 2, GL_TEXTURE_WIDTH), 8);
    call("glCopyTexSubImage2D", {GL_TEXTURE_2D, 0, 3, 1, 0, 0, 1, 1});
    EXPECT_EQ(error(), GL_NO_ERROR);
    call("glCopyTexSubImage2D", {GL_TEXTURE_2D, 0, 0, 2, 0, 0, 1, 1});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    call("glCopyTexSubImage1D", {GL_TEXTURE_1D, 2, 7, 0, 0, 1});
    EXPECT_EQ(error(), GL_NO_ERROR);
    call("glCopyTexSubImage1D", {GL_TEXTURE_1D, 2, 7, 0, 0, 2});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // glPrioritizeTextures reads its two arrays.
    ASSERT_TRUE(memory_.write_u32(kMem, 4).is_ok());
    const float quarter = 0.25f;
    u32 bits;
    std::memcpy(&bits, &quarter, 4);
    ASSERT_TRUE(memory_.write_u32(kMem + 4, bits).is_ok());
    call("glPrioritizeTextures", {1, kMem, kMem + 4});
    call("glGetTexParameterfv", {GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, kMem + 0x800});
    EXPECT_EQ(memory_.read_u32(kMem + 0x800).value(), bits);
    EXPECT_EQ(error(), GL_NO_ERROR);
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlTextureExportTest, ::testing::Values(false, true), bitness_name);

// ---- Unpacking (3.6.1, 3.6.4) -----------------------------------------------------

// Eq. 3.8/3.9: with UNPACK_ALIGNMENT a, ROW_LENGTH l, SKIP_PIXELS p and
// SKIP_ROWS r, group (x, y) of a format with n elements of s bytes starts at
// element p n + r k + y k + x n, k = (a / s) ceil(s n l / a) when s < a.
TEST_F(GlTextureTest, UnpackHonoursPixelStoreAlignmentRowLengthSkip) {
    // RGB / UNSIGNED_BYTE, 2x2, a = 4, l = 3, p = 1, r = 1: k = 4 ceil(9 / 4)
    // = 12 bytes, the first group at 1 * 3 + 1 * 12 = 15.
    std::vector<u8> mem(64);
    for (usize i = 0; i < mem.size(); ++i) mem[i] = static_cast<u8>(i * 3 + 1);
    cmd::PixelStore(ctx, GL_UNPACK_ALIGNMENT, 4, true);
    cmd::PixelStore(ctx, GL_UNPACK_ROW_LENGTH, 3, true);
    cmd::PixelStore(ctx, GL_UNPACK_SKIP_PIXELS, 1, true);
    cmd::PixelStore(ctx, GL_UNPACK_SKIP_ROWS, 1, true);
    const GlPixelLayout layout = gl_pixel_layout(ctx.state.pixel_store.unpack, GL_RGB, GL_UNSIGNED_BYTE, 2, 2);
    EXPECT_EQ(layout.first_byte, 15u);
    EXPECT_EQ(layout.row_bytes, 12u);
    EXPECT_EQ(layout.span_bytes, 15u + 12u + 6u);
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 1);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, bytes_of(mem));
    ASSERT_EQ(error(), GL_NO_ERROR);
    Readback rb;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rb));
    const std::vector<float> got = as_floats(rb.bytes);
    ASSERT_EQ(got.size(), 16u);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            const usize at = 15 + 12 * y + 3 * x;
            const usize t = (y * 2 + x) * 4;
            for (u32 k = 0; k < 3; ++k) EXPECT_EQ(got[t + k], ub(mem[at + k])) << x << "," << y << "," << k;
            EXPECT_EQ(got[t + 3], 1.0f);   // RGB: Table 6.1 A = 1
        }
    }

    // UNSIGNED_SHORT with SWAP_BYTES (Table 3.6) and a = 8 > s = 2: k =
    // (8 / 2) ceil(2 * 2 * 3 / 8) = 8 elements = 16 bytes for LUMINANCE_ALPHA.
    cmd::PixelStore(ctx, GL_UNPACK_ALIGNMENT, 8, true);
    cmd::PixelStore(ctx, GL_UNPACK_SWAP_BYTES, 1, true);
    cmd::PixelStore(ctx, GL_UNPACK_SKIP_PIXELS, 0, true);
    const GlPixelLayout la = gl_pixel_layout(ctx.state.pixel_store.unpack, GL_LUMINANCE_ALPHA, GL_UNSIGNED_SHORT, 2, 2);
    EXPECT_EQ(la.row_bytes, 16u);
    EXPECT_EQ(la.first_byte, 16u);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_LUMINANCE16_ALPHA16, 2, 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_SHORT,
                  bytes_of(mem));
    ASSERT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, GL_FLOAT, rb));
    const std::vector<float> la_got = as_floats(rb.bytes);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            const usize at = 16 + 16 * y + 4 * x;
            // Swapped: the first byte in memory is the high byte.
            const double l = (mem[at] * 256.0 + mem[at + 1]) / 65535.0;
            const double a = (mem[at + 2] * 256.0 + mem[at + 3]) / 65535.0;
            EXPECT_EQ(la_got[(y * 2 + x) * 2 + 0], q16(l)) << x << "," << y;
            EXPECT_EQ(la_got[(y * 2 + x) * 2 + 1], q16(a)) << x << "," << y;
        }
    }

    // BITMAP colour indices (eq. 3.10): k = a ceil(l / 8a) bytes; SKIP_PIXELS
    // bits are skipped; LSB_FIRST orders the bits from the least significant.
    cmd::PixelStore(ctx, GL_UNPACK_SWAP_BYTES, 0, true);
    cmd::PixelStore(ctx, GL_UNPACK_ALIGNMENT, 1, true);
    cmd::PixelStore(ctx, GL_UNPACK_ROW_LENGTH, 0, true);
    cmd::PixelStore(ctx, GL_UNPACK_SKIP_ROWS, 0, true);
    cmd::PixelStore(ctx, GL_UNPACK_SKIP_PIXELS, 3, true);
    cmd::PixelStore(ctx, GL_UNPACK_LSB_FIRST, 1, true);
    // I_TO_R = {0, 1}, so index 1 is red 1.
    const std::array<float, 2> to_r{0.0f, 1.0f};
    cmd::PixelMap(ctx, GL_PIXEL_MAP_I_TO_R, 2, to_r);
    // l = width = 4, a = 1: k = 1 ubyte per row.
    const std::vector<u8> bits{0b10101000, 0b00000001};
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGB, 4, 2, 0, GL_COLOR_INDEX, GL_BITMAP, bytes_of(bits));
    ASSERT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, rb));
    // Row 0 = byte 0 bits 3..6: 1, 0, 1, 0. Row 1 = byte 1 bits 3..6 of 0b1: 0, 0, 0, 0.
    EXPECT_EQ(rb.bytes, (std::vector<u8>{255, 0, 255, 0, 0, 0, 0, 0}));
}

// Spec 3.6.4 applies to texture images: scale and bias, the RGBA lookups when
// MAP_COLOR, and for colour indices the shift, offset and I_TO_* lookups;
// the result is clamped to [0, 1] (3.8.1).
TEST_F(GlTextureTest, PixelTransferAppliesToTextureImages) {
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 1);
    cmd::PixelTransfer(ctx, GL_RED_SCALE, 0.5, false);
    cmd::PixelTransfer(ctx, GL_GREEN_BIAS, 0.25, false);
    cmd::PixelTransfer(ctx, GL_BLUE_SCALE, 4.0, false);   // clamps to 1
    cmd::PixelTransfer(ctx, GL_ALPHA_BIAS, -0.5, false);
    const std::vector<u8> texel{200, 100, 80, 255};
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texel));
    Readback rb;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rb));
    std::vector<float> got = as_floats(rb.bytes);
    EXPECT_EQ(got[0], q8(200 / 255.0 * 0.5));
    EXPECT_EQ(got[1], q8(100 / 255.0 + 0.25));
    EXPECT_EQ(got[2], q8(80 / 255.0 * 4.0));
    EXPECT_EQ(got[3], q8(1.0 - 0.5));

    // MAP_COLOR: clamp, then entry round(c (size - 1)) of R_TO_R etc.
    cmd::PixelTransfer(ctx, GL_RED_SCALE, 1.0, false);
    cmd::PixelTransfer(ctx, GL_GREEN_BIAS, 0.0, false);
    cmd::PixelTransfer(ctx, GL_BLUE_SCALE, 1.0, false);
    cmd::PixelTransfer(ctx, GL_ALPHA_BIAS, 0.0, false);
    cmd::PixelTransfer(ctx, GL_MAP_COLOR, 1, true);
    const std::array<float, 4> r_map{0.1f, 0.2f, 0.3f, 0.4f};
    cmd::PixelMap(ctx, GL_PIXEL_MAP_R_TO_R, 4, r_map);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texel));
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rb));
    got = as_floats(rb.bytes);
    // 200/255 * 3 = 2.35 -> entry 2.
    EXPECT_EQ(got[0], q8(0.3f));
    // G_TO_G and the others keep their initial one-entry map {0}.
    EXPECT_EQ(got[1], 0.0f);
    cmd::PixelTransfer(ctx, GL_MAP_COLOR, 0, true);

    // Colour indices: (index << 1) + 1, then I_TO_R / I_TO_A of size 8,
    // addressed with & 7.
    cmd::PixelTransfer(ctx, GL_INDEX_SHIFT, 1, true);
    cmd::PixelTransfer(ctx, GL_INDEX_OFFSET, 1, true);
    std::array<float, 8> i_to_r{}, i_to_a{};
    for (u32 k = 0; k < 8; ++k) {
        i_to_r[k] = k / 8.0f;
        i_to_a[k] = 1.0f - k / 8.0f;
    }
    cmd::PixelMap(ctx, GL_PIXEL_MAP_I_TO_R, 8, i_to_r);
    cmd::PixelMap(ctx, GL_PIXEL_MAP_I_TO_A, 8, i_to_a);
    const std::vector<u8> indices{2, 5};   // (2 << 1) + 1 = 5; (5 << 1) + 1 = 11 & 7 = 3
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, bytes_of(indices));
    ASSERT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rb));
    got = as_floats(rb.bytes);
    EXPECT_EQ(got[0], q8(5 / 8.0));
    EXPECT_EQ(got[3], q8(1 - 5 / 8.0));
    EXPECT_EQ(got[4], q8(3 / 8.0));
    EXPECT_EQ(got[7], q8(1 - 3 / 8.0));
}

// ---- Internal formats (Tables 3.14-3.15) ----------------------------------------

// Every GL 1.1 internal format: its base format, the stored resolution of
// each component (6.1.3: the actual resolutions), the canonical stored texel
// (the base format's components selected from R, G, B, A, Table 3.15) and
// what glGetTexImage returns for it (Table 6.1).
TEST_F(GlTextureTest, InternalFormatsMapToBaseFormatAndSwizzle) {
    struct Row {
        u32 format;
        u32 base;
        u32 widest;   // widest Table 3.15 component (0: base / legacy)
    };
    // Transcribed from Tables 3.14 and 3.15 and the 1-4 rule of 3.8.1.
    const Row rows[] = {
        {GL_ALPHA, GL_ALPHA, 0}, {GL_LUMINANCE, GL_LUMINANCE, 0}, {GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 0},
        {GL_INTENSITY, GL_INTENSITY, 0}, {GL_RGB, GL_RGB, 0}, {GL_RGBA, GL_RGBA, 0},
        {1, GL_LUMINANCE, 0}, {2, GL_LUMINANCE_ALPHA, 0}, {3, GL_RGB, 0}, {4, GL_RGBA, 0},
        {GL_ALPHA4, GL_ALPHA, 4}, {GL_ALPHA8, GL_ALPHA, 8}, {GL_ALPHA12, GL_ALPHA, 12}, {GL_ALPHA16, GL_ALPHA, 16},
        {GL_LUMINANCE4, GL_LUMINANCE, 4}, {GL_LUMINANCE8, GL_LUMINANCE, 8}, {GL_LUMINANCE12, GL_LUMINANCE, 12},
        {GL_LUMINANCE16, GL_LUMINANCE, 16}, {GL_LUMINANCE4_ALPHA4, GL_LUMINANCE_ALPHA, 4},
        {GL_LUMINANCE6_ALPHA2, GL_LUMINANCE_ALPHA, 6}, {GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, 8},
        {GL_LUMINANCE12_ALPHA4, GL_LUMINANCE_ALPHA, 12}, {GL_LUMINANCE12_ALPHA12, GL_LUMINANCE_ALPHA, 12},
        {GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, 16}, {GL_INTENSITY4, GL_INTENSITY, 4},
        {GL_INTENSITY8, GL_INTENSITY, 8}, {GL_INTENSITY12, GL_INTENSITY, 12}, {GL_INTENSITY16, GL_INTENSITY, 16},
        {GL_R3_G3_B2, GL_RGB, 3}, {GL_RGB4, GL_RGB, 4}, {GL_RGB5, GL_RGB, 5}, {GL_RGB8, GL_RGB, 8},
        {GL_RGB10, GL_RGB, 10}, {GL_RGB12, GL_RGB, 12}, {GL_RGB16, GL_RGB, 16}, {GL_RGBA2, GL_RGBA, 2},
        {GL_RGBA4, GL_RGBA, 4}, {GL_RGB5_A1, GL_RGBA, 5}, {GL_RGBA8, GL_RGBA, 8}, {GL_RGB10_A2, GL_RGBA, 10},
        {GL_RGBA12, GL_RGBA, 12}, {GL_RGBA16, GL_RGBA, 16},
    };
    ASSERT_EQ(std::size(rows), 42u);   // 3.8.3: "a 42-valued integer"
    const float src[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    std::vector<u8> bytes(16);
    std::memcpy(bytes.data(), src, 16);
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 7);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.format);
        const std::optional<GlTexFormat> f = gl_tex_format(static_cast<i32>(row.format), true);
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->base, row.base);
        const bool wide = row.widest > 8;
        EXPECT_EQ(f->backend, wide ? graphics::PixelFormat::RGBA16Unorm : graphics::PixelFormat::RGBA8Unorm);
        cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, static_cast<i32>(row.format), 1, 1, 0, GL_RGBA, GL_FLOAT,
                      bytes_of(bytes));
        ASSERT_EQ(error(), GL_NO_ERROR);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT), static_cast<i32>(row.format));
        const i32 bits = wide ? 16 : 8;
        const bool rgb = row.base == GL_RGB || row.base == GL_RGBA;
        const bool alpha = row.base == GL_ALPHA || row.base == GL_LUMINANCE_ALPHA || row.base == GL_RGBA;
        const bool lum = row.base == GL_LUMINANCE || row.base == GL_LUMINANCE_ALPHA;
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE), rgb ? bits : 0);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_GREEN_SIZE), rgb ? bits : 0);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_BLUE_SIZE), rgb ? bits : 0);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE), alpha ? bits : 0);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_LUMINANCE_SIZE), lum ? bits : 0);
        EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_INTENSITY_SIZE), row.base == GL_INTENSITY ? bits : 0);

        // The stored canonical texel: Table 3.15 selects L = R, I = R, A = A.
        const auto q = [&](double c) { return wide ? q16(c) : q8(c); };
        std::array<float, 4> canonical{};
        switch (row.base) {
        case GL_ALPHA: canonical = {0, 0, 0, q(0.8f)}; break;
        case GL_LUMINANCE: canonical = {q(0.2f), q(0.2f), q(0.2f), 1}; break;
        case GL_LUMINANCE_ALPHA: canonical = {q(0.2f), q(0.2f), q(0.2f), q(0.8f)}; break;
        case GL_INTENSITY: canonical = {q(0.2f), q(0.2f), q(0.2f), q(0.2f)}; break;
        case GL_RGB: canonical = {q(0.2f), q(0.4f), q(0.6f), 1}; break;
        default: canonical = {q(0.2f), q(0.4f), q(0.6f), q(0.8f)}; break;
        }
        {
            const GlShareGroup::Lock held = ctx.share_group->lock();
            const GlTexLevel& l = ctx.bound_texture(2, held)->levels[0];
            ASSERT_EQ(l.texels.size(), 4u * (wide ? 2u : 1u));
            for (u32 k = 0; k < 4; ++k) {
                const double stored = wide ? (l.texels[2 * k] | (l.texels[2 * k + 1] << 8)) / 65535.0 : l.texels[k] / 255.0;
                EXPECT_EQ(static_cast<float>(stored), canonical[k]) << "component " << k;
            }
        }
        // Table 6.1: ALPHA (0,0,0,At), L (Lt,0,0,1), LA (Lt,0,0,At), I (It,0,0,1).
        std::array<float, 4> returned{};
        switch (row.base) {
        case GL_ALPHA: returned = {0, 0, 0, canonical[3]}; break;
        case GL_LUMINANCE: case GL_INTENSITY: returned = {canonical[0], 0, 0, 1}; break;
        case GL_LUMINANCE_ALPHA: returned = {canonical[0], 0, 0, canonical[3]}; break;
        default: returned = canonical; break;
        }
        Readback rb;
        ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rb));
        const std::vector<float> got = as_floats(rb.bytes);
        for (u32 k = 0; k < 4; ++k) EXPECT_EQ(got[k], returned[k]) << "component " << k;
    }
    // Not internal formats: GDI Generic records INVALID_ENUM, for glTexImage
    // and glCopyTexImage, 1D and 2D (gl11_state_probe64.expected
    // teximage-/copyteximage-bad-internalformat-{1d,2d}-0x1234, -0, -5,
    // -GL_RED, -GL_COLOR_INDEX), before the size check (-and-npot).
    for (i32 bad : {0x1234, 0, 5, static_cast<i32>(GL_RED), static_cast<i32>(GL_COLOR_INDEX)}) {
        cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, bad, 1, 1, 0, GL_RGBA, GL_FLOAT, bytes_of(bytes));
        EXPECT_EQ(error(), GL_INVALID_ENUM) << bad;
        cmd::TexImage(ctx, 1, GL_TEXTURE_1D, 0, bad, 1, 1, 0, GL_RGBA, GL_FLOAT, bytes_of(bytes));
        EXPECT_EQ(error(), GL_INVALID_ENUM) << bad;
        cmd::CopyTexImage(ctx, 2, GL_TEXTURE_2D, 0, static_cast<u32>(bad), 0, 0, 4, 4, 0);
        EXPECT_EQ(error(), GL_INVALID_ENUM) << bad;
        cmd::CopyTexImage(ctx, 1, GL_TEXTURE_1D, 0, static_cast<u32>(bad), 0, 0, 4, 1, 0);
        EXPECT_EQ(error(), GL_INVALID_ENUM) << bad;
    }
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, 0x1234, 3, 4, 0, GL_RGBA, GL_FLOAT, {.null_pointer = true});
    EXPECT_EQ(error(), GL_INVALID_ENUM);   // teximage-bad-internalformat-and-npot
    cmd::CopyTexImage(ctx, 2, GL_TEXTURE_2D, 0, 0x1234, 0, 0, 3, 4, 0);
    EXPECT_EQ(error(), GL_INVALID_ENUM);   // copyteximage-bad-internalformat-and-npot
    // glCopyTexImage refuses 1-4 with INVALID_ENUM (3.8.2;
    // copyteximage-bad-internalformat-{1d,2d}-1..4); glTexImage takes them
    // (teximage-bad-internalformat-{1d,2d}-1..4: no error).
    for (u32 count = 1; count <= 4; ++count) {
        cmd::CopyTexImage(ctx, 2, GL_TEXTURE_2D, 0, count, 0, 0, 4, 4, 0);
        EXPECT_EQ(error(), GL_INVALID_ENUM) << count;
        cmd::CopyTexImage(ctx, 1, GL_TEXTURE_1D, 0, count, 0, 0, 4, 1, 0);
        EXPECT_EQ(error(), GL_INVALID_ENUM) << count;
        cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, static_cast<i32>(count), 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      {.null_pointer = true});
        EXPECT_EQ(error(), GL_NO_ERROR) << count;
    }
}

// glGetTexImage packs under the PACK_* modes (4.3.2): L = R + G + B clamped,
// Table 4.7 conversions, alignment and skips, bytes between groups untouched.
TEST_F(GlTextureTest, GetTexImagePacksPerPackState) {
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 3);
    const std::vector<u8> texels{100, 50, 25, 255, 200, 200, 200, 0};
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texels));
    Readback rb;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(rb.bytes, (std::vector<u8>{175, 255}));   // 100+50+25; 600/255 clamps to 1
    // BYTE: [(2^8 - 1) c - 1] / 2.
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_ALPHA, GL_BYTE, rb));
    // c = 1: 254 / 2 = 127; c = 0: -1/2, rounded to nearest (half up) = 0.
    EXPECT_EQ(rb.bytes, (std::vector<u8>{127, 0}));
    // PACK_ALIGNMENT 4, ROW_LENGTH 3, SKIP_PIXELS 1 for RGB ubyte: k = 12,
    // first group at 3; the bytes not written keep the fill.
    cmd::PixelStore(ctx, GL_PACK_ALIGNMENT, 4, true);
    cmd::PixelStore(ctx, GL_PACK_ROW_LENGTH, 3, true);
    cmd::PixelStore(ctx, GL_PACK_SKIP_PIXELS, 1, true);
    rb.fill = 0xEE;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(rb.bytes, (std::vector<u8>{0xEE, 0xEE, 0xEE, 100, 50, 25, 200, 200, 200}));
    // Errors (6.1.4): index / depth / stencil formats, BITMAP, a proxy target.
    for (u32 format : {GL_COLOR_INDEX, GL_STENCIL_INDEX, GL_DEPTH_COMPONENT}) {
        EXPECT_FALSE(get_tex_image(ctx, GL_TEXTURE_2D, 0, format, GL_UNSIGNED_BYTE, rb));
        EXPECT_EQ(error(), GL_INVALID_ENUM);
    }
    EXPECT_FALSE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_BITMAP, rb));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    EXPECT_FALSE(get_tex_image(ctx, GL_PROXY_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    EXPECT_FALSE(get_tex_image(ctx, GL_TEXTURE_2D, 11, GL_RGBA, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // A level without an image writes nothing.
    EXPECT_FALSE(get_tex_image(ctx, GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// glTexSubImage replaces only its rectangle, assigned by the level's own
// internal format (3.8.2); its errors.
TEST_F(GlTextureTest, TexSubImageReplacesItsRegion) {
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 4);
    std::vector<u8> zero(4 * 4 * 4, 0);
    // Not yet defined: the region does not fit a 0x0 level, INVALID_VALUE
    // (err-glTexSubImage2D-undefined-level); an empty region is
    // INVALID_OPERATION (texsubimage2d-undefined-level-empty).
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(zero));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(zero));
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    cmd::BindTexture(ctx, GL_TEXTURE_1D, 40);
    cmd::TexSubImage(ctx, 1, GL_TEXTURE_1D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(zero));
    EXPECT_EQ(error(), GL_INVALID_VALUE);   // texsubimage1d-undefined-level
    cmd::CopyTexSubImage(ctx, 1, GL_TEXTURE_1D, 0, 0, 0, 0, 0, 1, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);   // copytexsubimage1d-undefined-level
    cmd::CopyTexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 0, 0, 0, 0, 1, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);   // copytexsubimage2d-undefined-level
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(zero));
    const std::vector<u8> patch{10, 20, 30, 40, 50, 60, 70, 80};   // 2x1 RGBA
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 1, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(patch));
    ASSERT_EQ(error(), GL_NO_ERROR);
    Readback rb;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, rb));
    std::vector<u8> expect(16, 0);
    expect[2 * 4 + 1] = 10;   // L = R of each group
    expect[2 * 4 + 2] = 50;
    EXPECT_EQ(rb.bytes, expect);
    // Offsets outside the image, negative sizes: INVALID_VALUE.
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 3, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(patch));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, -1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(patch));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 0, 0, -1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(patch));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexSubImage(ctx, 2, GL_PROXY_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(patch));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    cmd::TexSubImage(ctx, 2, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, bytes_of(patch));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    // 1D: glTexSubImage1D on the 1D object.
    cmd::BindTexture(ctx, GL_TEXTURE_1D, 5);
    cmd::TexImage(ctx, 1, GL_TEXTURE_1D, 0, GL_ALPHA8, 4, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bytes_of(zero));
    const std::vector<u8> a2{99, 98};
    cmd::TexSubImage(ctx, 1, GL_TEXTURE_1D, 0, 2, 0, 2, 1, GL_ALPHA, GL_UNSIGNED_BYTE, bytes_of(a2));
    ASSERT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_1D, 0, GL_ALPHA, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(rb.bytes, (std::vector<u8>{0, 0, 99, 98}));
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_1D, 0, GL_TEXTURE_HEIGHT), 1);
}

// The image argument checks of 3.8.1 (glTexImage).
TEST_F(GlTextureTest, TexImageValidatesItsArguments) {
    std::vector<u8> data(1024 * 4, 0);
    // Width / height not 2^n + 2 border: INVALID_VALUE; zero is the null texture.
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 3, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 4, -4, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_NO_ERROR);
    // Level < 0 or > log2(MAX_TEXTURE_SIZE): INVALID_VALUE.
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, -1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 11, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // Too large for MAX_TEXTURE_SIZE (1024) at that level: INVALID_VALUE.
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 2048, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, {.null_pointer = true});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 1, GL_RGBA, 1024, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, {.null_pointer = true});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // Format / type: STENCIL_INDEX, DEPTH_COMPONENT, a bad type, BITMAP with RGBA: INVALID_ENUM.
    for (u32 format : {GL_STENCIL_INDEX, GL_DEPTH_COMPONENT, GL_RGBA8}) {
        cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, format, GL_UNSIGNED_BYTE, bytes_of(data));
        EXPECT_EQ(error(), GL_INVALID_ENUM) << format;
    }
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_RGBA, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_BITMAP, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    // A NULL pointer defines the image with unspecified contents (3.8.1).
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGB5, 8, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, {.null_pointer = true});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 8);
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT), 2);
    // Inside glBegin / glEnd: INVALID_OPERATION (2.6.3).
    ctx.begin_mode = GL_TRIANGLES;
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    ctx.begin_mode = kGlOutsideBeginEnd;
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
}

// Design 11: texture borders are Phase 2. border = 1 is INVALID_VALUE (with a
// one-time UNIMPLEMENTED) and changes nothing; a proxy reports it cannot be
// stored (3.8.3: zero-sized, no error).
TEST_F(GlTextureTest, BorderOneFailsLoudly) {
    gl_forget_unimplemented_logs_for_tests();
    ::testing::internal::CaptureStderr();
    std::vector<u8> data(18 * 18 * 4, 7);
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 1);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    ASSERT_EQ(error(), GL_NO_ERROR);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 18, 18, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 16);   // unchanged
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_BORDER), 0);
    cmd::BindTexture(ctx, GL_TEXTURE_1D, 2);
    cmd::TexImage(ctx, 1, GL_TEXTURE_1D, 0, GL_RGBA, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(data));
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_1D, 0, GL_TEXTURE_WIDTH), 0);
    cmd::CopyTexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 18, 18, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::CopyTexImage(ctx, 1, GL_TEXTURE_1D, 0, GL_RGBA, 0, 0, 10, 1, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::TexImage(ctx, 2, GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 18, 18, 1, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_BORDER), 0);
    // One line per api, once (Design 11).
    const std::string log = ::testing::internal::GetCapturedStderr();
    const std::string line2d =
        "[vitreo] UNIMPLEMENTED opengl32!glTexImage2D: texture borders (border = 1) are not implemented (Phase 2)\n";
    EXPECT_NE(log.find(line2d), std::string::npos) << log;
    EXPECT_EQ(log.find(line2d), log.rfind(line2d)) << log;   // the proxy call did not log it again
    for (const char* api : {"glTexImage1D", "glCopyTexImage2D", "glCopyTexImage1D"}) {
        EXPECT_NE(log.find(std::string("UNIMPLEMENTED opengl32!") + api + ": texture borders"), std::string::npos)
            << api << "\n" << log;
    }
}

// Spec 3.8.3: a proxy takes the state a real image would have, or zero sizes
// when too large; no image and no error for size; other errors as usual.
TEST_F(GlTextureTest, ProxyTexturesAnswerSizeQueries) {
    cmd::TexImage(ctx, 2, GL_PROXY_TEXTURE_2D, 0, GL_RGBA12, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 1024);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT), 512);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT), static_cast<i32>(GL_RGBA12));
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE), 16);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE), 16);
    cmd::TexImage(ctx, 2, GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 2048, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT), 0);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE), 0);
    // Level 3 holds at most 1024 / 8.
    cmd::TexImage(ctx, 1, GL_PROXY_TEXTURE_1D, 3, GL_LUMINANCE8, 128, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_1D, 3, GL_TEXTURE_WIDTH), 128);
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_1D, 3, GL_TEXTURE_LUMINANCE_SIZE), 8);
    cmd::TexImage(ctx, 1, GL_PROXY_TEXTURE_1D, 3, GL_LUMINANCE8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(level_param(ctx, GL_PROXY_TEXTURE_1D, 3, GL_TEXTURE_WIDTH), 0);
    // Malformed sizes are still errors.
    cmd::TexImage(ctx, 2, GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 3, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, {});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // The real texture is untouched by proxy calls.
    EXPECT_EQ(level_param(ctx, GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);
    // A proxy is not a glTexParameter target.
    cmd::TexParameter(ctx, GL_PROXY_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(GL_LINEAR, true));
    EXPECT_EQ(error(), GL_INVALID_ENUM);
}

// ---- Texture objects (3.8.8) -------------------------------------------------------

TEST_F(GlTextureTest, TextureObjectCommandsFollowTheSpec) {
    std::vector<u32> names;
    cmd::GenTextures(ctx, -1, names);
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::GenTextures(ctx, 3, names);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(error(), GL_NO_ERROR);
    // Generated names are used but are not texture objects until bound.
    for (u32 n : names) EXPECT_FALSE(cmd::IsTexture(ctx, n));
    EXPECT_TRUE(ctx.share_group->has_objects());
    // Binding an unused name, never generated, creates it too.
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 77);
    EXPECT_TRUE(cmd::IsTexture(ctx, 77));
    std::vector<u32> more;
    cmd::GenTextures(ctx, 2, more);
    for (u32 n : more) {
        EXPECT_NE(n, 77u);
        for (u32 m : names) EXPECT_NE(n, m);
    }
    EXPECT_FALSE(cmd::IsTexture(ctx, 0));
    cmd::BindTexture(ctx, 0x806Fu /* GL 1.2 TEXTURE_3D */, 1);
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    // A new object has the initial state of Table 6.11.
    cmd::BindTexture(ctx, GL_TEXTURE_1D, names[0]);
    GlQuery q;
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, q));
    EXPECT_EQ(q.v[0], GL_NEAREST_MIPMAP_LINEAR);
    // Dimensionality is fixed at first bind.
    cmd::BindTexture(ctx, GL_TEXTURE_2D, names[0]);
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    GlQuery binding;
    ASSERT_TRUE(cmd::Get(ctx, GL_TEXTURE_BINDING_2D, binding));
    EXPECT_EQ(binding.v[0], 77);   // the failed bind changed nothing
    // Prioritize clamps to [0, 1] and ignores unused names and 0.
    const std::array<u32, 4> pn{names[0], 0, 9999, names[1]};
    const std::array<float, 4> pv{2.0f, 0.5f, 0.5f, 0.25f};
    cmd::PrioritizeTextures(ctx, 4, pn, pv);
    EXPECT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_1D, GL_TEXTURE_PRIORITY, q));
    EXPECT_EQ(q.v[0], 1.0);
    cmd::PrioritizeTextures(ctx, -1, {}, {});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // AreTexturesResident (gl11_state_probe64.expected aretexturesresident-*).
    // Object 77 has no image: not resident (-bound-no-image ret=0 res=0).
    std::vector<u8> residences;
    const std::array<u32, 1> one{77};
    GlResidency r = cmd::AreTexturesResident(ctx, 1, one, residences);
    EXPECT_FALSE(r.all_resident);
    EXPECT_TRUE(r.write_residences);
    EXPECT_EQ(residences, std::vector<u8>{0});
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, q));
    EXPECT_EQ(q.v[0], 0.0);
    // A NULL-pointer image is not resident (texparam-resident-after-null-teximage 0) ...
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, {.null_pointer = true});
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, q));
    EXPECT_EQ(q.v[0], 0.0);
    // ... one with data is (-after-teximage ret=1 res=1,1; residences written
    // even when all are resident), before any draw.
    const std::vector<u8> texels(16, 0x80);
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texels));
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, q));
    EXPECT_EQ(q.v[0], 1.0);
    r = cmd::AreTexturesResident(ctx, 1, one, residences);
    EXPECT_TRUE(r.all_resident);
    EXPECT_TRUE(r.write_residences);
    EXPECT_EQ(residences, std::vector<u8>{1});
    // A name that is not an object (0, unused, generated-but-unbound): FALSE,
    // INVALID_VALUE, the residences before it written (-zero / -unknown /
    // -generated-unbound ret=0 res=1,untouched).
    for (u32 bad : {0u, 9999u, names[2]}) {
        const std::array<u32, 2> rn{77, bad};
        r = cmd::AreTexturesResident(ctx, 2, rn, residences);
        EXPECT_FALSE(r.all_resident);
        EXPECT_TRUE(r.write_residences);
        EXPECT_EQ(residences, std::vector<u8>{1}) << bad;
        EXPECT_EQ(error(), GL_INVALID_VALUE) << bad;
    }
    cmd::AreTexturesResident(ctx, -1, {}, residences);
    EXPECT_EQ(error(), GL_INVALID_VALUE);   // aretexturesresident-neg
    // Delete: unused names and 0 ignored; deleting a bound texture binds 0.
    const std::array<u32, 3> dn{0, 12345, 77};
    cmd::DeleteTextures(ctx, 3, dn);
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_FALSE(cmd::IsTexture(ctx, 77));
    ASSERT_TRUE(cmd::Get(ctx, GL_TEXTURE_BINDING_2D, binding));
    EXPECT_EQ(binding.v[0], 0);
    cmd::DeleteTextures(ctx, -2, {});
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    // Inside glBegin: INVALID_OPERATION, and nothing generated.
    ctx.begin_mode = GL_POINTS;
    cmd::GenTextures(ctx, 1, more);
    EXPECT_TRUE(more.empty());
    ctx.begin_mode = kGlOutsideBeginEnd;
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
}

// Ruling 3 (spec 3.8.8): deleting the bound texture binds 0, so later
// glTexParameter calls set the default texture's state, without an error.
TEST_F(GlTextureTest, DeletingTheBoundTextureRevertsToTheDefault) {
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 5);
    cmd::TexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GlValues::scalar_value(GL_NEAREST, true));
    const std::array<u32, 1> five{5};
    cmd::DeleteTextures(ctx, 1, five);
    cmd::TexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GlValues::scalar_value(GL_CLAMP, true));
    EXPECT_EQ(error(), GL_NO_ERROR);
    GlQuery q;
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, q));
    EXPECT_EQ(q.v[0], GL_CLAMP);
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, q));
    EXPECT_EQ(q.v[0], GL_LINEAR);   // the default texture's own, not object 5's
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        EXPECT_EQ(ctx.bound_texture(2, held), &ctx.default_textures[1]);
        EXPECT_EQ(ctx.default_textures[1].params.wrap_s, GL_CLAMP);
    }
    // Rebinding 5 makes a fresh object with the initial state.
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 5);
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, q));
    EXPECT_EQ(q.v[0], GL_REPEAT);
    // glPopAttrib restoring a binding whose object was deleted binds 0.
    cmd::PushAttrib(ctx, GL_TEXTURE_BIT);
    cmd::DeleteTextures(ctx, 1, five);
    cmd::PopAttrib(ctx);
    EXPECT_EQ(error(), GL_NO_ERROR);
    GlQuery binding;
    ASSERT_TRUE(cmd::Get(ctx, GL_TEXTURE_BINDING_2D, binding));
    cmd::TexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GlValues::scalar_value(GL_CLAMP, true));
    EXPECT_EQ(error(), GL_NO_ERROR);
    const GlShareGroup::Lock held = ctx.share_group->lock();
    EXPECT_EQ(ctx.bound_texture(2, held), &ctx.default_textures[1]);
}

// Ruling 2: contexts sharing a group see one set of objects, from any thread;
// the group's lock serialises the commands.
TEST_F(GlTextureTest, TextureObjectsShareAcrossSharedContexts) {
    auto group = ctx.share_group;
    std::vector<u32> names;
    cmd::GenTextures(ctx, 1, names);
    const u32 name = names[0];
    cmd::BindTexture(ctx, GL_TEXTURE_2D, name);
    const std::vector<u8> texel{1, 2, 3, 4};
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texel));
    cmd::TexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(GL_LINEAR, true));

    // A second context of the same group, on another thread.
    std::thread other([&] {
        CurrentContext b(group);
        EXPECT_TRUE(cmd::IsTexture(b.ctx, name));
        cmd::BindTexture(b.ctx, GL_TEXTURE_2D, name);
        GlQuery q;
        EXPECT_TRUE(cmd::GetTexParameter(b.ctx, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, q));
        EXPECT_EQ(q.v[0], GL_LINEAR);
        Readback rb;
        EXPECT_TRUE(get_tex_image(b.ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rb));
        EXPECT_EQ(rb.bytes, texel);
        // Its change is the first context's too.
        cmd::TexParameter(b.ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GlValues::scalar_value(GL_CLAMP, true));
        std::vector<u32> its;
        cmd::GenTextures(b.ctx, 1, its);
        EXPECT_NE(its[0], name);   // one name space
        EXPECT_EQ(b.ctx.take_error(), GL_NO_ERROR);
    });
    other.join();
    GlQuery q;
    ASSERT_TRUE(cmd::GetTexParameter(ctx, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, q));
    EXPECT_EQ(q.v[0], GL_CLAMP);

    // Concurrent commands on both threads: the lock keeps the name map and
    // the objects consistent (every generated name is distinct; images land).
    std::atomic<bool> go{false};
    std::vector<u32> a_names, b_names;
    const auto worker = [&](std::vector<u32>& out, u32 texel_value) {
        CurrentContext c(group);
        while (!go.load()) {}
        const std::vector<u8> t(4 * 4 * 4, static_cast<u8>(texel_value));
        for (u32 k = 0; k < 200; ++k) {
            std::vector<u32> n;
            cmd::GenTextures(c.ctx, 2, n);
            out.insert(out.end(), n.begin(), n.end());
            cmd::BindTexture(c.ctx, GL_TEXTURE_2D, n[0]);
            cmd::TexImage(c.ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(t));
            cmd::BindTexture(c.ctx, GL_TEXTURE_2D, name);   // the shared one, both threads
            cmd::TexParameter(c.ctx, GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, GlValues::scalar_value(0.5, false));
            const std::array<u32, 1> del{n[1]};
            cmd::DeleteTextures(c.ctx, 1, del);
        }
        EXPECT_EQ(c.ctx.take_error(), GL_NO_ERROR);
    };
    std::thread ta(worker, std::ref(a_names), 10u), tb(worker, std::ref(b_names), 20u);
    go = true;
    ta.join();
    tb.join();
    std::vector<u32> all = a_names;
    all.insert(all.end(), b_names.begin(), b_names.end());
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
    EXPECT_EQ(all.size(), 800u);
    // Each thread's images stayed its own.
    for (u32 n : {a_names[0], b_names[0]}) {
        cmd::BindTexture(ctx, GL_TEXTURE_2D, n);
        Readback rb;
        ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, rb));
        EXPECT_EQ(rb.bytes[0], n == a_names[0] ? 10 : 20);
    }
    // wglShareLists refuses a destination that has objects (Task 5): a
    // generated name counts.
    GlContext lonely(GlFramebufferConfig{});
    EXPECT_FALSE(lonely.share_group->has_objects());
    std::vector<u32> one;
    cmd::GenTextures(lonely, 1, one);
    EXPECT_TRUE(lonely.share_group->has_objects());
}

// A context keeps a bound object another context of its group deleted: the
// name is unused again, the object lives until unbound here.
TEST_F(GlTextureTest, ObjectDeletedElsewhereStaysBoundHere) {
    auto group = ctx.share_group;
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 9);
    const std::vector<u8> texel{9, 9, 9, 9};
    cmd::TexImage(ctx, 2, GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes_of(texel));
    std::thread other([&] {
        CurrentContext b(group);
        const std::array<u32, 1> nine{9};
        cmd::DeleteTextures(b.ctx, 1, nine);
        EXPECT_FALSE(cmd::IsTexture(b.ctx, 9));
    });
    other.join();
    EXPECT_FALSE(cmd::IsTexture(ctx, 9));
    Readback rb;
    ASSERT_TRUE(get_tex_image(ctx, GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rb));
    EXPECT_EQ(rb.bytes, texel);
}

// ---- Mipmap completeness (3.8.2) ----------------------------------------------------

TEST_F(GlTextureTest, MipmapCompletenessFollowsTheLevelSequence) {
    cmd::BindTexture(ctx, GL_TEXTURE_2D, 1);
    const auto image = [&](i32 level, i32 w, i32 h, u32 format) {
        cmd::TexImage(ctx, 2, GL_TEXTURE_2D, level, static_cast<i32>(format), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      {.null_pointer = true});
        ASSERT_EQ(error(), GL_NO_ERROR);
    };
    const auto complete = [&]() {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        return gl_texture_complete(*ctx.bound_texture(2, held));
    };
    // 8 x 2: p = 3; levels 4x1, 2x1, 1x1 (once one side is 1 it stays 1).
    image(0, 8, 2, GL_RGBA);
    EXPECT_FALSE(complete());
    image(1, 4, 1, GL_RGBA);
    image(2, 2, 1, GL_RGBA);
    EXPECT_FALSE(complete());
    image(3, 1, 1, GL_RGBA);
    EXPECT_TRUE(complete());
    // Arrays above p are insignificant.
    image(5, 1, 1, GL_ALPHA);
    EXPECT_TRUE(complete());
    // A different internal format symbolic constant (RGBA8 is not RGBA).
    image(2, 2, 1, GL_RGBA8);
    EXPECT_FALSE(complete());
    image(2, 2, 1, GL_RGBA);
    EXPECT_TRUE(complete());
    // A wrong size.
    image(1, 4, 2, GL_RGBA);
    EXPECT_FALSE(complete());
    image(1, 4, 1, GL_RGBA);
    EXPECT_TRUE(complete());
    // The null texture at level 0 is never complete.
    image(0, 0, 0, GL_RGBA);
    EXPECT_FALSE(complete());
    // 1D: heights are 1 throughout.
    cmd::BindTexture(ctx, GL_TEXTURE_1D, 2);
    for (i32 level = 0; level <= 2; ++level) {
        cmd::TexImage(ctx, 1, GL_TEXTURE_1D, level, GL_LUMINANCE, 4 >> level, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      {.null_pointer = true});
    }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    EXPECT_TRUE(gl_texture_complete(*ctx.bound_texture(1, held)));
}

// Tables 3.18-3.19 as texture-stage operations (the GPU result is checked by
// GlTextureMetalTest.TexEnvModesPerBaseFormat).
TEST_F(GlTextureTest, TexEnvStagesNameTheTableFunctions) {
    using graphics::TexStageOp;
    struct Row {
        u32 mode, base;
        TexStageOp color, alpha;
    };
    const Row rows[] = {
        {GL_REPLACE, GL_ALPHA, TexStageOp::SelectArg2, TexStageOp::SelectArg1},
        {GL_REPLACE, GL_LUMINANCE, TexStageOp::SelectArg1, TexStageOp::SelectArg2},
        {GL_REPLACE, GL_LUMINANCE_ALPHA, TexStageOp::SelectArg1, TexStageOp::SelectArg1},
        {GL_REPLACE, GL_INTENSITY, TexStageOp::SelectArg1, TexStageOp::SelectArg1},
        {GL_REPLACE, GL_RGB, TexStageOp::SelectArg1, TexStageOp::SelectArg2},
        {GL_REPLACE, GL_RGBA, TexStageOp::SelectArg1, TexStageOp::SelectArg1},
        {GL_MODULATE, GL_ALPHA, TexStageOp::SelectArg2, TexStageOp::Modulate},
        {GL_MODULATE, GL_LUMINANCE, TexStageOp::Modulate, TexStageOp::SelectArg2},
        {GL_MODULATE, GL_LUMINANCE_ALPHA, TexStageOp::Modulate, TexStageOp::Modulate},
        {GL_MODULATE, GL_INTENSITY, TexStageOp::Modulate, TexStageOp::Modulate},
        {GL_MODULATE, GL_RGB, TexStageOp::Modulate, TexStageOp::SelectArg2},
        {GL_MODULATE, GL_RGBA, TexStageOp::Modulate, TexStageOp::Modulate},
        {GL_DECAL, GL_RGB, TexStageOp::SelectArg1, TexStageOp::SelectArg2},
        {GL_DECAL, GL_RGBA, TexStageOp::BlendTextureAlpha, TexStageOp::SelectArg2},
        // Undefined in Table 3.19; Vitreo passes the fragment through.
        {GL_DECAL, GL_ALPHA, TexStageOp::SelectArg2, TexStageOp::SelectArg2},
        {GL_DECAL, GL_LUMINANCE, TexStageOp::SelectArg2, TexStageOp::SelectArg2},
        {GL_DECAL, GL_LUMINANCE_ALPHA, TexStageOp::SelectArg2, TexStageOp::SelectArg2},
        {GL_DECAL, GL_INTENSITY, TexStageOp::SelectArg2, TexStageOp::SelectArg2},
        {GL_BLEND, GL_ALPHA, TexStageOp::SelectArg2, TexStageOp::Modulate},
        {GL_BLEND, GL_LUMINANCE, TexStageOp::Lerp, TexStageOp::SelectArg2},
        {GL_BLEND, GL_LUMINANCE_ALPHA, TexStageOp::Lerp, TexStageOp::Modulate},
        {GL_BLEND, GL_INTENSITY, TexStageOp::Lerp, TexStageOp::Lerp},
        {GL_BLEND, GL_RGB, TexStageOp::Lerp, TexStageOp::SelectArg2},
        {GL_BLEND, GL_RGBA, TexStageOp::Lerp, TexStageOp::Modulate},
    };
    for (const Row& r : rows) {
        const graphics::TextureStageState st = gl_tex_env_stage(r.mode, r.base);
        EXPECT_EQ(st.color_op, r.color) << r.mode << " " << r.base;
        EXPECT_EQ(st.alpha_op, r.alpha) << r.mode << " " << r.base;
        EXPECT_EQ(st.color_arg1 & 0xF, r.mode == GL_BLEND ? 3u : 2u);   // Cc (TFACTOR) or the texture
        EXPECT_EQ(st.color_arg2 & 0xF, 1u);                             // Cf
        EXPECT_NE(st.texture_transform_flags, 0u);                       // the texture matrix applies
    }
}

}  // namespace
