#include <gtest/gtest.h>

#include <graphics/contracts/cpu_surface.hpp>
#include <graphics/contracts/presentation_state.hpp>
#include <graphics/common/surface_presenter.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/graphics/ddraw/ddraw.hpp>
#include <graphics/contracts/backend_device.hpp>

#include <cstdlib>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

class RecordingSurfacePresenter final : public graphics::ISurfacePresenter {
public:
    bool present_indexed(u32 target, const graphics::CpuSurface& surface,
                         const u8* pixels, usize pixel_bytes) override {
        calls++;
        last_target = target;
        last_generation = surface.generation();
        last_pixel_bytes = pixel_bytes;
        first_pixel = pixels ? pixels[0] : 0;
        return true;
    }

    u32 calls{0};
    u32 last_target{0};
    u64 last_generation{0};
    usize last_pixel_bytes{0};
    u8 first_pixel{0};
};

class DirectDrawManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        manager.set_memory(&memory);
        manager.set_thunk_registry(&registry);
        manager.set_surface_presenter(&presenter);
        ASSERT_TRUE(registry.register_dll("ddraw.dll",
                                          create_ddraw_thunks(manager)).is_ok());
    }

    GuestMemory memory;
    ThunkRegistry registry{0x70000000};
    graphics::PresentationState presentation;
    RecordingSurfacePresenter presenter;
    DirectDrawManager manager{presentation};
};

TEST_F(DirectDrawManagerTest, CreateBuildsVtableAndReturnsObject) {
    constexpr GuestAddr output = 0x10000;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_NE(object, 0u);
    const GuestAddr vtable = memory.read_u32(object).value();
    ASSERT_NE(vtable, 0u);
    auto set_coop = registry.get_thunk_address(
        "ddraw.dll", "Vitreo_DDraw_SetCooperativeLevel");
    ASSERT_TRUE(set_coop.is_ok());
    EXPECT_EQ(memory.read_u32(vtable + 20 * 4).value(), set_coop.value());
}

TEST_F(DirectDrawManagerTest, CreateExUsesX64InterfaceAndThunkPointers) {
    ThunkRegistry high_registry{0x7ffb00000000};
    manager.set_memory(&memory, true);
    manager.set_thunk_registry(&high_registry);
    ASSERT_TRUE(high_registry.register_dll("ddraw.dll", create_ddraw_thunks(manager)).is_ok());
    constexpr GuestAddr output = 0x140000000;
    ASSERT_TRUE(memory.map_region(output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    constexpr std::array<u8, 16> iid{
        0xc0, 0x5e, 0xe6, 0x15, 0x9c, 0x3b, 0xd2, 0x11,
        0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b};
    ASSERT_TRUE(memory.write_bytes(output + 0x100, iid.data(), iid.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(output, UINT64_MAX).is_ok());
    ASSERT_EQ(manager.direct_draw_create_ex(0, output, output + 0x100, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u64(output).value();
    ASSERT_NE(object, 0u);
    const GuestAddr vtable = memory.read_u64(object).value();
    const auto thunk = high_registry.get_thunk_address("ddraw.dll", "Vitreo_DDraw2_GetAvailableVidMem");
    ASSERT_TRUE(thunk.is_ok());
    EXPECT_EQ(memory.read_u64(vtable + 23 * 8).value(), thunk.value());
    EXPECT_GT(thunk.value(), UINT32_MAX);
    ASSERT_EQ(manager.direct_draw_create_ex(0, output, output + 0x100, 1),
              ddraw::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(memory.read_u64(output).value(), 0u);
}

TEST_F(DirectDrawManagerTest, CreateExRejectsNonIDirectDraw7AndAggregation) {
    auto exports = create_ddraw_thunks(manager);
    const ExportEntry* create_ex = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "DirectDrawCreateEx") {
            create_ex = &e;
            break;
        }
    }
    ASSERT_NE(create_ex, nullptr);
    EXPECT_EQ(create_ex->stdcall_bytes, 16u);

    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x10080; // inside SetUp's 0x10000..0x10FFF map
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x100;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    struct Guid {
        u32 data1;
        u16 data2;
        u16 data3;
        u8 data4[8];
    };
    const Guid iid_dd = {
        0x6C14DB80u, 0xA733, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
    const Guid iid_dd7 = {
        0x15e65ec0u, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

    ASSERT_TRUE(memory.write_bytes(
        data + 0x40, reinterpret_cast<const Byte*>(&iid_dd), sizeof(iid_dd)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, data).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, data + 0x40).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    create_ex->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ddraw::DDERR_INVALIDPARAMS);

    ASSERT_TRUE(memory.write_bytes(
        data + 0x40, reinterpret_cast<const Byte*>(&iid_dd7), sizeof(iid_dd7)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0x1).is_ok());
    create_ex->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ddraw::CLASS_E_NOAGGREGATION);

    // IID_IDirectDraw7 with no aggregation is the supported form and yields a
    // real object on the IDirectDraw7 vtable, not E_NOINTERFACE — a caller
    // that asks for the interface by IID gets it.
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    create_ex->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ddraw::DD_OK);
    const GuestAddr created = memory.read_u32(data).value();
    ASSERT_NE(created, 0u);
    const GuestAddr created_vtable = memory.read_u32(created).value();
    ASSERT_NE(created_vtable, 0u);
    // Slot 29 is IDirectDraw7::EvaluateMode, the last of the six methods that
    // only exist on version 7.
    auto evaluate_mode =
        registry.get_thunk_address("ddraw.dll", "Vitreo_DDraw7_EvaluateMode");
    ASSERT_TRUE(evaluate_mode.is_ok());
    EXPECT_EQ(memory.read_u32(created_vtable + 29 * 4).value(),
              evaluate_mode.value());
}

TEST_F(DirectDrawManagerTest, CooperativeLevelAndDisplayModeTrackState) {
    constexpr GuestAddr output = 0x10000;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    constexpr u32 flags = ddraw::DDSCL_FULLSCREEN | ddraw::DDSCL_ALLOWREBOOT |
                          ddraw::DDSCL_EXCLUSIVE;
    EXPECT_EQ(manager.set_cooperative_level(object, 0x1234, flags), ddraw::DD_OK);
    EXPECT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    EXPECT_EQ(manager.cooperative_flags(object), flags);
    EXPECT_EQ(manager.display_width(object), 640u);
    EXPECT_EQ(manager.display_height(object), 480u);
    EXPECT_EQ(manager.display_bpp(object), 8u);
    EXPECT_EQ(presentation.target(), 0x1234u);
    EXPECT_EQ(presentation.display_mode().width, 640u);
}

TEST_F(DirectDrawManagerTest, SetDisplayModeNotifiesLogicalScreenMetrics) {
    u32 seen_hwnd = 0;
    u32 seen_w = 0;
    u32 seen_h = 0;
    u32 seen_bpp = 0;
    u32 restore_count = 0;
    manager.set_display_mode_callback(
        [&](u32 hwnd, u32 width, u32 height, u32 bits) {
            if (width == 0 || height == 0) {
                ++restore_count;
                return;
            }
            seen_hwnd = hwnd;
            seen_w = width;
            seen_h = height;
            seen_bpp = bits;
        });

    constexpr GuestAddr output = 0x10000;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    constexpr u32 flags = ddraw::DDSCL_FULLSCREEN | ddraw::DDSCL_ALLOWREBOOT |
                          ddraw::DDSCL_EXCLUSIVE;
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, flags), ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    EXPECT_EQ(seen_hwnd, 0x1234u);
    EXPECT_EQ(seen_w, 640u);
    EXPECT_EQ(seen_h, 480u);
    EXPECT_EQ(seen_bpp, 8u);

    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, ddraw::DDSCL_NORMAL),
              ddraw::DD_OK);
    EXPECT_EQ(restore_count, 1u);
}

TEST_F(DirectDrawManagerTest, DisplayModeRequiresExclusiveCooperativeLevel) {
    constexpr GuestAddr output = 0x10000;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, ddraw::DDSCL_NORMAL),
              ddraw::DD_OK);
    EXPECT_NE(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, CreateRejectsAggregationAndNonDefaultGuid) {
    constexpr GuestAddr output = 0x10000;
    EXPECT_EQ(manager.direct_draw_create(0, output, 1),
              ddraw::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(memory.read_u32(output).value(), 0u);
    EXPECT_EQ(manager.direct_draw_create(0x10100, output, 0),
              ddraw::DDERR_INVALIDDIRECTDRAWGUID);
}

TEST_F(DirectDrawManagerTest, PaletteCopiesAndUpdatesSharedColorEntries) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr entries = 0x10100;
    constexpr GuestAddr palette_output = 0x10500;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    for (u32 index = 0; index < 256; ++index) {
        ASSERT_TRUE(memory.write_u32(entries + index * 4, index).is_ok());
    }
    ASSERT_EQ(manager.create_palette(object, 0x44, entries, palette_output, 0),
              ddraw::DD_OK);
    const GuestAddr palette = memory.read_u32(palette_output).value();
    ASSERT_NE(palette, 0u);
    EXPECT_EQ(manager.palette_set_entries(palette, 0, 7, 1, entries + 12),
              ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(entries + 0x3F0, 0).is_ok());
    EXPECT_EQ(manager.palette_get_entries(palette, 0, 7, 1, entries + 0x3F0),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(entries + 0x3F0).value(), 3u);
}

TEST_F(DirectDrawManagerTest, PrimarySurfaceLocksSharedIndexedPixels) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    constexpr GuestAddr entries = 0x10800;
    constexpr GuestAddr palette_output = 0x10C10;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    constexpr u32 flags = ddraw::DDSCL_FULLSCREEN | ddraw::DDSCL_ALLOWREBOOT |
                          ddraw::DDSCL_EXCLUSIVE;
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, flags), ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();
    ASSERT_NE(surface, 0u);
    for (u32 index = 0; index < 256; ++index) {
        ASSERT_TRUE(memory.write_u32(entries + index * 4, index).is_ok());
    }
    ASSERT_EQ(manager.create_palette(object, 0x44, entries, palette_output, 0),
              ddraw::DD_OK);
    const GuestAddr palette = memory.read_u32(palette_output).value();
    ASSERT_EQ(manager.surface_set_palette(surface, palette), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    EXPECT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 12).value(), 640u);
    EXPECT_EQ(memory.read_u32(descriptor + 16).value(), 640u);
    const GuestAddr pixels = memory.read_u32(descriptor + 36).value();
    EXPECT_NE(pixels, 0u);
    ASSERT_TRUE(memory.write_u8(pixels, 42).is_ok());
    EXPECT_EQ(manager.surface_unlock(surface, pixels), ddraw::DD_OK);
    EXPECT_EQ(presenter.calls, 1u);
    EXPECT_EQ(presenter.last_target, 0x1234u);
    EXPECT_EQ(presenter.last_generation, 2u);
    EXPECT_EQ(presenter.last_pixel_bytes, 640u * 480u);
    EXPECT_EQ(presenter.first_pixel, 42u);
    ASSERT_TRUE(memory.write_u32(entries, 0x00030201).is_ok());
    EXPECT_EQ(manager.palette_set_entries(palette, 0, 42, 1, entries),
              ddraw::DD_OK);
    EXPECT_EQ(presenter.calls, 2u);
}

TEST_F(DirectDrawManagerTest, PrimarySurfaceLockRectReturnsInteriorPointer) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    constexpr GuestAddr rect_addr = 0x10D00;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    constexpr u32 flags = ddraw::DDSCL_FULLSCREEN | ddraw::DDSCL_ALLOWREBOOT |
                          ddraw::DDSCL_EXCLUSIVE;
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, flags), ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();

    // RECT { left=0, top=400, right=640, bottom=432 } — StarCraft Loading strip.
    ASSERT_TRUE(memory.write_u32(rect_addr + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(rect_addr + 4, 400).is_ok());
    ASSERT_TRUE(memory.write_u32(rect_addr + 8, 640).is_ok());
    ASSERT_TRUE(memory.write_u32(rect_addr + 12, 432).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, rect_addr, descriptor, 1, 0),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 8).value(), 32u);
    EXPECT_EQ(memory.read_u32(descriptor + 12).value(), 640u);
    EXPECT_EQ(memory.read_u32(descriptor + 16).value(), 640u);
    const GuestAddr base = memory.read_u32(descriptor + 36).value();
    // Interior pointer: top * pitch + left.
    // Full-surface Lock would return the base; RECT must offset by 400*640.
    ASSERT_EQ(manager.surface_unlock(surface, base), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr full = memory.read_u32(descriptor + 36).value();
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, rect_addr, descriptor, 1, 0),
              ddraw::DD_OK);
    const GuestAddr rect_ptr = memory.read_u32(descriptor + 36).value();
    EXPECT_EQ(rect_ptr, full + 400u * 640u);
    EXPECT_EQ(manager.surface_unlock(surface, rect_ptr), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, GetCapsReportsVideoMemoryAtDocumentedOffsets) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr caps = 0x10600;
    constexpr u32 caps_size = 316;  // DDCAPS_DX7
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_TRUE(memory.write_u32(caps, caps_size).is_ok());
    ASSERT_EQ(manager.get_caps(object, caps, 0), ddraw::DD_OK);

    // dwCaps: the three capabilities Vitreo actually has. The values matter --
    // 0x00080000 is DDCAPS_VBI and 0x00100000 is DDCAPS_ZBLTS, so the wrong
    // constants advertised vertical-blank interrupts and z-buffer blits.
    constexpr u32 DDCAPS_GDI = 0x00000400;
    constexpr u32 DDCAPS_PALETTE = 0x00008000;
    constexpr u32 DDCAPS_NOHARDWARE = 0x02000000;
    EXPECT_EQ(memory.read_u32(caps + 4).value(),
              DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_NOHARDWARE);

    // dwVidMemTotal/dwVidMemFree are the 16th and 17th DWORDs of DDCAPS.
    constexpr u32 pool = 64u * 1024u * 1024u;
    EXPECT_EQ(memory.read_u32(caps + 60).value(), pool);
    EXPECT_EQ(memory.read_u32(caps + 64).value(), pool);
    // +72 is dwCurrVisibleOverlays and +76 is dwNumFourCCCodes. Writing the
    // pool size here previously claimed 67 million FourCC codes.
    EXPECT_EQ(memory.read_u32(caps + 72).value(), 0u);
    EXPECT_EQ(memory.read_u32(caps + 76).value(), 0u);
}

TEST_F(DirectDrawManagerTest, GetCapsSeparatesHardwareAndHelCapabilities) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr driver_caps = 0x10600;
    constexpr GuestAddr hel_caps = 0x10800;
    constexpr u32 caps_size = 316;  // DDCAPS_DX7
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_TRUE(memory.write_u32(driver_caps, caps_size).is_ok());
    ASSERT_TRUE(memory.write_u32(hel_caps, caps_size).is_ok());
    ASSERT_EQ(manager.get_caps(object, driver_caps, hel_caps), ddraw::DD_OK);

    constexpr u32 DDCAPS_BLT = 0x00000040;
    constexpr u32 DDCAPS_GDI = 0x00000400;
    constexpr u32 DDCAPS_PALETTE = 0x00008000;
    constexpr u32 DDCAPS_COLORKEY = 0x00400000;
    constexpr u32 DDCAPS_NOHARDWARE = 0x02000000;
    constexpr u32 DDCAPS_BLTCOLORFILL = 0x04000000;
    constexpr u32 DDCAPS_CANBLTSYSMEM = 0x80000000;
    constexpr u32 DDCKEYCAPS_SRCBLT = 0x00000200;

    EXPECT_EQ(memory.read_u32(driver_caps + 4).value(),
              DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_NOHARDWARE);
    EXPECT_EQ(memory.read_u32(driver_caps + 12).value(), 0u);

    EXPECT_EQ(memory.read_u32(hel_caps + 4).value(),
              DDCAPS_BLT | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_COLORKEY |
                  DDCAPS_BLTCOLORFILL | DDCAPS_CANBLTSYSMEM);
    EXPECT_EQ(memory.read_u32(hel_caps + 12).value(), DDCKEYCAPS_SRCBLT);
    EXPECT_EQ(memory.read_u32(hel_caps + 4).value() & DDCAPS_NOHARDWARE, 0u);
}

TEST_F(DirectDrawManagerTest, VerticalBlankMethodsValidateAndWriteOutputs) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr status = 0x10600;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();

    EXPECT_EQ(manager.get_vertical_blank_status(0xDEADBEEF, status),
              ddraw::DDERR_INVALIDOBJECT);
    EXPECT_EQ(manager.get_vertical_blank_status(object, 0),
              ddraw::DDERR_INVALIDPARAMS);
    ASSERT_EQ(manager.get_vertical_blank_status(object, status), ddraw::DD_OK);
    EXPECT_LE(memory.read_u32(status).value(), 1u);

    EXPECT_EQ(manager.wait_for_vertical_blank(0xDEADBEEF, 1, 0),
              ddraw::DDERR_INVALIDOBJECT);
    EXPECT_EQ(manager.wait_for_vertical_blank(object, 0, 0),
              ddraw::DDERR_INVALIDPARAMS);
    EXPECT_EQ(manager.wait_for_vertical_blank(object, 3, 0),
              ddraw::DDERR_INVALIDPARAMS);
    EXPECT_EQ(manager.wait_for_vertical_blank(object, 2, 0),
              ddraw::E_NOTIMPL);
    EXPECT_EQ(manager.wait_for_vertical_blank(object, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.wait_for_vertical_blank(object, 4, 0), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, SecondLockReportsSurfaceBusy) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    EXPECT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0),
              ddraw::DDERR_SURFACEBUSY);
    // hEvent is reserved and must be NULL; that is a parameter error, not an
    // unimplemented method.
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    EXPECT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0x999),
              ddraw::DDERR_INVALIDPARAMS);
}

TEST_F(DirectDrawManagerTest, LockReportsSurfacePixelFormat) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 32), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();

    // Poison the DDPIXELFORMAT block so a stale mask cannot pass by accident.
    for (u32 offset = 72; offset < 104; offset += 4) {
        ASSERT_TRUE(memory.write_u32(descriptor + offset, 0xDEADBEEF).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 72).value(), 32u);
    EXPECT_EQ(memory.read_u32(descriptor + 76).value(), 0x40u);  // DDPF_RGB
    EXPECT_EQ(memory.read_u32(descriptor + 84).value(), 32u);
    EXPECT_EQ(memory.read_u32(descriptor + 88).value(), 0x00FF0000u);
    EXPECT_EQ(memory.read_u32(descriptor + 92).value(), 0x0000FF00u);
    EXPECT_EQ(memory.read_u32(descriptor + 96).value(), 0x000000FFu);
    EXPECT_EQ(memory.read_u32(descriptor + 80).value(), 0u);  // dwFourCC
}

TEST_F(DirectDrawManagerTest, OffscreenSurfaceBltRestoresPrimaryRect) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr primary_out = 0x10700;
    constexpr GuestAddr offscreen_out = 0x10710;
    constexpr GuestAddr rect = 0x10800;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 32, 8), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, primary_out, 0),
              ddraw::DD_OK);
    const GuestAddr primary = memory.read_u32(primary_out).value();

    // DDSD_CAPS|HEIGHT|WIDTH, DDSCAPS_OFFSCREENPLAIN|SYSTEMMEMORY
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x7).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 12, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x840).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, offscreen_out, 0),
              ddraw::DD_OK);
    const GuestAddr offscreen = memory.read_u32(offscreen_out).value();
    ASSERT_NE(offscreen, 0u);
    ASSERT_NE(offscreen, primary);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_get_surface_desc(offscreen, descriptor),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 8).value(), 32u);
    EXPECT_EQ(memory.read_u32(descriptor + 12).value(), 64u);
    EXPECT_EQ(memory.read_u32(descriptor + 16).value(), 64u);
    EXPECT_EQ(memory.read_u32(descriptor + 104).value(), 0x840u);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr primary_pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_TRUE(memory.write_u8(primary_pixels + 10 + 5 * 64, 0xAB).is_ok());
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);

    // Save rect (8,4)-(24,20) to offscreen, then trash primary, then restore.
    ASSERT_TRUE(memory.write_u32(rect + 0, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 8, 24).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 12, 20).is_ok());
    ASSERT_EQ(manager.surface_blt(offscreen, rect, primary, rect, 0x01000000, 0),
              ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u8(primary_pixels + 10 + 5 * 64, 0x00).is_ok());
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);

    ASSERT_EQ(manager.surface_blt(primary, rect, offscreen, rect, 0x01000000, 0),
              ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u8(primary_pixels + 10 + 5 * 64).value(), 0xABu);
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);
    EXPECT_GE(presenter.calls, 1u);
}

TEST_F(DirectDrawManagerTest, BltIntoLockedSurfaceReturnsSurfaceBusy) {
    // learn.microsoft.com Lock remarks / Blt return codes: blit of a locked
    // surface fails with DDERR_SURFACEBUSY (or DDERR_LOCKEDSURFACES).
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr primary_out = 0x10700;
    constexpr GuestAddr offscreen_out = 0x10710;
    constexpr GuestAddr rect = 0x10800;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 32, 16, 8), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, primary_out, 0),
              ddraw::DD_OK);
    const GuestAddr primary = memory.read_u32(primary_out).value();

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x7).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 12, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x840).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, offscreen_out, 0),
              ddraw::DD_OK);
    const GuestAddr offscreen = memory.read_u32(offscreen_out).value();

    ASSERT_TRUE(memory.write_u32(rect + 0, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 8, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 12, 2).is_ok());

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_blt(primary, rect, offscreen, rect, 0x01000000, 0),
              ddraw::DDERR_SURFACEBUSY);
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(offscreen, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_blt(primary, rect, offscreen, rect, 0x01000000, 0),
              ddraw::DDERR_SURFACEBUSY);
    ASSERT_EQ(manager.surface_unlock(offscreen, 0), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, BltFastAndSrcColorKeySkipTransparentPixels) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr primary_out = 0x10700;
    constexpr GuestAddr offscreen_out = 0x10710;
    constexpr GuestAddr src_rect = 0x10800;
    constexpr GuestAddr color_key = 0x10820;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 16, 8, 8), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, primary_out, 0),
              ddraw::DD_OK);
    const GuestAddr primary = memory.read_u32(primary_out).value();

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x7).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 12, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x840).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, offscreen_out, 0),
              ddraw::DD_OK);
    const GuestAddr offscreen = memory.read_u32(offscreen_out).value();

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(offscreen, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr src_pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_TRUE(memory.write_u8(src_pixels + 0, 0x00).is_ok());  // key
    ASSERT_TRUE(memory.write_u8(src_pixels + 1, 0x7F).is_ok());  // opaque
    ASSERT_EQ(manager.surface_unlock(offscreen, 0), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr dest_pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_TRUE(memory.write_u8(dest_pixels + 0, 0x11).is_ok());
    ASSERT_TRUE(memory.write_u8(dest_pixels + 1, 0x22).is_ok());
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(color_key + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(color_key + 4, 0).is_ok());
    ASSERT_EQ(manager.surface_set_color_key(offscreen, 0x8, color_key),
              ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(src_rect + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(src_rect + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(src_rect + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(src_rect + 12, 1).is_ok());
    // DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT
    ASSERT_EQ(manager.surface_blt_fast(primary, 0, 0, offscreen, src_rect,
                                       0x00000011),
              ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u8(dest_pixels + 0).value(), 0x11u);  // key skipped
    EXPECT_EQ(memory.read_u8(dest_pixels + 1).value(), 0x7Fu);
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, PrimaryReleaseInvalidatesPresenterWithoutCrash) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    struct TrackingPresenter final : graphics::ISurfacePresenter {
        const graphics::CpuSurface* last{nullptr};
        u32 invalidations{0};
        bool present_indexed(u32, const graphics::CpuSurface& surface,
                             const u8*, usize) override {
            last = &surface;
            return true;
        }
        void invalidate_surface(const graphics::CpuSurface* surface) noexcept
            override {
            if (surface == last) {
                last = nullptr;
                ++invalidations;
            }
        }
    } tracking;
    manager.set_surface_presenter(&tracking);

    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 32, 16, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);
    EXPECT_NE(tracking.last, nullptr);
    EXPECT_EQ(manager.surface_release(surface), 0u);
    EXPECT_EQ(tracking.invalidations, 1u);
    EXPECT_EQ(tracking.last, nullptr);
}

TEST_F(DirectDrawManagerTest, IsLostAndRestoreSucceedWhenSurfacePresent) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();

    // DD_OK from IsLost means the surface memory is still attached. Vitreo never
    // frees it on a mode switch, so this must not be E_NOTIMPL / SURFACELOST.
    EXPECT_EQ(manager.surface_is_lost(surface), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_restore(surface), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_is_lost(0), ddraw::DDERR_INVALIDOBJECT);
    EXPECT_EQ(manager.surface_restore(0), ddraw::DDERR_INVALIDOBJECT);
}

TEST(SurfacePresenterTest, ExpandsIndexedPixelsWithPitchToBgra) {
    graphics::CpuSurface surface{2, 2, 8, 4};
    auto palette = std::make_shared<graphics::IndexedColorTable>();
    const graphics::PaletteEntry colors[] = {
        {1, 2, 3, 0}, {10, 20, 30, 0}, {40, 50, 60, 0},
    };
    ASSERT_TRUE(palette->set_entries(0, 3, colors));
    surface.set_palette(palette);
    const u8 pixels[] = {0, 1, 99, 99, 2, 0, 99, 99};
    std::vector<u8> bgra;

    ASSERT_TRUE(graphics::expand_indexed_bgra(surface, pixels, sizeof(pixels), bgra));
    const std::vector<u8> expected = {
        3, 2, 1, 255, 30, 20, 10, 255,
        60, 50, 40, 255, 3, 2, 1, 255,
    };
    EXPECT_EQ(bgra, expected);
}

TEST(SurfacePresenterTest, AspectFitDefaultsToFractionalStretch) {
    // 640×480 into 1920×1080: default is fractional 2.25× (linear present),
    // not 2× nearest — thin UI bitmap fonts look crushed under integer upscale.
    unsetenv("VITREO_INTEGER_SCALE");
    const auto fitted = graphics::aspect_fit_rect(640, 480, 1920, 1080);
    EXPECT_DOUBLE_EQ(fitted.x, 240);
    EXPECT_DOUBLE_EQ(fitted.y, 0);
    EXPECT_DOUBLE_EQ(fitted.width, 1440);
    EXPECT_DOUBLE_EQ(fitted.height, 1080);
    EXPECT_FALSE(graphics::presentation_scale_is_integer(640, 480, fitted));
}

TEST(SurfacePresenterTest, AspectFitAllowsFractionalBelowTwo) {
    // 1600×1200 into 2560×1440: max integer scale is 1; default still
    // stretches (RCT3's highest preset) rather than huge letterboxing.
    unsetenv("VITREO_INTEGER_SCALE");
    const auto fitted = graphics::aspect_fit_rect(1600, 1200, 2560, 1440);
    EXPECT_DOUBLE_EQ(fitted.x, 320);
    EXPECT_DOUBLE_EQ(fitted.y, 0);
    EXPECT_DOUBLE_EQ(fitted.width, 1920);
    EXPECT_DOUBLE_EQ(fitted.height, 1440);
    EXPECT_FALSE(graphics::presentation_scale_is_integer(1600, 1200, fitted));
}

TEST(SurfacePresenterTest, AspectFitCentersWithinTallTarget) {
    unsetenv("VITREO_INTEGER_SCALE");
    const auto fitted = graphics::aspect_fit_rect(640, 480, 1024, 1280);
    EXPECT_DOUBLE_EQ(fitted.x, 0);
    EXPECT_DOUBLE_EQ(fitted.y, 256);
    EXPECT_DOUBLE_EQ(fitted.width, 1024);
    EXPECT_DOUBLE_EQ(fitted.height, 768);
}

TEST(SurfacePresenterTest, AspectFitKeepsExactSize) {
    unsetenv("VITREO_INTEGER_SCALE");
    const auto fitted = graphics::aspect_fit_rect(640, 480, 640, 480);
    EXPECT_DOUBLE_EQ(fitted.x, 0);
    EXPECT_DOUBLE_EQ(fitted.y, 0);
    EXPECT_DOUBLE_EQ(fitted.width, 640);
    EXPECT_DOUBLE_EQ(fitted.height, 480);
}

TEST(SurfacePresenterTest, AspectFitRejectsEmptyDimensions) {
    const auto fitted = graphics::aspect_fit_rect(640, 480, 0, 1080);
    EXPECT_DOUBLE_EQ(fitted.width, 0);
    EXPECT_DOUBLE_EQ(fitted.height, 0);
}

TEST(SurfacePresenterTest, IntegerScaleEnvForcesIntegerWhenAtLeastTwo) {
    setenv("VITREO_INTEGER_SCALE", "1", 1);
    const auto fitted = graphics::aspect_fit_rect(640, 480, 1920, 1080);
    EXPECT_DOUBLE_EQ(fitted.x, 320);
    EXPECT_DOUBLE_EQ(fitted.y, 60);
    EXPECT_DOUBLE_EQ(fitted.width, 1280);
    EXPECT_DOUBLE_EQ(fitted.height, 960);
    EXPECT_TRUE(graphics::presentation_scale_is_integer(640, 480, fitted));
    unsetenv("VITREO_INTEGER_SCALE");
}

TEST(SurfacePresenterTest, IntegerScaleEnvForcesOneTimes) {
    setenv("VITREO_INTEGER_SCALE", "1", 1);
    const auto fitted = graphics::aspect_fit_rect(1600, 1200, 2560, 1440);
    EXPECT_DOUBLE_EQ(fitted.width, 1600);
    EXPECT_DOUBLE_EQ(fitted.height, 1200);
    EXPECT_TRUE(graphics::presentation_scale_is_integer(1600, 1200, fitted));
    unsetenv("VITREO_INTEGER_SCALE");
}

TEST(SurfacePresenterTest, IntegerScaleEnvZeroForcesStretch) {
    setenv("VITREO_INTEGER_SCALE", "0", 1);
    const auto fitted = graphics::aspect_fit_rect(640, 480, 1920, 1080);
    EXPECT_DOUBLE_EQ(fitted.x, 240);
    EXPECT_DOUBLE_EQ(fitted.y, 0);
    EXPECT_DOUBLE_EQ(fitted.width, 1440);
    EXPECT_DOUBLE_EQ(fitted.height, 1080);
    unsetenv("VITREO_INTEGER_SCALE");
}

// =============================================================================
// IDirectDrawClipper
// =============================================================================

TEST_F(DirectDrawManagerTest, DirectDrawCreateClipperIsExportedWithStdcall12) {
    // The software renderers that resolve DirectDrawCreateClipper by name give
    // up entirely when GetProcAddress returns NULL, so the export's presence
    // is part of the contract, not an implementation detail.
    auto exports = create_ddraw_thunks(manager);
    const ExportEntry* entry = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "DirectDrawCreateClipper") {
            entry = &e;
            break;
        }
    }
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12u);
}

TEST_F(DirectDrawManagerTest, CreateClipperBuildsNineSlotVtable) {
    constexpr GuestAddr output = 0x10000;
    ASSERT_EQ(manager.direct_draw_create_clipper(0, output, 0), ddraw::DD_OK);
    const GuestAddr clipper = memory.read_u32(output).value();
    ASSERT_NE(clipper, 0u);
    const GuestAddr vtable = memory.read_u32(clipper).value();
    ASSERT_NE(vtable, 0u);
    // Slot order is IUnknown's three then GetClipList, GetHWnd, Initialize,
    // IsClipListChanged, SetClipList, SetHWnd.
    const char* const names[9] = {
        "Vitreo_DDClipper_QueryInterface", "Vitreo_DDClipper_AddRef",
        "Vitreo_DDClipper_Release",        "Vitreo_DDClipper_GetClipList",
        "Vitreo_DDClipper_GetHWnd",        "Vitreo_DDClipper_Initialize",
        "Vitreo_DDClipper_IsClipListChanged",
        "Vitreo_DDClipper_SetClipList",    "Vitreo_DDClipper_SetHWnd",
    };
    for (u32 slot = 0; slot < 9; ++slot) {
        auto thunk = registry.get_thunk_address("ddraw.dll", names[slot]);
        ASSERT_TRUE(thunk.is_ok()) << names[slot];
        EXPECT_EQ(memory.read_u32(vtable + slot * 4).value(), thunk.value())
            << names[slot];
    }
}

TEST_F(DirectDrawManagerTest, CreateClipperRejectsFlagsAndAggregation) {
    constexpr GuestAddr output = 0x10000;
    EXPECT_EQ(manager.direct_draw_create_clipper(1, output, 0),
              ddraw::DDERR_INVALIDPARAMS);
    EXPECT_EQ(manager.direct_draw_create_clipper(0, output, 0x1234),
              ddraw::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(memory.read_u32(output).value(), 0u);
}

TEST_F(DirectDrawManagerTest, ClipperInitializeOnlyOnceAndNotAfterCreateClipper) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr clipper_out = 0x10010;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();

    // Built by DirectDrawCreateClipper: unowned until Initialize.
    ASSERT_EQ(manager.direct_draw_create_clipper(0, clipper_out, 0),
              ddraw::DD_OK);
    const GuestAddr standalone = memory.read_u32(clipper_out).value();
    EXPECT_EQ(manager.clipper_initialize(standalone, object, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.clipper_initialize(standalone, object, 0),
              ddraw::DDERR_ALREADYINITIALIZED);

    // Built by IDirectDraw::CreateClipper: already associated.
    ASSERT_EQ(manager.create_clipper(object, 0, clipper_out, 0), ddraw::DD_OK);
    const GuestAddr owned = memory.read_u32(clipper_out).value();
    EXPECT_EQ(manager.clipper_initialize(owned, object, 0),
              ddraw::DDERR_ALREADYINITIALIZED);
}

TEST_F(DirectDrawManagerTest, ClipListRoundTripsAndReportsRequiredSize) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr region = 0x10100;
    constexpr GuestAddr readback = 0x10300;
    constexpr GuestAddr size = 0x10500;
    ASSERT_EQ(manager.direct_draw_create_clipper(0, output, 0), ddraw::DD_OK);
    const GuestAddr clipper = memory.read_u32(output).value();

    // With no region set at all there is nothing to report.
    ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
    EXPECT_EQ(manager.clipper_get_clip_list(clipper, 0, 0, size),
              ddraw::DDERR_NOCLIPLIST);

    // RGNDATAHEADER { dwSize=32, iType=RDH_RECTANGLES, nCount=2, nRgnSize } +
    // two RECTs.
    ASSERT_TRUE(memory.write_u32(region + 0, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 12, 32).is_ok());
    const i32 rects[2][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}};
    for (u32 i = 0; i < 2; ++i) {
        for (u32 j = 0; j < 4; ++j) {
            ASSERT_TRUE(memory.write_u32(region + 32 + i * 16 + j * 4,
                                         static_cast<u32>(rects[i][j])).is_ok());
        }
    }
    ASSERT_EQ(manager.clipper_set_clip_list(clipper, region, 0), ddraw::DD_OK);

    // The size-query form takes a null buffer and reports the byte count.
    ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
    ASSERT_EQ(manager.clipper_get_clip_list(clipper, 0, 0, size), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(size).value(), 32u + 2u * 16u);

    // Too small a buffer is refused rather than truncated.
    ASSERT_TRUE(memory.write_u32(size, 40).is_ok());
    EXPECT_EQ(manager.clipper_get_clip_list(clipper, 0, readback, size),
              ddraw::DDERR_REGIONTOOSMALL);

    ASSERT_TRUE(memory.write_u32(size, 256).is_ok());
    ASSERT_EQ(manager.clipper_get_clip_list(clipper, 0, readback, size),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(readback + 0).value(), 32u);   // dwSize
    EXPECT_EQ(memory.read_u32(readback + 4).value(), 1u);    // RDH_RECTANGLES
    EXPECT_EQ(memory.read_u32(readback + 8).value(), 2u);    // nCount
    // rcBound is the union of the two rectangles.
    EXPECT_EQ(memory.read_u32(readback + 16).value(), 10u);
    EXPECT_EQ(memory.read_u32(readback + 20).value(), 20u);
    EXPECT_EQ(memory.read_u32(readback + 24).value(), 70u);
    EXPECT_EQ(memory.read_u32(readback + 28).value(), 80u);
    for (u32 i = 0; i < 2; ++i) {
        for (u32 j = 0; j < 4; ++j) {
            EXPECT_EQ(memory.read_u32(readback + 32 + i * 16 + j * 4).value(),
                      static_cast<u32>(rects[i][j]));
        }
    }
}

TEST_F(DirectDrawManagerTest, ClipperHwndAndClipListAreMutuallyExclusive) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr region = 0x10100;
    constexpr GuestAddr hwnd_out = 0x10500;
    ASSERT_EQ(manager.direct_draw_create_clipper(0, output, 0), ddraw::DD_OK);
    const GuestAddr clipper = memory.read_u32(output).value();

    // Without a resolver no window can be attached; SetHWnd must not claim a
    // region it cannot produce.
    EXPECT_EQ(manager.clipper_set_hwnd(clipper, 0, 0x1234),
              ddraw::DDERR_INVALIDPARAMS);

    manager.set_client_region_callback(
        [](u32 hwnd, i32& left, i32& top, i32& right, i32& bottom) {
            if (hwnd != 0x1234) return false;
            left = 4;
            top = 8;
            right = 20;
            bottom = 24;
            return true;
        });
    ASSERT_EQ(manager.clipper_set_hwnd(clipper, 0, 0x1234), ddraw::DD_OK);
    ASSERT_EQ(manager.clipper_get_hwnd(clipper, hwnd_out), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(hwnd_out).value(), 0x1234u);

    ASSERT_TRUE(memory.write_u32(region + 0, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 12, 0).is_ok());
    EXPECT_EQ(manager.clipper_set_clip_list(clipper, region, 0),
              ddraw::DDERR_CLIPPERISUSINGHWND);
}

TEST_F(DirectDrawManagerTest, AttachedClipperConfinesBltToItsRegion) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr primary_out = 0x10700;
    constexpr GuestAddr clipper_out = 0x10710;
    constexpr GuestAddr region = 0x10100;
    constexpr GuestAddr rect = 0x10800;
    constexpr GuestAddr fx = 0x10900;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 32, 8), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x200).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, primary_out, 0),
              ddraw::DD_OK);
    const GuestAddr primary = memory.read_u32(primary_out).value();

    // Clip to x in [8,16), y in [4,8).
    ASSERT_EQ(manager.create_clipper(object, 0, clipper_out, 0), ddraw::DD_OK);
    const GuestAddr clipper = memory.read_u32(clipper_out).value();
    ASSERT_TRUE(memory.write_u32(region + 0, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 12, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 32 + 0, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 32 + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 32 + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(region + 32 + 12, 8).is_ok());
    ASSERT_EQ(manager.clipper_set_clip_list(clipper, region, 0), ddraw::DD_OK);
    ASSERT_EQ(manager.surface_set_clipper(primary, clipper), ddraw::DD_OK);

    // Colour-fill the whole surface; only the clip rectangle may change.
    ASSERT_TRUE(memory.write_u32(fx + 0x50, 0xCD).is_ok());
    constexpr u32 DDBLT_COLORFILL = 0x00000400;
    constexpr u32 DDBLT_WAIT = 0x01000000;
    ASSERT_TRUE(memory.write_u32(rect + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 8, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(rect + 12, 32).is_ok());
    ASSERT_EQ(manager.surface_blt(primary, rect, 0, 0,
                                  DDBLT_COLORFILL | DDBLT_WAIT, fx),
              ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr pixels = memory.read_u32(descriptor + 36).value();
    EXPECT_EQ(memory.read_u8(pixels + 8 + 4 * 64).value(), 0xCDu);   // inside
    EXPECT_EQ(memory.read_u8(pixels + 15 + 7 * 64).value(), 0xCDu);  // inside
    EXPECT_EQ(memory.read_u8(pixels + 7 + 4 * 64).value(), 0u);      // left of
    EXPECT_EQ(memory.read_u8(pixels + 16 + 4 * 64).value(), 0u);     // right of
    EXPECT_EQ(memory.read_u8(pixels + 8 + 3 * 64).value(), 0u);      // above
    EXPECT_EQ(memory.read_u8(pixels + 8 + 8 * 64).value(), 0u);      // below
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);
}

// =============================================================================
// IDirect3DDevice7
// =============================================================================

namespace {

/// Slot index into the IDirect3DDevice7 vtable plus the thunk that must sit
/// there. A wrong index dispatches a different method with a different stack
/// frame, so the ordering is asserted rather than assumed.
struct DeviceSlotCheck {
    u32 slot;
    const char* name;
};

} // namespace

class Direct3DDevice7Test : public DirectDrawManagerTest {
protected:
    /// Build an IDirectDraw object, a 32-bit render-target surface and a HAL
    /// device against it.
    void SetUp() override {
        DirectDrawManagerTest::SetUp();
        constexpr GuestAddr output = 0x10000;
        constexpr GuestAddr descriptor = 0x10600;
        constexpr GuestAddr surface_out = 0x10700;
        constexpr GuestAddr device_out = 0x10710;

        ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
        object = memory.read_u32(output).value();
        ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                                ddraw::DDSCL_FULLSCREEN |
                                                ddraw::DDSCL_EXCLUSIVE),
                  ddraw::DD_OK);
        ASSERT_EQ(manager.set_display_mode(object, 64, 32, 32), ddraw::DD_OK);

        // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT, 32bpp,
        // DDSCAPS_OFFSCREENPLAIN|DDSCAPS_3DDEVICE.
        ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
        ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x1007).is_ok());
        ASSERT_TRUE(memory.write_u32(descriptor + 8, 32).is_ok());
        ASSERT_TRUE(memory.write_u32(descriptor + 12, 64).is_ok());
        ASSERT_TRUE(memory.write_u32(descriptor + 84, 32).is_ok());
        ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x2040).is_ok());
        ASSERT_EQ(manager.create_surface(object, descriptor, surface_out, 0),
                  ddraw::DD_OK);
        surface = memory.read_u32(surface_out).value();

        write_hal_device_guid(clsid);
        ASSERT_EQ(manager.d3d7_create_device(object, clsid, surface, device_out),
                  ddraw::DD_OK);
        device = memory.read_u32(device_out).value();
        ASSERT_NE(device, 0u);
    }

    /// IID_IDirect3DHALDevice {84E63DE0-46AA-11CF-816F-0000C020156E} in the
    /// little-endian GUID memory layout: DWORD, WORD, WORD, then eight bytes.
    void write_hal_device_guid(GuestAddr address) {
        ASSERT_TRUE(memory.write_u32(address + 0, 0x84E63DE0u).is_ok());
        ASSERT_TRUE(memory.write_u32(address + 4, 0x11CF46AAu).is_ok());
        ASSERT_TRUE(memory.write_u32(address + 8, 0x00006F81u).is_ok());
        ASSERT_TRUE(memory.write_u32(address + 12, 0x6E1520C0u).is_ok());
    }

    static constexpr GuestAddr clsid = 0x10800;
    GuestAddr object{0};
    GuestAddr surface{0};
    GuestAddr device{0};
};

TEST_F(Direct3DDevice7Test, VtableSlotsMatchTheDocumentedOrder) {
    const GuestAddr vtable = memory.read_u32(device).value();
    ASSERT_NE(vtable, 0u);
    const DeviceSlotCheck checks[] = {
        {0, "Vitreo_D3D7Dev_QueryInterface"},
        {3, "Vitreo_D3D7Dev_GetCaps"},
        {5, "Vitreo_D3D7Dev_BeginScene"},
        {10, "Vitreo_D3D7Dev_Clear"},
        {11, "Vitreo_D3D7Dev_SetTransform"},
        {20, "Vitreo_D3D7Dev_SetRenderState"},
        {25, "Vitreo_D3D7Dev_DrawPrimitive"},
        {26, "Vitreo_D3D7Dev_DrawIndexedPrimitive"},
        {35, "Vitreo_D3D7Dev_SetTexture"},
        {37, "Vitreo_D3D7Dev_SetTextureStageState"},
        {44, "Vitreo_D3D7Dev_LightEnable"},
        {48, "Vitreo_D3D7Dev_GetInfo"},
    };
    for (const auto& check : checks) {
        auto thunk = registry.get_thunk_address("ddraw.dll", check.name);
        ASSERT_TRUE(thunk.is_ok()) << check.name;
        EXPECT_EQ(memory.read_u32(vtable + check.slot * 4).value(),
                  thunk.value())
            << check.name;
    }
}

TEST_F(Direct3DDevice7Test, CreateDeviceRefusesAnUndocumentedDeviceGuid) {
    constexpr GuestAddr device_out = 0x10710;
    ASSERT_TRUE(memory.write_u32(clsid + 0, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(manager.d3d7_create_device(object, clsid, surface, device_out),
              ddraw::DDERR_INVALIDPARAMS);
    EXPECT_EQ(memory.read_u32(device_out).value(), 0u);

    // A render target that is not one of this object's surfaces is refused
    // too: the device would have nothing to draw into.
    write_hal_device_guid(clsid);
    EXPECT_EQ(manager.d3d7_create_device(object, clsid, 0x999999, device_out),
              ddraw::DDERR_INVALIDPARAMS);
}

TEST_F(Direct3DDevice7Test, RenderStatesReadBackDocumentedD3D7Defaults) {
    constexpr GuestAddr value = 0x10900;
    // D3D7 powers up with Gouraud shading, LESSEQUAL depth compare and
    // specular highlights on — the last of which D3D8 flipped to FALSE, so it
    // cannot be inherited from the D3D9 defaults.
    ASSERT_EQ(manager.device_get_render_state(device, 9, value), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 2u);   // D3DSHADE_GOURAUD
    ASSERT_EQ(manager.device_get_render_state(device, 23, value), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 4u);   // D3DCMP_LESSEQUAL
    ASSERT_EQ(manager.device_get_render_state(device, 29, value), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 1u);   // SPECULARENABLE
    ASSERT_EQ(manager.device_get_render_state(device, 137, value), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 1u);   // LIGHTING

    ASSERT_EQ(manager.device_set_render_state(device, 137, 0), ddraw::DD_OK);
    ASSERT_EQ(manager.device_get_render_state(device, 137, value), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 0u);
}

TEST_F(Direct3DDevice7Test, TextureStageZeroDefaultsToModulate) {
    constexpr GuestAddr value = 0x10900;
    // Stage 0 must modulate the texture against the diffuse colour with no
    // SetTextureStageState call at all; every later stage starts disabled.
    ASSERT_EQ(manager.device_get_texture_stage_state(device, 0, 1, value),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 4u);   // D3DTOP_MODULATE
    ASSERT_EQ(manager.device_get_texture_stage_state(device, 1, 1, value),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 1u);   // D3DTOP_DISABLE
    ASSERT_EQ(manager.device_get_texture_stage_state(device, 3, 11, value),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(value).value(), 3u);   // TEXCOORDINDEX == stage
}

TEST_F(Direct3DDevice7Test, TransformsRoundTripAndMultiplyComposes) {
    constexpr GuestAddr matrix = 0x10900;
    constexpr GuestAddr readback = 0x10A00;
    const auto write_matrix = [&](GuestAddr address, const float* values) {
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &values[i], sizeof(bits));
            ASSERT_TRUE(memory.write_u32(address + i * 4, bits).is_ok());
        }
    };
    const auto read_element = [&](GuestAddr address, u32 index) {
        const u32 bits = memory.read_u32(address + index * 4).value();
        float out = 0.0f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    };

    // A scale of 2 in x and 3 in y, applied twice, must compose to 4 and 9.
    const float scale[16] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    write_matrix(matrix, scale);
    // D3DTRANSFORMSTATE_WORLD is 1 in D3D7 (D3D9 renumbered it to 256).
    ASSERT_EQ(manager.device_set_transform(device, 1, matrix), ddraw::DD_OK);
    ASSERT_EQ(manager.device_get_transform(device, 1, readback), ddraw::DD_OK);
    EXPECT_FLOAT_EQ(read_element(readback, 0), 2.0f);
    EXPECT_FLOAT_EQ(read_element(readback, 5), 3.0f);

    ASSERT_EQ(manager.device_multiply_transform(device, 1, matrix),
              ddraw::DD_OK);
    ASSERT_EQ(manager.device_get_transform(device, 1, readback), ddraw::DD_OK);
    EXPECT_FLOAT_EQ(read_element(readback, 0), 4.0f);
    EXPECT_FLOAT_EQ(read_element(readback, 5), 9.0f);

    // An unset transform reads back as identity.
    ASSERT_EQ(manager.device_get_transform(device, 3, readback), ddraw::DD_OK);
    EXPECT_FLOAT_EQ(read_element(readback, 0), 1.0f);
    EXPECT_FLOAT_EQ(read_element(readback, 1), 0.0f);
    // D3DTRANSFORMSTATE values outside the documented set are refused.
    EXPECT_EQ(manager.device_set_transform(device, 99, matrix),
              ddraw::E_INVALIDARG);
}

TEST_F(Direct3DDevice7Test, SceneNestingIsRejected) {
    EXPECT_EQ(manager.device_end_scene(device),
              ddraw::D3DERR_SCENE_NOT_IN_SCENE);
    ASSERT_EQ(manager.device_begin_scene(device), ddraw::DD_OK);
    EXPECT_EQ(manager.device_begin_scene(device), ddraw::D3DERR_SCENE_IN_SCENE);
    ASSERT_EQ(manager.device_end_scene(device), ddraw::DD_OK);
    EXPECT_EQ(manager.device_end_scene(device),
              ddraw::D3DERR_SCENE_NOT_IN_SCENE);
}

TEST_F(Direct3DDevice7Test, ClearFillsTheRequestedRectangleOfTheRenderTarget) {
    constexpr GuestAddr rects = 0x10900;
    constexpr GuestAddr descriptor = 0x10600;
    // D3DRECT { x1, y1, x2, y2 }.
    ASSERT_TRUE(memory.write_u32(rects + 0, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(rects + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(rects + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(rects + 12, 8).is_ok());
    constexpr u32 D3DCLEAR_TARGET = 0x1;
    ASSERT_EQ(manager.device_clear(device, 1, rects, D3DCLEAR_TARGET,
                                   0xFF204060u, 0, 0),
              ddraw::DD_OK);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    const GuestAddr pixels = memory.read_u32(descriptor + 36).value();
    const GuestAddr pitch = memory.read_u32(descriptor + 16).value();
    EXPECT_EQ(memory.read_u32(pixels + 8 * 4 + 4 * pitch).value(), 0xFF204060u);
    EXPECT_EQ(memory.read_u32(pixels + 15 * 4 + 7 * pitch).value(), 0xFF204060u);
    EXPECT_EQ(memory.read_u32(pixels + 7 * 4 + 4 * pitch).value(), 0u);
    EXPECT_EQ(memory.read_u32(pixels + 16 * 4 + 4 * pitch).value(), 0u);
    EXPECT_EQ(memory.read_u32(pixels + 8 * 4 + 8 * pitch).value(), 0u);
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);
}

TEST_F(Direct3DDevice7Test, ClearWithoutAnAttachedDepthBufferFailsHonestly) {
    constexpr u32 D3DCLEAR_ZBUFFER = 0x2;
    constexpr u32 D3DCLEAR_STENCIL = 0x4;
    // The device was built on a plain render target, so there is no depth or
    // stencil surface to clear and saying otherwise would be a lie.
    EXPECT_EQ(manager.device_clear(device, 0, 0, D3DCLEAR_ZBUFFER, 0, 0, 0),
              ddraw::D3DERR_ZBUFFER_NOTPRESENT);
    EXPECT_EQ(manager.device_clear(device, 0, 0, D3DCLEAR_STENCIL, 0, 0, 0),
              ddraw::D3DERR_STENCILBUFFER_NOTPRESENT);
}

TEST_F(Direct3DDevice7Test, LightsAreSetAndEnabledIndependently) {
    constexpr GuestAddr light = 0x10900;
    constexpr GuestAddr readback = 0x10A00;
    constexpr GuestAddr enabled = 0x10B00;
    const auto write_f32 = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(memory.write_u32(address, bits).is_ok());
    };
    // D3DLIGHT7: dltType at 0, diffuse at 4, range at 76.
    ASSERT_TRUE(memory.write_u32(light + 0, 3).is_ok());  // D3DLIGHT_DIRECTIONAL
    write_f32(light + 4, 0.25f);
    write_f32(light + 76, 500.0f);
    ASSERT_EQ(manager.device_set_light(device, 2, light), ddraw::DD_OK);

    // SetLight must not enable the light.
    ASSERT_EQ(manager.device_get_light_enable(device, 2, enabled), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(enabled).value(), 0u);
    ASSERT_EQ(manager.device_light_enable(device, 2, 1), ddraw::DD_OK);
    ASSERT_EQ(manager.device_get_light_enable(device, 2, enabled), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(enabled).value(), 1u);

    ASSERT_EQ(manager.device_get_light(device, 2, readback), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(readback + 0).value(), 3u);
    u32 bits = memory.read_u32(readback + 76).value();
    float range = 0.0f;
    std::memcpy(&range, &bits, sizeof(range));
    EXPECT_FLOAT_EQ(range, 500.0f);

    // A light index that was never set has nothing to report.
    EXPECT_EQ(manager.device_get_light(device, 5, readback),
              ddraw::DDERR_INVALIDPARAMS);
    // An out-of-range light type is not a light.
    ASSERT_TRUE(memory.write_u32(light + 0, 7).is_ok());
    EXPECT_EQ(manager.device_set_light(device, 2, light), ddraw::E_INVALIDARG);
}

TEST_F(Direct3DDevice7Test, DrawPrimitiveFailsRatherThanClaimingAFrame) {
    constexpr GuestAddr vertices = 0x10900;
    constexpr u32 D3DFVF_XYZ = 0x002;
    // Nothing rasterizes yet, so a draw must report failure rather than
    // DD_OK for a frame that never reached a render target.
    ASSERT_EQ(manager.device_begin_scene(device), ddraw::DD_OK);
    EXPECT_EQ(manager.device_draw_primitive(device, 4, D3DFVF_XYZ, vertices, 3,
                                            0),
              ddraw::E_NOTIMPL);
    ASSERT_EQ(manager.device_end_scene(device), ddraw::DD_OK);
    // Outside a scene the call is invalid regardless.
    EXPECT_EQ(manager.device_draw_primitive(device, 4, D3DFVF_XYZ, vertices, 3,
                                            0),
              ddraw::D3DERR_SCENE_NOT_IN_SCENE);
    // An undefined primitive type never reaches the pipeline description.
    ASSERT_EQ(manager.device_begin_scene(device), ddraw::DD_OK);
    EXPECT_EQ(manager.device_draw_primitive(device, 9, D3DFVF_XYZ, vertices, 3,
                                            0),
              ddraw::E_INVALIDARG);
    ASSERT_EQ(manager.device_end_scene(device), ddraw::DD_OK);
}

// =============================================================================
// Complex flipping chains
// =============================================================================
//
// A fullscreen guest asks for DDSCAPS_COMPLEX|DDSCAPS_FLIP with a back buffer
// count, reaches the buffers through GetAttachedSurface, draws into them, and
// calls Flip. SimCity 4 stops dead if any link of that is missing.

namespace {

/// Creates a flipping primary of `back_count` back buffers and returns it.
GuestAddr make_flip_chain(DirectDrawManager& manager, GuestMemory& memory,
                          GuestAddr object, GuestAddr descriptor,
                          GuestAddr output, u32 back_count) {
    constexpr u32 DDSD_CAPS = 0x1;
    constexpr u32 DDSD_BACKBUFFERCOUNT = 0x20;
    constexpr u32 DDSCAPS_BACKBUFFER_CHAIN = 0x218;  // PRIMARY|FLIP|COMPLEX
    for (u32 offset = 0; offset < 108; offset += 4) {
        (void)memory.write_u32(descriptor + offset, 0);
    }
    (void)memory.write_u32(descriptor, 108);
    (void)memory.write_u32(descriptor + 4, DDSD_CAPS | DDSD_BACKBUFFERCOUNT);
    (void)memory.write_u32(descriptor + 20, back_count);
    (void)memory.write_u32(descriptor + 104, DDSCAPS_BACKBUFFER_CHAIN);
    if (manager.create_surface(object, descriptor, output, 0) != ddraw::DD_OK) {
        return 0;
    }
    return memory.read_u32(output).value_or(0);
}

} // namespace

TEST_F(DirectDrawManagerTest, GetAttachedSurfaceWalksTheBackBufferChain) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10100;
    constexpr GuestAddr caps_in = 0x10300;
    constexpr GuestAddr attached = 0x10400;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    // SetDisplayMode is refused outside exclusive mode, which is the state a
    // fullscreen guest establishes first.
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, 0x11),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 32), ddraw::DD_OK);
    const GuestAddr primary =
        make_flip_chain(manager, memory, object, descriptor, output, 1);
    ASSERT_NE(primary, 0u);

    constexpr u32 DDSCAPS_BACKBUFFER = 0x4;
    constexpr u32 DDSCAPS_FRONTBUFFER = 0x20;
    ASSERT_TRUE(memory.write_u32(caps_in, DDSCAPS_BACKBUFFER).is_ok());
    ASSERT_EQ(manager.surface_get_attached_surface(primary, caps_in, attached),
              ddraw::DD_OK);
    const GuestAddr back = memory.read_u32(attached).value();
    EXPECT_NE(back, 0u);
    EXPECT_NE(back, primary);

    // The back buffer names the primary as its front buffer, which is how a
    // guest walks back up the chain.
    ASSERT_TRUE(memory.write_u32(caps_in, DDSCAPS_FRONTBUFFER).is_ok());
    ASSERT_EQ(manager.surface_get_attached_surface(back, caps_in, attached),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(attached).value(), primary);

    // Capabilities matching no attachment fail rather than guessing, and the
    // output pointer is cleared so a caller cannot read a stale surface.
    constexpr u32 DDSCAPS_ZBUFFER = 0x00020000;
    ASSERT_TRUE(memory.write_u32(caps_in, DDSCAPS_ZBUFFER).is_ok());
    EXPECT_EQ(manager.surface_get_attached_surface(primary, caps_in, attached),
              ddraw::DDERR_NOTFOUND);
    EXPECT_EQ(memory.read_u32(attached).value(), 0u);
}

TEST_F(DirectDrawManagerTest, FlipRotatesChainStorageAndPresents) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10100;
    constexpr GuestAddr caps_in = 0x10300;
    constexpr GuestAddr attached = 0x10400;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    // SetDisplayMode is refused outside exclusive mode, which is the state a
    // fullscreen guest establishes first.
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, 0x11),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 64, 32), ddraw::DD_OK);
    const GuestAddr primary =
        make_flip_chain(manager, memory, object, descriptor, output, 1);
    ASSERT_NE(primary, 0u);
    constexpr u32 DDSCAPS_BACKBUFFER = 0x4;
    ASSERT_TRUE(memory.write_u32(caps_in, DDSCAPS_BACKBUFFER).is_ok());
    ASSERT_EQ(manager.surface_get_attached_surface(primary, caps_in, attached),
              ddraw::DD_OK);
    const GuestAddr back = memory.read_u32(attached).value();

    // Find each surface's pixels through GetSurfaceDesc, then mark the back
    // buffer so the rotation is observable.
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_get_surface_desc(back, descriptor), ddraw::DD_OK);
    const GuestAddr back_pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_get_surface_desc(primary, descriptor),
              ddraw::DD_OK);
    const GuestAddr front_pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_NE(back_pixels, front_pixels);
    ASSERT_TRUE(memory.write_u32(back_pixels, 0xAABBCCDDu).is_ok());

    const u32 presents_before = presenter.calls;
    ASSERT_EQ(manager.surface_flip(primary, 0, 0), ddraw::DD_OK);
    EXPECT_EQ(presenter.calls, presents_before + 1);

    // Page-flip semantics: the interfaces keep their identity and the storage
    // moves, so the primary now reads what was drawn into the back buffer.
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_get_surface_desc(primary, descriptor),
              ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 36).value(), back_pixels);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_get_surface_desc(back, descriptor), ddraw::DD_OK);
    EXPECT_EQ(memory.read_u32(descriptor + 36).value(), front_pixels);
}

TEST_F(DirectDrawManagerTest, FlipRequiresAChainAndAnUnlockedSurface) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10100;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    // SetDisplayMode is refused outside exclusive mode, which is the state a
    // fullscreen guest establishes first.
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, 0x11),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 64, 32), ddraw::DD_OK);

    // A plain primary carries no chain, so Flip has nothing to rotate.
    constexpr u32 DDSD_CAPS = 0x1;
    constexpr u32 DDSCAPS_PRIMARYSURFACE = 0x200;
    for (u32 offset = 0; offset < 108; offset += 4) {
        ASSERT_TRUE(memory.write_u32(descriptor + offset, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, DDSD_CAPS).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, DDSCAPS_PRIMARYSURFACE).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, output, 0),
              ddraw::DD_OK);
    const GuestAddr plain = memory.read_u32(output).value();
    EXPECT_EQ(manager.surface_flip(plain, 0, 0), ddraw::DDERR_NOTFLIPPABLE);

    // Flipping while a link is locked would move storage out from under the
    // pointer the guest is still writing through.
    const GuestAddr primary =
        make_flip_chain(manager, memory, object, descriptor, output, 1);
    ASSERT_NE(primary, 0u);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(primary, 0, descriptor, 1, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_flip(primary, 0, 0), ddraw::DDERR_SURFACEBUSY);
    ASSERT_EQ(manager.surface_unlock(primary, 0), ddraw::DD_OK);
    EXPECT_EQ(manager.surface_flip(primary, 0, 0), ddraw::DD_OK);
}

TEST_F(DirectDrawManagerTest, GetPixelFormatDescribesTheSurfaceStandalone) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10100;
    constexpr GuestAddr format = 0x10300;
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    // SetDisplayMode is refused outside exclusive mode, which is the state a
    // fullscreen guest establishes first.
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234, 0x11),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 640, 480, 32), ddraw::DD_OK);
    const GuestAddr primary =
        make_flip_chain(manager, memory, object, descriptor, output, 1);
    ASSERT_NE(primary, 0u);

    // DDPIXELFORMAT stands alone here, so dwSize is at offset 0 rather than at
    // the +72 where DDSURFACEDESC embeds it.
    ASSERT_EQ(manager.surface_get_pixel_format(primary, format), ddraw::DD_OK);
    constexpr u32 DDPF_RGB = 0x40;
    EXPECT_EQ(memory.read_u32(format + 0).value(), 32u);   // dwSize
    EXPECT_EQ(memory.read_u32(format + 4).value(), DDPF_RGB);
    EXPECT_EQ(memory.read_u32(format + 12).value(), 32u);  // dwRGBBitCount
    EXPECT_EQ(memory.read_u32(format + 16).value(), 0x00FF0000u);
    EXPECT_EQ(memory.read_u32(format + 20).value(), 0x0000FF00u);
    EXPECT_EQ(memory.read_u32(format + 24).value(), 0x000000FFu);
}
