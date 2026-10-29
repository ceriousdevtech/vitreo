#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/user32/resources/resources.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/resources/resource_walker.hpp>
#include <windowing/model/window_types.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <winapi/kernel32/error.hpp>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

class ResourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x1F000;
        marshaler.attach_cpu(cpu);
        exports = create_resource_thunks(mgr);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ResourceManager mgr{memory};
    DllExports exports;
};

// ---- MessageBox tests ----

TEST_F(ResourceTest, MessageBox_MB_OK_ReturnsIDOK) {
    i32 result = mgr.message_box(0, "Hello", "Title", mb::MB_OK);
    EXPECT_EQ(result, id_btn::IDOK);
}

TEST_F(ResourceTest, MessageBox_MB_YESNO_ReturnsIDYES) {
    i32 result = mgr.message_box(0, "Save?", "Confirm", mb::MB_YESNO);
    EXPECT_EQ(result, id_btn::IDYES);
}

TEST_F(ResourceTest, MessageBox_ANSI_WorksWithMBOK) {
    // ANSI-agnostic at this level — same path as MB_OK
    i32 result = mgr.message_box(0, "ANSI text", "ANSI caption", mb::MB_OK);
    EXPECT_EQ(result, id_btn::IDOK);
}

// Learn About Dialog Boxes: MessageBox disables the owner for the duration
// and restores it afterward (08c M11).
TEST_F(ResourceTest, MessageBoxDisablesOwnerThenRestores) {
    windowing::HwndManager hwnd_mgr;
    windowing::WindowData data{};
    data.parent = windowing::HWND_DESKTOP;
    data.enabled = true;
    data.owner_thread_id = 1;
    const windowing::HWND owner = hwnd_mgr.create(data);
    mgr.set_hwnd_manager(&hwnd_mgr);
    setenv("VITREO_MESSAGEBOX_AUTO", "1", 1);
    EXPECT_EQ(mgr.message_box(owner, "c", "t", mb::MB_OK), id_btn::IDOK);
    unsetenv("VITREO_MESSAGEBOX_AUTO");
    EXPECT_TRUE(hwnd_mgr.find(owner)->enabled);
}

TEST_F(ResourceTest, MessageBoxNullCaptionAndText) {
    setenv("VITREO_MESSAGEBOX_AUTO", "1", 1);
    // Thunk-level NULL handling is exercised via direct message_box args that
    // match the Learn defaults the thunks now apply.
    EXPECT_EQ(mgr.message_box(0, "Error", "", mb::MB_OK), id_btn::IDOK);
    unsetenv("VITREO_MESSAGEBOX_AUTO");
    cpu.gpr.rcx = 0;  // owner
    cpu.gpr.rdx = 0;  // NULL text
    cpu.gpr.r8 = 0;   // NULL caption
    cpu.gpr.r9 = mb::MB_OK;
    setenv("VITREO_MESSAGEBOX_AUTO", "1", 1);
    for (auto& entry : exports.entries) {
        if (entry.name == "MessageBoxW") {
            entry.handler(marshaler);
            break;
        }
    }
    unsetenv("VITREO_MESSAGEBOX_AUTO");
    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(id_btn::IDOK));
}

TEST_F(ResourceTest, MessageBoxExWUsesTheMessageBoxContract) {
    constexpr GuestAddr text = 0x10100;
    constexpr GuestAddr caption = 0x10200;
    ASSERT_TRUE(marshaler.write_wide_string(text, "Installer status").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(caption, "Chrome Setup").is_ok());

    ExportEntry* message_box_ex = nullptr;
    for (auto& entry : exports.entries) {
        if (entry.name == "MessageBoxExW") message_box_ex = &entry;
    }
    ASSERT_NE(message_box_ex, nullptr);

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = text;
    cpu.gpr.r8 = caption;
    cpu.gpr.r9 = mb::MB_OK;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0x0409).is_ok());
    message_box_ex->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, id_btn::IDOK);
}

// ---- LoadCursor tests ----

TEST_F(ResourceTest, LoadCursor_Arrow_NonNull) {
    u32 handle = mgr.load_cursor(0, idc::IDC_ARROW);
    EXPECT_NE(handle, 0u);
}

TEST_F(ResourceTest, LoadCursor_IBeam_DifferentHandle) {
    u32 arrow = mgr.load_cursor(0, idc::IDC_ARROW);
    u32 ibeam = mgr.load_cursor(0, idc::IDC_IBEAM);
    EXPECT_NE(arrow, ibeam);
}

TEST_F(ResourceTest, LoadCursor_SystemImageIsStockArrow) {
    const u32 handle = mgr.load_cursor(0, idc::IDC_ARROW);
    auto image = mgr.cursor_image(handle);
    ASSERT_TRUE(image.has_value());
    EXPECT_TRUE(image->is_system);
    EXPECT_EQ(image->system_id, idc::IDC_ARROW);
}

TEST_F(ResourceTest, LoadCursor_ModuleMissingSystemIdReturnsNull) {
    // LoadCursor(hInstance, IDC_ARROW) with a module that has no cursor
    // resource 32512 returns NULL. Stock cursors are LoadCursor(NULL, id).
    constexpr GuestAddr image_base = 0x400000;
    ASSERT_TRUE(memory.map_region(image_base, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    const u32 handle = mgr.load_cursor(static_cast<u32>(image_base),
                                       idc::IDC_ARROW);
    EXPECT_EQ(handle, 0u);
}

TEST_F(ResourceTest, LoadCursor_SizeAndHelpSystemIds) {
    const u32 size_ns = mgr.load_cursor(0, idc::IDC_SIZENS);
    const u32 help = mgr.load_cursor(0, idc::IDC_HELP);
    ASSERT_NE(size_ns, 0u);
    ASSERT_NE(help, 0u);
    EXPECT_NE(size_ns, help);
    auto image = mgr.cursor_image(size_ns);
    ASSERT_TRUE(image.has_value());
    EXPECT_TRUE(image->is_system);
    EXPECT_EQ(image->system_id, idc::IDC_SIZENS);
}

TEST_F(ResourceTest, CreateCursorRgbaRetainsPixels) {
    std::vector<u8> rgba(8 * 8 * 4, 0);
    rgba[0] = 10;
    rgba[1] = 20;
    rgba[2] = 30;
    rgba[3] = 255;
    const u32 handle = mgr.create_cursor_rgba(1, 2, 8, 8, std::move(rgba));
    ASSERT_NE(handle, 0u);
    auto image = mgr.cursor_image(handle);
    ASSERT_TRUE(image.has_value());
    EXPECT_FALSE(image->is_system);
    EXPECT_EQ(image->width, 8u);
    EXPECT_EQ(image->height, 8u);
    EXPECT_EQ(image->hotspot_x, 1u);
    EXPECT_EQ(image->hotspot_y, 2u);
    ASSERT_GE(image->rgba.size(), 4u);
    EXPECT_EQ(image->rgba[0], 10);
    EXPECT_EQ(image->rgba[1], 20);
    EXPECT_EQ(image->rgba[2], 30);
    EXPECT_EQ(image->rgba[3], 255);
    EXPECT_TRUE(mgr.destroy_cursor(handle));
}

TEST_F(ResourceTest, CreateCursorRetainsAndXorBits) {
    // 8x8, WORD-aligned 1bpp = 2 bytes/row. Top-left XOR=1 AND=0 → white.
    // Next pixel AND=1 XOR=0 → transparent.
    std::vector<u8> and_plane(16, 0);
    std::vector<u8> xor_plane(16, 0);
    xor_plane[0] = 0x80;
    and_plane[0] = 0x40;
    const u32 handle = mgr.create_cursor(2, 3, 8, 8,
                                         and_plane.data(), xor_plane.data());
    ASSERT_NE(handle, 0u);
    auto image = mgr.cursor_image(handle);
    ASSERT_TRUE(image.has_value());
    EXPECT_FALSE(image->is_system);
    EXPECT_EQ(image->width, 8u);
    EXPECT_EQ(image->height, 8u);
    EXPECT_EQ(image->hotspot_x, 2u);
    EXPECT_EQ(image->hotspot_y, 3u);
    ASSERT_EQ(image->rgba.size(), 8u * 8u * 4u);
    EXPECT_EQ(image->rgba[0], 255);
    EXPECT_EQ(image->rgba[1], 255);
    EXPECT_EQ(image->rgba[2], 255);
    EXPECT_EQ(image->rgba[3], 255);
    EXPECT_EQ(image->rgba[4], 0);
    EXPECT_EQ(image->rgba[5], 0);
    EXPECT_EQ(image->rgba[6], 0);
    EXPECT_EQ(image->rgba[7], 0);
    EXPECT_TRUE(mgr.destroy_cursor(handle));
    EXPECT_FALSE(mgr.destroy_cursor(handle));
}

TEST_F(ResourceTest, ColorCursorAndMaskIsTransparentNotWhiteBox) {
    // 8x8 24bpp XOR is solid white; AND punches a hole except the top-left
    // pixel. Invert-as-white used to leave an opaque white square.
    constexpr u32 width = 8;
    constexpr u32 height = 8;
    constexpr u32 xor_stride = ((width * 24 + 31) / 32) * 4;
    constexpr u32 and_stride = ((width + 31) / 32) * 4;
    std::vector<u8> blob(4 + 40 + xor_stride * height + and_stride * height, 0);
    blob[4] = 40;
    blob[8] = static_cast<u8>(width);
    blob[12] = static_cast<u8>(height * 2);
    blob[16] = 1;
    blob[18] = 24;
    u8* xor_bits = blob.data() + 4 + 40;
    std::fill(xor_bits, xor_bits + xor_stride * height, 255);
    u8* and_bits = xor_bits + xor_stride * height;
    std::fill(and_bits, and_bits + and_stride * height, 0xFF);
    // Bottom-up: image (0,0) is the last stored AND/XOR row. Clear AND bit 7.
    and_bits[(height - 1) * and_stride] = 0x7F;

    const auto decoded = windowing::decode_rt_cursor(
        blob.data(), static_cast<u32>(blob.size()));
    ASSERT_EQ(decoded.width, width);
    ASSERT_EQ(decoded.height, height);
    ASSERT_EQ(decoded.rgba.size(), width * height * 4u);
    EXPECT_EQ(decoded.rgba[0], 255);
    EXPECT_EQ(decoded.rgba[1], 255);
    EXPECT_EQ(decoded.rgba[2], 255);
    EXPECT_EQ(decoded.rgba[3], 255);
    EXPECT_EQ(decoded.rgba[4], 0);
    EXPECT_EQ(decoded.rgba[5], 0);
    EXPECT_EQ(decoded.rgba[6], 0);
    EXPECT_EQ(decoded.rgba[7], 0);
}

namespace {
/// RT_CURSOR blob (hotspot + DIB) for a 4x4 32bpp cursor. @p bgra is
/// top-down; @p and_rows holds one AND byte per top-down row (bit 7 = x 0).
std::vector<u8> rt_cursor_32bpp(const u32 (&bgra)[16], const u8 (&and_rows)[4]) {
    constexpr u32 w = 4, h = 4, xor_stride = w * 4, and_stride = 4;
    std::vector<u8> blob(4 + 40 + xor_stride * h + and_stride * h, 0);
    blob[4] = 40;
    blob[8] = w;
    blob[12] = h * 2;
    blob[16] = 1;
    blob[18] = 32;
    u8* xor_bits = blob.data() + 4 + 40;
    u8* and_bits = xor_bits + xor_stride * h;
    for (u32 y = 0; y < h; ++y) {
        const u32 stored = h - 1 - y;  // bottom-up
        std::memcpy(xor_bits + stored * xor_stride, &bgra[y * w], xor_stride);
        and_bits[stored * and_stride] = and_rows[y];
    }
    return blob;
}
}  // namespace

// s3gdi_icon_probe "alpha" vs "alpha+halfmask": a 32bpp image whose alpha
// channel is not all zero is drawn by alpha and its AND mask is ignored.
// Unity writes exactly this with an all-ones AND mask; honouring the mask
// made every pixel transparent and the game's cursor invisible.
TEST_F(ResourceTest, Cursor32bppWithAlphaIgnoresAndMask) {
    u32 px[16];
    for (u32 i = 0; i < 16; ++i) px[i] = (static_cast<u32>(i * 16) << 24) | 0x00112233u;
    const u8 all_transparent[4] = {0xF0, 0xF0, 0xF0, 0xF0};
    const auto blob = rt_cursor_32bpp(px, all_transparent);
    const auto decoded = windowing::decode_rt_cursor(blob.data(), static_cast<u32>(blob.size()));
    ASSERT_EQ(decoded.width, 4u);
    ASSERT_EQ(decoded.height, 4u);
    for (u32 i = 0; i < 16; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(decoded.rgba[i * 4 + 0], 0x11);  // R
        EXPECT_EQ(decoded.rgba[i * 4 + 1], 0x22);  // G
        EXPECT_EQ(decoded.rgba[i * 4 + 2], 0x33);  // B
        EXPECT_EQ(decoded.rgba[i * 4 + 3], i * 16);
    }
}

// s3gdi_icon_probe "opaque": a 32bpp image whose alpha bytes are all zero
// has no alpha channel; it is opaque and the AND mask decides transparency.
TEST_F(ResourceTest, Cursor32bppZeroAlphaUsesAndMask) {
    u32 px[16];
    for (u32 i = 0; i < 16; ++i) px[i] = 0x00445566u;
    const u8 right_half[4] = {0x30, 0x30, 0x30, 0x30};  // x 2,3 transparent
    const auto blob = rt_cursor_32bpp(px, right_half);
    const auto decoded = windowing::decode_rt_cursor(blob.data(), static_cast<u32>(blob.size()));
    ASSERT_EQ(decoded.rgba.size(), 64u);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            SCOPED_TRACE(testing::Message() << "x=" << x << " y=" << y);
            const u8* p = decoded.rgba.data() + (y * 4 + x) * 4;
            if (x < 2) {
                EXPECT_EQ(p[0], 0x44);
                EXPECT_EQ(p[1], 0x55);
                EXPECT_EQ(p[2], 0x66);
                EXPECT_EQ(p[3], 0xFF);
            } else {
                EXPECT_EQ(p[3], 0x00);
            }
        }
    }
}

TEST_F(ResourceTest, LoadCursor_ModuleGroupDecodesHotspotAndPixels) {
    constexpr GuestAddr image_base = 0x400000;
    constexpr u32 resource_rva = 0x1000;
    constexpr GuestAddr resource_base = image_base + resource_rva;
    ASSERT_TRUE(memory.map_region(image_base, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());

    auto write_u16 = [&](u32 offset, u16 value) {
        ASSERT_TRUE(memory.write_u16(resource_base + offset, value).is_ok());
    };
    auto write_u32 = [&](u32 offset, u32 value) {
        ASSERT_TRUE(memory.write_u32(resource_base + offset, value).is_ok());
    };

    // Type dir at 0: RT_CURSOR + RT_GROUP_CURSOR.
    write_u16(12, 0); write_u16(14, 2);
    write_u32(16, windowing::RT::CURSOR); write_u32(20, 0x80000020);
    write_u32(24, windowing::RT::GROUP_CURSOR); write_u32(28, 0x80000038);

    // CURSOR name dir at 0x20, id 1.
    write_u16(0x20 + 12, 0); write_u16(0x20 + 14, 1);
    write_u32(0x20 + 16, 1); write_u32(0x20 + 20, 0x80000050);

    // GROUP_CURSOR name dir at 0x38, id 101.
    write_u16(0x38 + 12, 0); write_u16(0x38 + 14, 1);
    write_u32(0x38 + 16, 101); write_u32(0x38 + 20, 0x80000068);

    // Language dirs (English).
    write_u16(0x50 + 12, 0); write_u16(0x50 + 14, 1);
    write_u32(0x50 + 16, 0x0409); write_u32(0x50 + 20, 0x80);
    write_u16(0x68 + 12, 0); write_u16(0x68 + 14, 1);
    write_u32(0x68 + 16, 0x0409); write_u32(0x68 + 20, 0x90);

    constexpr u32 cursor_blob_off = 0xA0;
    constexpr u32 xor_stride = 4;
    constexpr u32 height = 8;
    constexpr u32 palette_bytes = 8;
    constexpr u32 xor_bytes = xor_stride * height;
    constexpr u32 and_bytes = xor_stride * height;
    constexpr u32 cursor_blob_size = 4 + 40 + palette_bytes + xor_bytes + and_bytes;
    constexpr u32 group_blob_off = cursor_blob_off + cursor_blob_size;

    write_u32(0x80, resource_rva + cursor_blob_off);
    write_u32(0x84, cursor_blob_size);
    write_u32(0x90, resource_rva + group_blob_off);
    write_u32(0x94, 20);

    std::vector<u8> cursor_blob(cursor_blob_size, 0);
    cursor_blob[0] = 2; // hotspot x
    cursor_blob[2] = 3; // hotspot y
    cursor_blob[4] = 40; // biSize
    cursor_blob[8] = 8;  // biWidth
    cursor_blob[12] = 16; // biHeight = 2 * 8
    cursor_blob[16] = 1;  // biPlanes
    cursor_blob[18] = 1;  // biBitCount
    cursor_blob[4 + 40 + 4] = 255; // palette[1].blue
    cursor_blob[4 + 40 + 5] = 255;
    cursor_blob[4 + 40 + 6] = 255;
    // Bottom-up: top row is the last XOR scanline. Top-left white.
    cursor_blob[4 + 40 + palette_bytes + 7 * xor_stride] = 0x80;
    ASSERT_TRUE(memory.write_bytes(resource_base + cursor_blob_off,
                                   cursor_blob.data(),
                                   cursor_blob.size()).is_ok());

    std::vector<u8> group_blob(20, 0);
    group_blob[2] = 2; // type = cursor
    group_blob[4] = 1; // count
    group_blob[6] = 8; // width
    group_blob[7] = 8; // height
    group_blob[10] = 1; // planes
    group_blob[12] = 1; // bit count
    group_blob[14] = static_cast<u8>(cursor_blob_size);
    group_blob[18] = 1; // RT_CURSOR id
    ASSERT_TRUE(memory.write_bytes(resource_base + group_blob_off,
                                   group_blob.data(),
                                   group_blob.size()).is_ok());

    mgr.set_resource_info(image_base, resource_rva, 0x3000);
    const u32 first = mgr.load_cursor(static_cast<u32>(image_base), 101);
    ASSERT_NE(first, 0u);
    const u32 second = mgr.load_cursor(static_cast<u32>(image_base), 101);
    EXPECT_EQ(first, second);
    EXPECT_FALSE(mgr.destroy_cursor(first));

    auto image = mgr.cursor_image(first);
    ASSERT_TRUE(image.has_value());
    EXPECT_FALSE(image->is_system);
    EXPECT_EQ(image->width, 8u);
    EXPECT_EQ(image->height, 8u);
    EXPECT_EQ(image->hotspot_x, 2u);
    EXPECT_EQ(image->hotspot_y, 3u);
    ASSERT_EQ(image->rgba.size(), 8u * 8u * 4u);
    EXPECT_EQ(image->rgba[0], 255);
    EXPECT_EQ(image->rgba[1], 255);
    EXPECT_EQ(image->rgba[2], 255);
    EXPECT_EQ(image->rgba[3], 255);
    EXPECT_EQ(image->rgba[4], 0);
    EXPECT_EQ(image->rgba[5], 0);
    EXPECT_EQ(image->rgba[6], 0);
    EXPECT_EQ(image->rgba[7], 255);
}

// ---- LoadIcon tests ----

TEST_F(ResourceTest, LoadIcon_Application_NonNull) {
    u32 handle = mgr.load_icon(0, idi::IDI_APPLICATION);
    EXPECT_NE(handle, 0u);
}

TEST_F(ResourceTest, LoadIcon_UnknownModuleInstance_ReturnsNull) {
    // No PE mapped at instance=1 → cannot resolve RT_GROUP_ICON.
    u32 handle = mgr.load_icon(1, idi::IDI_APPLICATION);
    EXPECT_EQ(handle, 0u);
}

TEST_F(ResourceTest, LoadIcon_ModuleGroupReturnsSharedHandle) {
    // Microsoft Learn LoadIconW(hInstance, MAKEINTRESOURCE(id)): load and
    // share the module RT_GROUP_ICON (Notepad++ IconList::addIcon path).
    constexpr GuestAddr image_base = 0x400000;
    constexpr u32 resource_rva = 0x1000;
    constexpr GuestAddr resource_base = image_base + resource_rva;
    ASSERT_TRUE(memory.map_region(image_base, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());

    auto write_u16 = [&](u32 offset, u16 value) {
        ASSERT_TRUE(memory.write_u16(resource_base + offset, value).is_ok());
    };
    auto write_u32 = [&](u32 offset, u32 value) {
        ASSERT_TRUE(memory.write_u32(resource_base + offset, value).is_ok());
    };

    // Type dir: RT_ICON + RT_GROUP_ICON.
    write_u16(12, 0); write_u16(14, 2);
    write_u32(16, windowing::RT::ICON); write_u32(20, 0x80000020);
    write_u32(24, windowing::RT::GROUP_ICON); write_u32(28, 0x80000038);

    write_u16(0x20 + 12, 0); write_u16(0x20 + 14, 1);
    write_u32(0x20 + 16, 1); write_u32(0x20 + 20, 0x80000050);

    write_u16(0x38 + 12, 0); write_u16(0x38 + 14, 1);
    write_u32(0x38 + 16, 101); write_u32(0x38 + 20, 0x80000068);

    write_u16(0x50 + 12, 0); write_u16(0x50 + 14, 1);
    write_u32(0x50 + 16, 0x0409); write_u32(0x50 + 20, 0x80);
    write_u16(0x68 + 12, 0); write_u16(0x68 + 14, 1);
    write_u32(0x68 + 16, 0x0409); write_u32(0x68 + 20, 0x90);

    constexpr u32 icon_blob_off = 0xA0;
    constexpr u32 xor_stride = 4;
    constexpr u32 height = 8;
    constexpr u32 palette_bytes = 8;
    constexpr u32 xor_bytes = xor_stride * height;
    constexpr u32 and_bytes = xor_stride * height;
    constexpr u32 icon_blob_size = 40 + palette_bytes + xor_bytes + and_bytes;
    constexpr u32 group_blob_off = icon_blob_off + icon_blob_size;

    write_u32(0x80, resource_rva + icon_blob_off);
    write_u32(0x84, icon_blob_size);
    write_u32(0x90, resource_rva + group_blob_off);
    write_u32(0x94, 20);

    std::vector<u8> icon_blob(icon_blob_size, 0);
    icon_blob[0] = 40;
    icon_blob[4] = 8;
    icon_blob[8] = 16; // height * 2 for XOR+AND
    icon_blob[12] = 1;
    icon_blob[14] = 1;
    icon_blob[40 + 4] = 255;
    icon_blob[40 + 5] = 255;
    icon_blob[40 + 6] = 255;
    icon_blob[40 + palette_bytes + 7 * xor_stride] = 0x80;
    ASSERT_TRUE(memory.write_bytes(resource_base + icon_blob_off,
                                   icon_blob.data(),
                                   icon_blob.size()).is_ok());

    std::vector<u8> group_blob(20, 0);
    group_blob[2] = 1; // type = icon
    group_blob[4] = 1;
    group_blob[6] = 8;
    group_blob[7] = 8;
    group_blob[10] = 1;
    group_blob[12] = 1;
    group_blob[14] = static_cast<u8>(icon_blob_size);
    group_blob[18] = 1; // RT_ICON id
    ASSERT_TRUE(memory.write_bytes(resource_base + group_blob_off,
                                   group_blob.data(),
                                   group_blob.size()).is_ok());

    mgr.set_resource_info(image_base, resource_rva, 0x3000);
    const u32 first = mgr.load_icon(static_cast<u32>(image_base), 101);
    ASSERT_NE(first, 0u);
    const u32 second = mgr.load_icon(static_cast<u32>(image_base), 101);
    EXPECT_EQ(first, second);
    EXPECT_FALSE(mgr.destroy_icon(first));
}

TEST_F(ResourceTest, RegisteredIconsHonorSharedAndOwnedLifetime) {
    std::vector<u8> minimal_ico(23);
    minimal_ico[2] = 1;
    minimal_ico[4] = 1;
    minimal_ico[14] = 1; // bytesInRes
    minimal_ico[18] = 22; // imageOffset
    const ResourceManager::IconSourceKey source{0x400000, 126};

    const u32 shared_a = mgr.register_icon_image(minimal_ico, source, 16, 16);
    const u32 shared_b = mgr.register_icon_image(minimal_ico, source, 32, 32);
    ASSERT_NE(shared_a, 0u);
    EXPECT_EQ(shared_b, shared_a);
    EXPECT_FALSE(mgr.destroy_icon(shared_a));

    const u32 owned_a = mgr.register_icon_image(minimal_ico);
    const u32 owned_b = mgr.register_icon_image(minimal_ico);
    ASSERT_NE(owned_a, 0u);
    ASSERT_NE(owned_b, 0u);
    EXPECT_NE(owned_a, owned_b);
    EXPECT_TRUE(mgr.destroy_icon(owned_a));
    EXPECT_FALSE(mgr.destroy_icon(owned_a));
}

// ---- LoadString test ----

TEST_F(ResourceTest, LoadString_ReturnsZeroWithoutResourceInfo) {
    constexpr GuestAddr BUF = 0x900000;
    constexpr u32 BUF_SIZE = 64;
    (void)memory.map_region(BUF, BUF_SIZE, MemoryProtection::ReadWrite);

    i32 result = mgr.load_string(0, 100, BUF, 32);
    EXPECT_EQ(result, 0);
}

TEST_F(ResourceTest, ResourceLookupPrefersEnglishOverFirstLanguageEntry) {
    constexpr GuestAddr image_base = 0x400000;
    constexpr u32 resource_rva = 0x1000;
    constexpr GuestAddr resource_base = image_base + resource_rva;
    ASSERT_TRUE(memory.map_region(image_base, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());

    auto write_u16 = [&](u32 offset, u16 value) {
        ASSERT_TRUE(memory.write_u16(resource_base + offset, value).is_ok());
    };
    auto write_u32 = [&](u32 offset, u32 value) {
        ASSERT_TRUE(memory.write_u32(resource_base + offset, value).is_ok());
    };

    write_u16(12, 0); write_u16(14, 1);
    write_u32(16, windowing::RT::DIALOG); write_u32(20, 0x80000018);
    write_u16(24 + 12, 0); write_u16(24 + 14, 1);
    write_u32(40, 1000); write_u32(44, 0x80000030);
    write_u16(48 + 12, 0); write_u16(48 + 14, 2);
    write_u32(64, 0x0404); write_u32(68, 80);
    write_u32(72, 0x0409); write_u32(76, 96);
    write_u32(80, 0x2000); write_u32(84, 4);
    write_u32(96, 0x2100); write_u32(100, 4);
    ASSERT_TRUE(memory.write_u32(image_base + 0x2000, 0x04040404).is_ok());
    ASSERT_TRUE(memory.write_u32(image_base + 0x2100, 0x04090409).is_ok());

    auto entry = windowing::find_resource(memory, image_base, resource_rva,
                                           0x1000, windowing::RT::DIALOG, 1000);
    ASSERT_NE(entry.data, nullptr);
    EXPECT_EQ(entry.guest_addr, image_base + 0x2100);
    EXPECT_EQ(memory.read_u32(entry.guest_addr).value(), 0x04090409u);
}

// ---- Desktop / Foreground window tests ----

TEST_F(ResourceTest, GetDesktopWindow_ReturnsNonNull) {
    u32 hwnd = mgr.get_desktop_window();
    EXPECT_NE(hwnd, 0u);
    EXPECT_EQ(hwnd, windowing::HWND_DESKTOP);
    EXPECT_LT(hwnd, 0x00010000u);
}

TEST_F(ResourceTest, GetForegroundWindow_ReturnsNonNull) {
    u32 hwnd = mgr.get_foreground_window();
    EXPECT_NE(hwnd, 0u);
}

TEST_F(ResourceTest, GetForegroundWindowReturnsTrackedGuestWindow) {
    windowing::CocoaBridge bridge;
    mgr.set_cocoa_bridge(&bridge);
    constexpr windowing::HWND foreground = 0x10000;
    bridge.set_active_hwnd(foreground);

    EXPECT_EQ(mgr.get_foreground_window(), foreground);
}

TEST_F(ResourceTest, GetForegroundWindowThunkIgnoresStackContents) {
    constexpr GuestAddr stack = 0x80000;
    ASSERT_TRUE(memory.map_region(stack, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    auto exports = create_resource_thunks(mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetForegroundWindow";
        });
    ASSERT_NE(entry, exports.entries.end());

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), mgr.get_foreground_window());
}

TEST_F(ResourceTest, SetForegroundWindow_ReturnsTrue) {
    i32 result = mgr.set_foreground_window(0x12345);
    EXPECT_EQ(result, 1);
}

TEST_F(ResourceTest, IsDialogMessageDoesNotConsumeUnprocessedMessages) {
    constexpr GuestAddr stack = 0x81000;
    ASSERT_TRUE(memory.map_region(stack, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    const auto exports = create_resource_thunks(mgr);

    for (const char* name : {"IsDialogMessageA", "IsDialogMessageW"}) {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        ASSERT_NE(entry, exports.entries.end());

        cpu.gpr.rax = ~u64{0};
        entry->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u) << name;
    }
}

// ExitWindowsEx: a guest can neither log off the capsule's session nor shut
// down the Mac, so every request fails where it is made (Learn: shutdown and
// restart need SE_SHUTDOWN_NAME, which a guest is not granted).
TEST_F(ResourceTest, ExitWindowsExFailsInsteadOfFakingAShutdown) {
    const ExportEntry* exit_windows = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "ExitWindowsEx") exit_windows = &e;
    }
    ASSERT_NE(exit_windows, nullptr);
    EXPECT_EQ(exit_windows->stdcall_bytes, 8u);
    cpu.gpr.rcx = 0x1;   // EWX_SHUTDOWN
    cpu.gpr.rdx = 0;
    exit_windows->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1314u);   // ERROR_PRIVILEGE_NOT_HELD
    cpu.gpr.rcx = 0x0;   // EWX_LOGOFF
    exit_windows->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 50u);     // ERROR_NOT_SUPPORTED
}
