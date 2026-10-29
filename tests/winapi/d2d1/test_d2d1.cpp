// SPDX-License-Identifier: LGPL-2.1-only
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/common/path_geometry_math.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d2d1/d2d1.hpp"
#include "winapi/graphics/d2d1/d2d1_contract.inc"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include <cstring>
#include <gtest/gtest.h>
using namespace vitreo;
using namespace vitreo::winapi;
namespace {
struct Harness {
  GuestMemory memory;
  HandleTable handles;
  GdiManager gdi{handles};
  ThunkRegistry registry{0x7d000000};
  D2DManager manager;
  cpu::CpuState cpu{};
  Marshaler marshaler;
  bool wide;
  Harness(bool x64) : manager(memory, x64, &gdi), marshaler(memory), wide(x64) {
    (void)memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite);
    cpu.mode = x64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
    cpu.gpr.rsp = 0x18000;
    marshaler.attach_cpu(cpu);
    (void)registry.register_dll("d2d1.dll", create_d2d1_thunks(manager));
    EXPECT_TRUE(manager.bind_thunks(registry));
    manager.set_dpi_provider([] { return std::pair{96.f, 96.f}; });
    (void)memory.write_bytes(
        0x11000,
        reinterpret_cast<const Byte *>(d2d1_contract::factory_iid.data()), 16);
  }
  void args(std::initializer_list<u64> values) {
    cpu.pending_raise.active = false;
    cpu.gpr.rax = 0;
    usize i = 0;
    for (auto value : values) {
      if (wide && i < 4) {
        switch (i) {
        case 0:
          cpu.gpr.rcx = value;
          break;
        case 1:
          cpu.gpr.rdx = value;
          break;
        case 2:
          cpu.gpr.r8 = value;
          break;
        case 3:
          cpu.gpr.r9 = value;
          break;
        }
      } else if (wide)
        (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, value);
      else
        (void)memory.write_u32(cpu.gpr.rsp + 4 + i * 4,
                               static_cast<u32>(value));
      ++i;
    }
  }
  GuestAddr pointer(GuestAddr p) {
    return wide ? memory.read_u64(p).value_or(0)
                : memory.read_u32(p).value_or(0);
  }
  GuestAddr target(GuestAddr factory,
                   u32 alpha = d2d1_contract::D2D1_ALPHA_MODE_PREMULTIPLIED,
                   u32 type = d2d1_contract::D2D1_RENDER_TARGET_TYPE_DEFAULT) {
    const u32 p[] = {
        type, d2d1_contract::DXGI_FORMAT_B8G8R8A8_UNORM, alpha, 0, 0, 0, 0};
    (void)memory.write_bytes(0x13000, reinterpret_cast<const Byte *>(p),
                             sizeof(p));
    args({factory, 0x13000, 0x12010});
    manager.factory_method(marshaler, 16);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    return pointer(0x12010);
  }
  void floats(float x, float y) {
    u32 xb, yb;
    std::memcpy(&xb, &x, 4);
    std::memcpy(&yb, &y, 4);
    if (wide) {
      cpu.sse.xmm[1].u32_[0] = xb;
      cpu.sse.xmm[2].u32_[0] = yb;
    } else {
      (void)memory.write_u32(cpu.gpr.rsp + 8, xb);
      (void)memory.write_u32(cpu.gpr.rsp + 12, yb);
    }
  }
  GuestAddr create(u32 type = 0) {
    args({type, 0x11000, 0, 0x12000});
    manager.create_factory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    return pointer(0x12000);
  }
};
} // namespace
TEST(D2D1Factory, NativeWidthIdentityLifetimeAndDpiSnapshot) {
  for (bool wide : {false, true}) {
    SCOPED_TRACE(wide);
    Harness h(wide);
    float dpi = 120;
    h.manager.set_dpi_provider([&] { return std::pair{dpi, dpi}; });
    const auto factory = h.create();
    ASSERT_NE(factory, 0u);
    const auto vtable = h.pointer(factory);
    ASSERT_NE(vtable, 0u);
    for (usize slot = 0; slot < std::size(d2d1_contract::factory_methods);
         ++slot) {
      const auto name = std::string("_D2D1_Factory_") +
                        d2d1_contract::factory_methods[slot].name;
      EXPECT_EQ(h.pointer(vtable + slot * (wide ? 8 : 4)),
                h.registry.get_thunk_address("d2d1.dll", name, true).value());
    }
    h.args({factory, 0x11000, 0x12008});
    h.manager.factory_method(h.marshaler, 0);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_EQ(h.pointer(0x12008), factory);
    h.args({factory});
    h.manager.factory_method(h.marshaler, 2);
    EXPECT_EQ(h.cpu.gpr.rax, 1u);
    dpi = 144;
    h.args({factory, 0x12100, 0x12104});
    h.manager.factory_method(h.marshaler, 4);
    float actual;
    u32 bits = h.memory.read_u32(0x12100).value();
    std::memcpy(&actual, &bits, 4);
    EXPECT_EQ(actual, 120);
    h.args({factory});
    h.manager.factory_method(h.marshaler, 3);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    h.args({factory, 0x12100, 0x12104});
    h.manager.factory_method(h.marshaler, 4);
    bits = h.memory.read_u32(0x12100).value();
    std::memcpy(&actual, &bits, 4);
    EXPECT_EQ(actual, 144);
    h.args({factory});
    h.manager.factory_method(h.marshaler, 2);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.memory.read_u32(factory).is_err());
  }
}
#include "d2d1_factory_oracle.inc"
TEST(D2D1Factory, MatchesWindowsCreationValidationAndOutputMatrix) {
  for (bool wide : {false, true}) {
    Harness h(wide);
    ASSERT_TRUE(h.memory
                    .write_bytes(0x11020,
                                 reinterpret_cast<const Byte *>(
                                     d2d1_contract::unknown_iid.data()),
                                 16)
                    .is_ok());
    const u32 levels[] = {0, 1, 2, 3, 4, 0xffffffffu};
    for (const auto &row : factory_cases) {
      if (row.wide != wide)
        continue;
      SCOPED_TRACE(::testing::Message()
                   << wide << " type=" << row.type << " iid=" << row.iid
                   << " option=" << row.option << " output=" << row.out);
      const GuestAddr iid = row.iid == 0   ? 0x11000
                            : row.iid == 1 ? 0x11020
                            : row.iid == 2 ? 0x11040
                            : row.iid == 3 ? 0
                                           : 1;
      const GuestAddr output = row.out == 0 ? 0x12000 : row.out == 1 ? 0 : 1;
      ASSERT_TRUE(h.memory.write_u64(0x12000, 0x12345678).is_ok());
      ASSERT_TRUE(
          h.memory.write_u32(0x11100, levels[row.option < 6 ? row.option : 0])
              .is_ok());
      const GuestAddr options = row.option == 6   ? 0
                                : row.option == 7 ? 1
                                                  : 0x11100;
      h.args({row.type, iid, options, output});
      h.manager.create_factory(h.marshaler);
      EXPECT_EQ(h.cpu.pending_raise.active, row.fault);
      const auto object = h.pointer(0x12000);
      const u32 state = object == 0x12345678 ? 2 : object ? 1 : 0;
      EXPECT_EQ(state, row.state);
      if (!row.fault) {
        EXPECT_EQ(h.cpu.gpr.rax, row.result);
        if (row.state == 1) {
          h.args({object});
          h.manager.factory_method(h.marshaler, 2);
        }
      }
    }
  }
}
TEST(D2D1RenderTarget, DcClearSolidFillPixelsAndBatchStateMatchWindows) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    SCOPED_TRACE(wide);
    Harness h(wide);
    const auto factory = h.create();
    const u32 properties[] = {
        0, DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED, 0, 0, 0,
        0};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13000,
                                 reinterpret_cast<const Byte *>(properties),
                                 sizeof(properties))
                    .is_ok());
    h.args({factory, 0x13000, 0x12010});
    h.manager.factory_method(h.marshaler, 16);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto target = h.pointer(0x12010);
    ASSERT_NE(target, 0u);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, D2DERR_WRONG_STATE);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 0u);
    std::array<u8, 4 * 4 * 4> initial{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(4, 4, initial);
    ASSERT_NE(bitmap, 0u);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(dc, 0u);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bound[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13040, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    h.args({target, dc, 0x13040});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const float blue[] = {0, 0, 1, 1}, red[] = {1, 0, 0, 1},
                rectangle[] = {1, 1, 3, 3};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13060, reinterpret_cast<const Byte *>(blue),
                                 sizeof(blue))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13080, reinterpret_cast<const Byte *>(red),
                                 sizeof(red))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130a0,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, 0x13060, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto brush = h.pointer(0x12020);
    ASSERT_NE(brush, 0u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13080});
    h.manager.target_method(h.marshaler, target_Clear);
    h.args({target, 0x130a0, brush});
    h.manager.target_method(h.marshaler, target_FillRectangle);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    // Windows oracle DIB words ffff0000 / ff0000ff correspond to these
    // COLORREF values after BGRA-to-GDI channel conversion.
    EXPECT_EQ(h.gdi.get_pixel(dc, 0, 0), 0x000000ffu);
    EXPECT_EQ(h.gdi.get_pixel(dc, 1, 1), 0x00ff0000u);
    h.args(wide ? std::initializer_list<u64>{target, 31, 32}
                : std::initializer_list<u64>{target, 31, 0, 32, 0});
    h.manager.target_method(h.marshaler, target_SetTags);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, D2DERR_WRONG_STATE);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 31u);
    EXPECT_EQ(h.memory.read_u64(0x13408).value(), 32u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
  }
}
#include "d2d1_dc_pixels.inc"
TEST(D2D1RenderTarget,
     AlphaOpacityFractionalDpiTransformAndClipPixelsMatchWindows) {
  using namespace d2d1_contract;
  for (const auto &row : dc_pixels) {
    SCOPED_TRACE(::testing::Message()
                 << row.wide << " alpha=" << row.alpha << " aa=" << row.aa
                 << " variant=" << row.variant);
    Harness h(row.wide);
    const auto factory = h.create();
    const auto target = h.target(factory, row.alpha);
    ASSERT_NE(target, 0u);
    const std::array<u8, 64> pixels{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(4, 4, pixels);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bound[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13040, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    h.args({target, dc, 0x13040});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const float foreground[] = {.8f, .2f, .1f, .75f},
                background[] = {.2f, .4f, .6f, .5f};
    const float properties[] = {.5f, 1, 0, 0, 1, 0, 0};
    float rectangle[] = {1, 1, 3, 3};
    if (row.variant == 1) {
      rectangle[0] = rectangle[1] = .25f;
      rectangle[2] = rectangle[3] = 2.75f;
    }
    float transform[] = {1, 0, 0, 1, 0, 0};
    if (row.variant == 2) {
      transform[0] = transform[3] = 1.25f;
      transform[4] = transform[5] = .25f;
    }
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13060,
                                 reinterpret_cast<const Byte *>(foreground),
                                 sizeof(foreground))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13080,
                                 reinterpret_cast<const Byte *>(background),
                                 sizeof(background))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130a0,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130c0,
                                 reinterpret_cast<const Byte *>(properties),
                                 sizeof(properties))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13100,
                                 reinterpret_cast<const Byte *>(transform),
                                 sizeof(transform))
                    .is_ok());
    h.args({target, 0x13060, 0x130c0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto brush = h.pointer(0x12020);
    h.args({target, 0x13100});
    h.manager.target_method(h.marshaler, target_SetTransform);
    if (row.variant == 2) {
      h.args({target, 0, 0});
      h.floats(120, 144);
      h.manager.target_method(h.marshaler, target_SetDpi);
    }
    h.args({target, row.aa});
    h.manager.target_method(h.marshaler, target_SetAntialiasMode);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13080});
    h.manager.target_method(h.marshaler, target_Clear);
    if (row.variant == 3) {
      const float clip[] = {0, 0, 2, 2};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13140,
                                   reinterpret_cast<const Byte *>(clip),
                                   sizeof(clip))
                      .is_ok());
      h.args({target, 0x13140, D2D1_ANTIALIAS_MODE_ALIASED});
      h.manager.target_method(h.marshaler, target_PushAxisAlignedClip);
    }
    h.args({target, 0x130a0, brush});
    h.manager.target_method(h.marshaler, target_FillRectangle);
    if (row.variant == 3) {
      h.args({target});
      h.manager.target_method(h.marshaler, target_PopAxisAlignedClip);
    }
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    if (row.aa == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE &&
        (row.variant == 1 || row.variant == 2)) {
      EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
      continue;
    }
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.gdi.with_device_surface(dc, [&](auto &surface) {
      for (usize i = 0; i < 16; ++i) {
        const auto *p = surface.rgba + i * 4;
        const u32 value =
            (u32(p[3]) << 24) | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
        EXPECT_EQ(value, row.pixels[i]) << "pixel " << i;
      }
    }));
  }
}
TEST(D2D1RenderTarget,
     ResourceDomainsFactoryRetentionAndIndependentBrushLifetime) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(), other_factory = h.create();
    const auto target = h.target(factory), other = h.target(factory),
               foreign = h.target(other_factory);
    const float color[] = {.2f, .4f, .6f, .5f}, rectangle[] = {0, 0, 2, 2};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13200, reinterpret_cast<const Byte *>(color),
                                 sizeof(color))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13220,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, 0x13200, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto brush = h.pointer(0x12020);
    const std::array<u8, 16> pixels{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(2, 2, pixels);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bound[] = {0, 0, 2, 2};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13240, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    for (const auto &item : {std::pair{other, D2DERR_WRONG_RESOURCE_DOMAIN},
                             std::pair{foreign, D2DERR_WRONG_FACTORY}}) {
      h.args({item.first, dc, 0x13240});
      h.manager.target_method(h.marshaler, target_BindDC);
      h.args(wide ? std::initializer_list<u64>{item.first, 41, 42}
                  : std::initializer_list<u64>{item.first, 41, 0, 42, 0});
      h.manager.target_method(h.marshaler, target_SetTags);
      h.args({item.first});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({item.first, 0x13220, brush});
      h.manager.target_method(h.marshaler, target_FillRectangle);
      h.args({item.first, 0x13400, 0x13408});
      h.manager.target_method(h.marshaler, target_EndDraw);
      EXPECT_EQ(h.cpu.gpr.rax, item.second);
      EXPECT_EQ(h.memory.read_u64(0x13400).value(), 41u);
      EXPECT_EQ(h.memory.read_u64(0x13408).value(), 42u);
    }
    h.args({factory});
    h.manager.factory_method(h.marshaler, factory_Release);
    EXPECT_NE(h.cpu.gpr.rax, 0u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_Release);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.memory.read_u32(target).is_err());
    h.args({brush, 0x12030});
    h.manager.brush_method(h.marshaler, brush_GetFactory);
    EXPECT_EQ(h.pointer(0x12030), factory);
    h.args({factory});
    h.manager.factory_method(h.marshaler, factory_Release);
    h.args({brush, 0x13300});
    h.manager.brush_method(h.marshaler, brush_GetColor);
    EXPECT_EQ(h.cpu.gpr.rax, 0x13300u);
    std::array<float, 4> actual{};
    ASSERT_TRUE(h.memory
                    .read_bytes(0x13300,
                                reinterpret_cast<Byte *>(actual.data()),
                                sizeof(actual))
                    .is_ok());
    EXPECT_EQ(actual[0], color[0]);
    EXPECT_EQ(actual[3], color[3]);
    h.args({brush});
    h.manager.brush_method(h.marshaler, brush_Release);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_FALSE(h.memory.read_u32(factory)
                     .is_err()); // other same-factory target still owns it
    h.args({other});
    h.manager.target_method(h.marshaler, target_Release);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.memory.read_u32(factory).is_err());
  }
}

TEST(D2D1RenderTarget, UnsupportedDrawsReportFirstErrorTagsAndFlushRecovery) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(), target = h.target(factory);
    h.args(wide ? std::initializer_list<u64>{target, 0x1122334455667788ull,
                                             0x8877665544332211ull}
                : std::initializer_list<u64>{target, 0x55667788, 0x11223344,
                                             0x44332211, 0x88776655});
    h.manager.target_method(h.marshaler, target_SetTags);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.manager.target_method(h.marshaler, target_DrawEllipse);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_Flush);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 0x1122334455667788ull);
    EXPECT_EQ(h.memory.read_u64(0x13408).value(), 0x8877665544332211ull);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 0u);
    h.args({factory, 0x11000, 0});
    h.manager.factory_method(h.marshaler, factory_QueryInterface);
    EXPECT_EQ(h.cpu.gpr.rax, E_POINTER);
    EXPECT_FALSE(h.cpu.pending_raise.active);
    h.args({target, 0x11000, 0});
    h.manager.target_method(h.marshaler, target_QueryInterface);
    EXPECT_EQ(h.cpu.gpr.rax, E_POINTER);
    EXPECT_FALSE(h.cpu.pending_raise.active);
  }
}
#include <atomic>
#include <thread>
TEST(D2D1Factory, InterlockedReferencesRemainValidAcrossGuestThreads) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(D2D1_FACTORY_TYPE_MULTI_THREADED);
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; index++)
      workers.emplace_back([&, index] {
        cpu::CpuState cpu{};
        cpu.mode = wide ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        cpu.gpr.rsp = 0x14000 + index * 0x100;
        if (wide)
          cpu.gpr.rcx = factory;
        else if (h.memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(factory))
                     .is_err()) {
          ++failures;
          return;
        }
        Marshaler marshaler(h.memory);
        marshaler.attach_cpu(cpu);
        for (unsigned n = 0; n < 1000; n++) {
          h.manager.factory_method(marshaler, factory_AddRef);
          if (cpu.gpr.rax < 2 || cpu.pending_raise.active)
            ++failures;
          h.manager.factory_method(marshaler, factory_Release);
          if (cpu.gpr.rax < 1 || cpu.pending_raise.active)
            ++failures;
        }
      });
    for (auto &worker : workers)
      worker.join();
    EXPECT_EQ(failures, 0u);
    h.args({factory});
    h.manager.factory_method(h.marshaler, factory_Release);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
  }
}
TEST(D2D1Factory, MissingRuntimeBindingFailsWithoutPublishingFactory) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    D2DManager unbound(h.memory, wide);
    unbound.set_dpi_provider([] { return std::pair{96.f, 96.f}; });
    h.args({D2D1_FACTORY_TYPE_SINGLE_THREADED, 0x11000, 0, 0x12000});
    unbound.create_factory(h.marshaler);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(h.pointer(0x12000), 0u);
    h.manager.set_dpi_provider({});
    h.args({0, 0x11000, 0, 0x12000});
    h.manager.create_factory(h.marshaler);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(h.pointer(0x12000), 0u);
  }
}
TEST(D2D1RenderTarget,
     WindowRedirectionKeepsOpaqueCoverageWhileDibRetainsAlpha) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(), target = h.target(factory);
    // GdiManager's model window-DC fixture owns a redirection surface even
    // without a Cocoa presenter. It exposes the exact foreign-raster alpha
    // policy consumed by GDI/GDI+ and Direct2D.
    const auto dc = h.gdi.get_dc(0x1234);
    ASSERT_TRUE(h.gdi.with_device_surface(
        dc, [](auto &surface) { ASSERT_EQ(surface.alpha, 255u); }));
    const i32 rectangle[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13200,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, dc, 0x13200});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0});
    h.manager.target_method(h.marshaler, target_Clear);
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.gdi.with_device_surface(dc, [](auto &surface) {
      EXPECT_EQ(surface.rgba[0], 0u);
      EXPECT_EQ(surface.rgba[3], 255u);
    }));
  }
}
TEST(D2D1RenderTarget,
     InitialDcDpiAndFactoryCachedResetFollowDistinctDocumentedDefaults) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    float system_dpi = 120;
    h.manager.set_dpi_provider(
        [&] { return std::pair{system_dpi, system_dpi}; });
    const auto factory = h.create(), target = h.target(factory);
    const auto read_dpi = [&] {
      h.args({target, 0x13300, 0x13304});
      h.manager.target_method(h.marshaler, target_GetDpi);
      const auto bits = h.memory.read_u32(0x13300).value();
      float value;
      std::memcpy(&value, &bits, 4);
      return value;
    };
    EXPECT_EQ(read_dpi(), dip_screen_dpi);
    system_dpi = 144;
    h.args({target, 0, 0});
    h.floats(0, 0);
    h.manager.target_method(h.marshaler, target_SetDpi);
    EXPECT_EQ(read_dpi(),
              120.f); // provider change alone does not refresh factory
    h.args({factory});
    h.manager.factory_method(h.marshaler, factory_ReloadSystemMetrics);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_EQ(read_dpi(), 120.f); // existing target state stays unchanged
    h.args({target, 0, 0});
    h.floats(0, 0);
    h.manager.target_method(h.marshaler, target_SetDpi);
    EXPECT_EQ(read_dpi(), 144.f);
  }
}
TEST(D2D1RenderTarget, ValidHardwareMinimumDoesNotPreventSoftwareCreation) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create();
    for (const auto type :
         {D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1_RENDER_TARGET_TYPE_SOFTWARE})
      for (const auto level :
           {u32{0}, D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_10_0}) {
        SCOPED_TRACE(::testing::Message()
                     << wide << " type=" << type << " level=" << level);
        const u32 p[] = {type,
                         DXGI_FORMAT_B8G8R8A8_UNORM,
                         D2D1_ALPHA_MODE_PREMULTIPLIED,
                         0,
                         0,
                         0,
                         level};
        ASSERT_TRUE(h.memory
                        .write_bytes(0x13000, reinterpret_cast<const Byte *>(p),
                                     sizeof(p))
                        .is_ok());
        h.args({factory, 0x13000, 0x12010});
        h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
        EXPECT_EQ(h.cpu.gpr.rax, S_OK);
        const auto target = h.pointer(0x12010);
        EXPECT_NE(target, 0u);
        if (target) {
          h.args({target});
          h.manager.target_method(h.marshaler, target_Release);
        }
      }
  }
}
#include "d2d1_capabilities_oracle.inc"
TEST(D2D1RenderTarget, SoftwareIsSupportedMatchesFullWindowsPropertyMatrix) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create();
    for (u32 usage = 0; usage < 4; ++usage)
      for (u32 alpha :
           {D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_ALPHA_MODE_IGNORE}) {
        const u32 p[] = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                         DXGI_FORMAT_B8G8R8A8_UNORM,
                         alpha,
                         0,
                         0,
                         usage,
                         0};
        ASSERT_TRUE(h.memory
                        .write_bytes(0x13000, reinterpret_cast<const Byte *>(p),
                                     sizeof(p))
                        .is_ok());
        h.args({factory, 0x13000, 0x12010});
        h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        const auto target = h.pointer(0x12010);
        for (const auto &row : dc_capabilities) {
          if (row.wide != wide || row.owner_usage != usage ||
              row.owner_alpha != alpha)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << wide << " ownerusage=" << usage
                       << " owneralpha=" << alpha
                       << " props=" << ::testing::PrintToString(row.properties)
                       << " pointer=" << row.pointer);
          ASSERT_TRUE(h.memory
                          .write_bytes(0x13100,
                                       reinterpret_cast<const Byte *>(
                                           row.properties.data()),
                                       sizeof(row.properties))
                          .is_ok());
          const GuestAddr address = row.pointer == 2 ? 0x13100 : row.pointer;
          set_last_error(0x12345678);
          h.args({target, address});
          h.manager.target_method(h.marshaler, target_IsSupported);
          EXPECT_EQ(h.cpu.pending_raise.active, row.fault);
          if (!row.fault)
            EXPECT_EQ(h.cpu.gpr.rax, row.result);
          EXPECT_EQ(get_last_error(), row.last);
        }
        h.args({target});
        h.manager.target_method(h.marshaler, target_Release);
      }
  }
}
TEST(D2D1RenderTarget,
     HardwareOnlyNeverFallsBackAndCapabilityQueriesDoNotLatchDrawingErrors) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create();
    for (const auto level :
         {u32{0}, D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_10_0}) {
      const u32 p[] = {D2D1_RENDER_TARGET_TYPE_HARDWARE,
                       DXGI_FORMAT_B8G8R8A8_UNORM,
                       D2D1_ALPHA_MODE_IGNORE,
                       0,
                       0,
                       0,
                       level};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13000, reinterpret_cast<const Byte *>(p),
                                   sizeof(p))
                      .is_ok());
      ASSERT_TRUE(h.memory.write_u64(0x12010, 0x12345678).is_ok());
      h.args({factory, 0x13000, 0x12010});
      h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
      EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
      EXPECT_EQ(h.pointer(0x12010), 0u);
    }
    const auto target = h.target(factory);
    const std::array<u8, 16> pixels{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(2, 2, pixels),
               dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 rectangle[] = {0, 0, 2, 2};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13200,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, dc, 0x13200});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0});
    h.manager.target_method(h.marshaler, target_Clear);
    const u32 unsupported_query[] = {D2D1_RENDER_TARGET_TYPE_HARDWARE,
                                     DXGI_FORMAT_B8G8R8A8_UNORM,
                                     D2D1_ALPHA_MODE_PREMULTIPLIED,
                                     0,
                                     0,
                                     0,
                                     0};
    ASSERT_TRUE(
        h.memory
            .write_bytes(0x13300,
                         reinterpret_cast<const Byte *>(unsupported_query),
                         sizeof(unsupported_query))
            .is_ok());
    h.args({target, 0x13300});
    h.manager.target_method(h.marshaler, target_IsSupported);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, S_OK);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 0u);
    EXPECT_EQ(h.memory.read_u64(0x13408).value(), 0u);
  }
}
#include "graphics/metal/metal_device.hpp"
TEST(D2D1Hardware, BackendBindingRejectsReplacementAndLateChanges) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    auto *backend = &graphics::MetalDevice::instance();
    EXPECT_TRUE(h.manager.set_backend(backend));
    EXPECT_TRUE(h.manager.set_backend(backend));
    EXPECT_FALSE(h.manager.set_backend(nullptr));
    const auto factory = h.create();
    const u32 props[] = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                         DXGI_FORMAT_B8G8R8A8_UNORM,
                         D2D1_ALPHA_MODE_IGNORE,
                         0,
                         0,
                         0,
                         0};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13000, reinterpret_cast<const Byte *>(props),
                                 sizeof(props))
                    .is_ok());
    h.args({factory, 0x13000, 0x12010});
    h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    EXPECT_FALSE(h.manager.set_backend(backend));
    EXPECT_FALSE(h.manager.set_backend(nullptr));
  }
}
#include "graphics/common/hardware_solid_surface.hpp"
TEST(D2D1Hardware,
     RealGpuSolidSurfaceProducesClearAndBlendPixelsBeforeReadbackReturns) {
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal device unavailable";
  auto surface = graphics::HardwareSolidSurface::create(backend);
  ASSERT_NE(surface, nullptr);
  ASSERT_TRUE(surface->resize(4, 4));
  ASSERT_TRUE(surface->draw({0, 0, 4, 4}, {1, 0, 0, 1}, false, {0, 0, 4, 4}));
  ASSERT_TRUE(surface->draw({1, 1, 3, 3}, {0, 0, 1, 1}, true, {0, 0, 4, 4}));
  std::array<u8, 64> pixels{};
  ASSERT_TRUE(surface->readback(pixels));
  // Explicit HARDWARE Windows oracle: BGRA words ffff0000 and ff0000ff.
  u32 corner, center;
  std::memcpy(&corner, pixels.data(), 4);
  std::memcpy(&center, pixels.data() + 20, 4);
  EXPECT_EQ(corner, 0xffff0000u);
  EXPECT_EQ(center, 0xff0000ffu);
}

TEST(D2D1Hardware,
     NativeHardwareTargetExecutesGpuDrawAndPublishesExactDcPixels) {
  using namespace d2d1_contract;
  if (!graphics::MetalDevice::instance().available())
    GTEST_SKIP() << "Metal device unavailable";
  for (bool wide : {false, true}) {
    SCOPED_TRACE(wide);
    Harness h(wide);
    ASSERT_TRUE(h.manager.set_backend(&graphics::MetalDevice::instance()));
    const auto factory = h.create();
    const u32 properties[] = {D2D1_RENDER_TARGET_TYPE_HARDWARE,
                              DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED,
                              0,
                              0,
                              0,
                              0};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13000,
                                 reinterpret_cast<const Byte *>(properties),
                                 sizeof(properties))
                    .is_ok());
    h.args({factory, 0x13000, 0x12010});
    h.manager.factory_method(h.marshaler, 16);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto target = h.pointer(0x12010);
    ASSERT_NE(target, 0u);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, D2DERR_WRONG_STATE);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 0u);
    std::array<u8, 4 * 4 * 4> initial{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(4, 4, initial);
    ASSERT_NE(bitmap, 0u);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(dc, 0u);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bound[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13040, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    h.args({target, dc, 0x13040});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const float blue[] = {0, 0, 1, 1}, red[] = {1, 0, 0, 1},
                rectangle[] = {1, 1, 3, 3};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13060, reinterpret_cast<const Byte *>(blue),
                                 sizeof(blue))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13080, reinterpret_cast<const Byte *>(red),
                                 sizeof(red))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130a0,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, 0x13060, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto brush = h.pointer(0x12020);
    ASSERT_NE(brush, 0u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13080});
    h.manager.target_method(h.marshaler, target_Clear);
    h.args({target, 0x130a0, brush});
    h.manager.target_method(h.marshaler, target_FillRectangle);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    // Windows oracle DIB words ffff0000 / ff0000ff correspond to these
    // COLORREF values after BGRA-to-GDI channel conversion.
    EXPECT_EQ(h.gdi.get_pixel(dc, 0, 0), 0x000000ffu);
    EXPECT_EQ(h.gdi.get_pixel(dc, 1, 1), 0x00ff0000u);
    h.args(wide ? std::initializer_list<u64>{target, 31, 32}
                : std::initializer_list<u64>{target, 31, 0, 32, 0});
    h.manager.target_method(h.marshaler, target_SetTags);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, D2DERR_WRONG_STATE);
    EXPECT_EQ(h.memory.read_u64(0x13400).value(), 31u);
    EXPECT_EQ(h.memory.read_u64(0x13408).value(), 32u);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13400, 0x13408});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
  }
}
#include "d2d1_dc_hardware_pixels.inc"
TEST(D2D1Hardware, HardwareAlphaOpacityAliasedDpiTransformAndClipMatchWindows) {
  using namespace d2d1_contract;
  if (!graphics::MetalDevice::instance().available())
    GTEST_SKIP() << "Metal device unavailable";
  for (const auto &row : dc_hardware_pixels) {
    SCOPED_TRACE(::testing::Message()
                 << row.wide << " alpha=" << row.alpha << " aa=" << row.aa
                 << " variant=" << row.variant);
    Harness h(row.wide);
    ASSERT_TRUE(h.manager.set_backend(&graphics::MetalDevice::instance()));
    const auto factory = h.create();
    const auto target =
        h.target(factory, row.alpha, D2D1_RENDER_TARGET_TYPE_HARDWARE);
    ASSERT_NE(target, 0u);
    const std::array<u8, 64> pixels{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(4, 4, pixels);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bound[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13040, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    h.args({target, dc, 0x13040});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const float foreground[] = {.8f, .2f, .1f, .75f},
                background[] = {.2f, .4f, .6f, .5f};
    const float properties[] = {.5f, 1, 0, 0, 1, 0, 0};
    float rectangle[] = {1, 1, 3, 3};
    if (row.variant == 1) {
      rectangle[0] = rectangle[1] = .25f;
      rectangle[2] = rectangle[3] = 2.75f;
    }
    float transform[] = {1, 0, 0, 1, 0, 0};
    if (row.variant == 2) {
      transform[0] = transform[3] = 1.25f;
      transform[4] = transform[5] = .25f;
    }
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13060,
                                 reinterpret_cast<const Byte *>(foreground),
                                 sizeof(foreground))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13080,
                                 reinterpret_cast<const Byte *>(background),
                                 sizeof(background))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130a0,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x130c0,
                                 reinterpret_cast<const Byte *>(properties),
                                 sizeof(properties))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13100,
                                 reinterpret_cast<const Byte *>(transform),
                                 sizeof(transform))
                    .is_ok());
    h.args({target, 0x13060, 0x130c0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    const auto brush = h.pointer(0x12020);
    h.args({target, 0x13100});
    h.manager.target_method(h.marshaler, target_SetTransform);
    if (row.variant == 2) {
      h.args({target, 0, 0});
      h.floats(120, 144);
      h.manager.target_method(h.marshaler, target_SetDpi);
    }
    h.args({target, row.aa});
    h.manager.target_method(h.marshaler, target_SetAntialiasMode);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({target, 0x13080});
    h.manager.target_method(h.marshaler, target_Clear);
    if (row.variant == 3) {
      const float clip[] = {0, 0, 2, 2};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13140,
                                   reinterpret_cast<const Byte *>(clip),
                                   sizeof(clip))
                      .is_ok());
      h.args({target, 0x13140, D2D1_ANTIALIAS_MODE_ALIASED});
      h.manager.target_method(h.marshaler, target_PushAxisAlignedClip);
    }
    h.args({target, 0x130a0, brush});
    h.manager.target_method(h.marshaler, target_FillRectangle);
    if (row.variant == 3) {
      h.args({target});
      h.manager.target_method(h.marshaler, target_PopAxisAlignedClip);
    }
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    if (row.aa == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE &&
        (row.variant == 1 || row.variant == 2)) {
      EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
      continue;
    }
    ASSERT_EQ(h.cpu.gpr.rax, 0u);
    EXPECT_TRUE(h.gdi.with_device_surface(dc, [&](auto &surface) {
      for (usize i = 0; i < 16; ++i) {
        const auto *p = surface.rgba + i * 4;
        const u32 value =
            (u32(p[3]) << 24) | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
        EXPECT_EQ(value, row.pixels[i]) << "pixel " << i;
      }
    }));
  }
}
#include "d2d1_dc_initial_oracle.inc"
TEST(D2D1RenderTarget,
     FirstRepeatedAndReboundBatchesSeedFromCurrentDcForCpuAndGpu) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  for (const auto &row : dc_initial_cases) {
    if (row.hardware && !backend.available())
      continue;
    SCOPED_TRACE(::testing::Message()
                 << row.hardware << " wide=" << row.wide << " seed=" << row.seed
                 << " alpha=" << row.alpha << " kind=" << row.kind);
    Harness h(row.wide);
    if (row.hardware)
      ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create(),
               target =
                   h.target(factory, row.alpha,
                            row.hardware ? D2D1_RENDER_TARGET_TYPE_HARDWARE
                                         : D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    ASSERT_NE(target, 0u);
    std::array<u8, 64> empty{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(4, 4, empty),
               dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const auto seed_dc = [&](u32 argb) {
      ASSERT_TRUE(h.gdi.with_device_surface(dc, [&](auto &surface) {
        for (usize i = 0; i < 16; ++i) {
          auto *p = surface.rgba + i * 4;
          p[0] = static_cast<u8>(argb >> 16);
          p[1] = static_cast<u8>(argb >> 8);
          p[2] = static_cast<u8>(argb);
          p[3] = static_cast<u8>(argb >> 24);
        }
      }));
    };
    if (row.kind == 2) {
      const i32 small[] = {0, 0, 2, 2};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13200,
                                   reinterpret_cast<const Byte *>(small),
                                   sizeof(small))
                      .is_ok());
      h.args({target, dc, 0x13200});
      h.manager.target_method(h.marshaler, target_BindDC);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    }
    seed_dc(row.seed);
    const i32 bound[] = {0, 0, 4, 4};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13200, reinterpret_cast<const Byte *>(bound),
                                 sizeof(bound))
                    .is_ok());
    h.args({target, dc, 0x13200});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    if (row.kind == 5)
      seed_dc(0xff2468ac);
    const float color[] = {0, 0, 1, row.kind >= 3 ? .5f : 1.f},
                rectangle[] = {1, 1, 3, 3};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13220, reinterpret_cast<const Byte *>(color),
                                 sizeof(color))
                    .is_ok());
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13240,
                                 reinterpret_cast<const Byte *>(rectangle),
                                 sizeof(rectangle))
                    .is_ok());
    h.args({target, 0x13220, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    const auto brush = h.pointer(0x12020);
    const auto batch = [&] {
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      if (row.kind) {
        h.args({target, 0x13240, brush});
        h.manager.target_method(h.marshaler, target_FillRectangle);
      }
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    };
    batch();
    if (row.kind == 4 || row.kind == 6) {
      if (row.kind == 6)
        seed_dc(0xff2468ac);
      batch();
    }
    EXPECT_TRUE(h.gdi.with_device_surface(dc, [&](auto &surface) {
      for (usize i = 0; i < 16; ++i) {
        const auto *p = surface.rgba + i * 4;
        const u32 argb =
            (u32(p[3]) << 24) | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
        EXPECT_EQ(argb, row.pixels[i]) << "pixel " << i;
      }
    }));
  }
}
TEST(D2D1Hardware,
     DefaultSelectsAvailableGpuWhileSoftwareAndUncertifiedMinimumStayTruthful) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal device unavailable";
  for (bool wide : {false, true}) {
    Harness h(wide);
    ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create();
    for (const auto requested :
         {D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1_RENDER_TARGET_TYPE_SOFTWARE,
          D2D1_RENDER_TARGET_TYPE_HARDWARE}) {
      const auto target = h.target(factory, D2D1_ALPHA_MODE_IGNORE, requested);
      ASSERT_NE(target, 0u);
      const u32 hardware_query[] = {D2D1_RENDER_TARGET_TYPE_HARDWARE,
                                    DXGI_FORMAT_B8G8R8A8_UNORM,
                                    D2D1_ALPHA_MODE_IGNORE,
                                    0,
                                    0,
                                    0,
                                    0};
      ASSERT_TRUE(
          h.memory
              .write_bytes(0x13300,
                           reinterpret_cast<const Byte *>(hardware_query),
                           sizeof(hardware_query))
              .is_ok());
      h.args({target, 0x13300});
      h.manager.target_method(h.marshaler, target_IsSupported);
      EXPECT_EQ(h.cpu.gpr.rax,
                requested == D2D1_RENDER_TARGET_TYPE_SOFTWARE ? 0u : 1u);
      h.args({target});
      h.manager.target_method(h.marshaler, target_Release);
    }
    const u32 minimum[] = {D2D1_RENDER_TARGET_TYPE_HARDWARE,
                           DXGI_FORMAT_B8G8R8A8_UNORM,
                           D2D1_ALPHA_MODE_IGNORE,
                           0,
                           0,
                           0,
                           D3D_FEATURE_LEVEL_9_1};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13000,
                                 reinterpret_cast<const Byte *>(minimum),
                                 sizeof(minimum))
                    .is_ok());
    h.args({factory, 0x13000, 0x12010});
    h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(h.pointer(0x12010), 0u);
    ASSERT_TRUE(
        h.memory.write_u32(0x13000, D2D1_RENDER_TARGET_TYPE_DEFAULT).is_ok());
    h.args({factory, 0x13000, 0x12010});
    h.manager.factory_method(h.marshaler, factory_CreateDCRenderTarget);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    const auto fallback = h.pointer(0x12010);
    const u32 software_query[] = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                  DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE,
                                  0,
                                  0,
                                  0,
                                  0};
    ASSERT_TRUE(h.memory
                    .write_bytes(0x13300,
                                 reinterpret_cast<const Byte *>(software_query),
                                 sizeof(software_query))
                    .is_ok());
    h.args({fallback, 0x13300});
    h.manager.target_method(h.marshaler, target_IsSupported);
    EXPECT_EQ(h.cpu.gpr.rax, 1u);
  }
}
#include "d2d1_empty_batch_oracle.inc"
TEST(D2D1RenderTarget, BindAndEmptyBatchDoNotWriteBackDibPixelsOrPadding) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  for (bool hardware : {false, true})
    for (bool wide : {false, true}) {
      if (hardware && !backend.available())
        continue;
      SCOPED_TRACE(::testing::Message() << hardware << " wide=" << wide);
      Harness h(wide);
      ASSERT_TRUE(h.memory
                      .map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
                                  GdiManager::GDI_DIB_SCRATCH_SIZE,
                                  MemoryProtection::ReadWrite)
                      .is_ok());
      h.gdi.set_guest_memory(&h.memory);
      if (hardware)
        ASSERT_TRUE(h.manager.set_backend(&backend));
      const auto factory = h.create(),
                 target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                   hardware ? D2D1_RENDER_TARGET_TYPE_HARDWARE
                                            : D2D1_RENDER_TARGET_TYPE_SOFTWARE);
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13500,
                                   reinterpret_cast<const Byte *>(
                                       empty_dib_header.data()),
                                   empty_dib_header.size())
                      .is_ok());
      const auto bitmap =
          h.gdi.create_dib_section(0, 0x13500, 0, 0x12040, wide);
      ASSERT_NE(bitmap, 0u);
      const auto bits = h.pointer(0x12040);
      ASSERT_NE(bits, 0u);
      const auto dc = h.gdi.create_compatible_dc(0);
      ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
      ASSERT_TRUE(h.memory.write_u32(bits, empty_dib_sentinel).is_ok());
      const i32 bound[] = {0, 0, 1, 1};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13200,
                                   reinterpret_cast<const Byte *>(bound),
                                   sizeof(bound))
                      .is_ok());
      h.args({target, dc, 0x13200});
      h.manager.target_method(h.marshaler, target_BindDC);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      EXPECT_EQ(h.memory.read_u32(bits).value(), empty_dib_sentinel);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      EXPECT_EQ(h.memory.read_u32(bits).value(), empty_dib_sentinel);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      EXPECT_EQ(h.memory.read_u32(bits).value(), empty_dib_sentinel);
    }
}
TEST(D2D1RenderTarget,
     ReadonlyBindAndEmptyBatchDoNotScheduleWindowPresentation) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  for (bool hardware : {false, true})
    for (bool wide : {false, true}) {
      if (hardware && !backend.available())
        continue;
      SCOPED_TRACE(::testing::Message() << hardware << " wide=" << wide);
      Harness h(wide);
      if (hardware)
        ASSERT_TRUE(h.manager.set_backend(&backend));
      const auto factory = h.create(),
                 target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                   hardware ? D2D1_RENDER_TARGET_TYPE_HARDWARE
                                            : D2D1_RENDER_TARGET_TYPE_SOFTWARE);
      constexpr Handle window = 0x1234;
      const auto dc = h.gdi.get_dc(window);
      h.gdi.flush_window(window); // consume the model redirection surface's
                                  // initial dirty state
      unsigned scheduled = 0;
      std::vector<std::function<void()>> work;
      h.gdi.set_presentation_scheduler([&](std::function<void()> callback) {
        ++scheduled;
        work.push_back(std::move(callback));
      });
      ASSERT_EQ(scheduled, 0u);
      const i32 rectangle[] = {0, 0, 4, 4};
      ASSERT_TRUE(h.memory
                      .write_bytes(0x13200,
                                   reinterpret_cast<const Byte *>(rectangle),
                                   sizeof(rectangle))
                      .is_ok());
      h.args({target, dc, 0x13200});
      h.manager.target_method(h.marshaler, target_BindDC);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      EXPECT_EQ(scheduled, 0u);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      EXPECT_EQ(scheduled, 0u);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      EXPECT_EQ(scheduled, 0u);
      // Positive control: a completed real draw must still invalidate/present.
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({target, 0});
      h.manager.target_method(h.marshaler, target_Clear);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      EXPECT_EQ(scheduled, 1u);
      h.gdi.stop_presentation();
      work.clear();
    }
}

TEST(D2D1Bitmap, CopiesArbitraryWicSourceAndKeepsFactoryAliveInBothWidths) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(),
               target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                 D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    unsigned callbacks = 0;
    h.manager.set_guest_interface_call([&](Marshaler &m, GuestAddr source,
                                           u32 slot, const u64 *args,
                                           usize count) -> u32 {
      EXPECT_EQ(source, 0x19000u);
      ++callbacks;
      if (slot == wic_GetPixelFormat) {
        EXPECT_EQ(count, 2u);
        (void)h.memory.write_bytes(args[1],
                                   GUID_WICPixelFormat32bppPBGRA.data(), 16);
        return S_OK;
      }
      if (slot == wic_GetSize) {
        EXPECT_EQ(count, 3u);
        (void)m.write_u32(args[1], 2);
        (void)m.write_u32(args[2], 2);
        return S_OK;
      }
      if (slot == wic_CopyPixels) {
        EXPECT_EQ(count, 5u);
        EXPECT_EQ(args[2], 8u);
        EXPECT_EQ(args[3], 16u);
        const u8 pixels[] = {0,   0, 255, 255, 0,   255, 0,   255,
                             255, 0, 0,   255, 128, 128, 128, 128};
        (void)h.memory.write_bytes(args[4], pixels, 16);
        return S_OK;
      }
      ADD_FAILURE() << slot;
      return E_NOTIMPL;
    });
    h.args({target, 0x19000, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    EXPECT_EQ(callbacks, 3u);
    const auto bitmap = h.pointer(0x12020);
    ASSERT_NE(bitmap, 0u);
    h.args({bitmap, 0x15000});
    h.manager.bitmap_method(h.marshaler, bitmap_GetPixelFormat);
    EXPECT_EQ(h.memory.read_u32(0x15000).value(), DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(h.memory.read_u32(0x15004).value(),
              D2D1_ALPHA_MODE_PREMULTIPLIED);
    h.args({factory});
    h.manager.factory_method(h.marshaler, 2);
    h.args({target});
    h.manager.target_method(h.marshaler, 2);
    h.args({bitmap, 0x12030});
    h.manager.bitmap_method(h.marshaler, bitmap_GetFactory);
    EXPECT_EQ(h.pointer(0x12030), factory);
    h.args({bitmap, 0x15000});
    h.manager.bitmap_method(h.marshaler, bitmap_GetPixelSize);
    EXPECT_EQ(h.memory.read_u32(0x15000).value(), 2u);
    EXPECT_EQ(h.memory.read_u32(0x15004).value(), 2u);
    h.args({bitmap});
    h.manager.bitmap_method(h.marshaler, 2);
    h.args({factory});
    h.manager.factory_method(h.marshaler, 2);
    EXPECT_TRUE(h.memory.read_u32(bitmap).is_err());
  }
}
namespace {
#include "d2d1_bitmap_draw_pixels.inc"
}
TEST(D2D1Bitmap,
     RealHardwareSamplesCopiedWicPixelsAndRetainsTextureThroughCompletion) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal unavailable";
  for (bool wide : {false, true}) {
    Harness h(wide);
    ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create(),
               target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                 D2D1_RENDER_TARGET_TYPE_HARDWARE);
    h.manager.set_guest_interface_call([&](Marshaler &m, GuestAddr, u32 slot,
                                           const u64 *a, usize) -> u32 {
      if (slot == wic_GetPixelFormat) {
        (void)h.memory.write_bytes(a[1], GUID_WICPixelFormat32bppPBGRA.data(),
                                   16);
        return S_OK;
      }
      if (slot == wic_GetSize) {
        (void)m.write_u32(a[1], 2);
        (void)m.write_u32(a[2], 2);
        return S_OK;
      }
      if (slot == wic_CopyPixels) {
        const u8 bytes[] = {0,   0, 255, 255, 0,   255, 0,   255,
                            255, 0, 0,   255, 128, 128, 128, 128};
        (void)h.memory.write_bytes(a[4], bytes, sizeof(bytes));
        return S_OK;
      }
      return E_NOTIMPL;
    });
    h.args({target, 0x19000, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    const auto bitmap = h.pointer(0x12020);
    h.manager.set_guest_interface_call({});
    std::array<u8, 64> seed{};
    const auto dib = h.gdi.create_bitmap_from_rgba(4, 4, seed),
               dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, dib), 0u);
    const i32 bound[] = {0, 0, 4, 4};
    (void)h.memory.write_bytes(0x14000, reinterpret_cast<const u8 *>(bound),
                               sizeof(bound));
    h.args({target, dc, 0x14000});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    for (const auto &expected : bitmap_draw_oracle) {
      if (expected.wide != wide)
        continue;
      const float red[] = {1, 0, 0, 1}, blue[] = {0, 0, 1, 1},
                  full[] = {0, 0, 4, 4}, crop_dest[] = {1, 1, 3, 3},
                  crop_src[] = {1, 0, 2, 1};
      (void)h.memory.write_bytes(
          0x14020, reinterpret_cast<const u8 *>(expected.stage ? blue : red),
          16);
      (void)h.memory.write_bytes(
          0x14040,
          reinterpret_cast<const u8 *>(expected.stage ? crop_dest : full), 16);
      (void)h.memory.write_bytes(0x14060,
                                 reinterpret_cast<const u8 *>(crop_src), 16);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({target, 0x14020});
      h.manager.target_method(h.marshaler, target_Clear);
      const float opacity = expected.stage ? .5f : 1.f;
      u32 opacity_bits;
      std::memcpy(&opacity_bits, &opacity, 4);
      h.args({target, bitmap, 0x14040, opacity_bits,
              D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
              expected.stage ? 0x14060u : 0u});
      if (wide)
        h.cpu.sse.xmm[3].u32_[0] = opacity_bits;
      h.manager.target_method(h.marshaler, target_DrawBitmap);
      if (expected.stage) {
        h.args({bitmap});
        h.manager.bitmap_method(h.marshaler, 2);
        EXPECT_TRUE(h.memory.read_u32(bitmap).is_err());
      }
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      std::array<u32, 16> actual{};
      ASSERT_TRUE(h.gdi.read_device_surface(
          dc, [&](const GdiManager::ReadOnlyDeviceSurface &surface) {
            for (u32 y = 0; y < 4; ++y)
              for (u32 x = 0; x < 4; ++x) {
                const auto *p = surface.rgba + (y * surface.width + x) * 4;
                actual[y * 4 + x] = (u32(p[3]) << 24) | (u32(p[0]) << 16) |
                                    (u32(p[1]) << 8) | p[2];
              }
          }));
      EXPECT_EQ(actual, expected.pixels) << expected.stage;
    }
  }
}
namespace {
#include "d2d1_wic_bitmap_cases.inc"
#include "d2d1_wic_bitmap_pixels.inc"
struct ArbitraryWicSource {
  Harness &h;
  std::array<u8, 16> format = d2d1_contract::GUID_WICPixelFormat32bppPBGRA;
  u32 width = 2, height = 2, fail = 0;
  std::array<u32, 4> calls{};
  ArbitraryWicSource(Harness &harness) : h(harness) {
    h.manager.set_guest_interface_call([this](Marshaler &m, GuestAddr source,
                                              u32 slot, const u64 *a,
                                              usize) -> u32 {
      using namespace d2d1_contract;
      EXPECT_EQ(source, 0x19000u);
      if (slot == wic_GetPixelFormat) {
        ++calls[1];
        if (fail == 2)
          return 0x80070005u;
        (void)h.memory.write_bytes(a[1], format.data(), format.size());
        return S_OK;
      }
      if (slot == wic_GetSize) {
        ++calls[0];
        if (fail == 1)
          return 0x80070005u;
        (void)m.write_u32(a[1], width);
        (void)m.write_u32(a[2], height);
        return S_OK;
      }
      if (slot == wic_CopyPixels) {
        ++calls[2];
        if (fail == 3)
          return 0x80070005u;
        const u32 bpp = format == GUID_WICPixelFormat24bppBGR   ? 3
                        : format == GUID_WICPixelFormat8bppGray ? 1
                                                                : 4;
        const u32 stride = a[2];
        if (stride < width * bpp)
          return E_INVALIDARG;
        for (u32 y = 0; y < height; ++y)
          for (u32 x = 0; x < width; ++x) {
            const u8 p[4] = {u8(x ? 32 : 0), u8(y ? 64 : 0), 128, 128};
            (void)h.memory.write_bytes(a[4] + y * stride + x * bpp, p, bpp);
          }
        return S_OK;
      }
      if (slot == wic_GetResolution) {
        ++calls[3];
        return E_NOTIMPL;
      }
      ADD_FAILURE() << slot;
      return E_NOTIMPL;
    });
  }
};
} // namespace
TEST(D2D1Bitmap, NativeWindowsCreationAndValidationMatrixBothWidths) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal unavailable";
  const std::array<std::array<u8, 16>, 9> formats = {
      GUID_WICPixelFormat32bppPBGRA, GUID_WICPixelFormat32bppBGRA,
      GUID_WICPixelFormat32bppPRGBA, GUID_WICPixelFormat32bppRGBA,
      GUID_WICPixelFormat32bppBGR,   GUID_WICPixelFormat32bppRGB,
      GUID_WICPixelFormat24bppBGR,   GUID_WICPixelFormat8bppGray,
      std::array<u8, 16>{}};
  for (bool wide : {false, true}) {
    Harness h(wide);
    ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create();
    const GuestAddr targets[] = {0,
                                 h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                          D2D1_RENDER_TARGET_TYPE_SOFTWARE),
                                 h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                          D2D1_RENDER_TARGET_TYPE_HARDWARE)};
    ArbitraryWicSource source(h);
    for (const auto &c : wic_bitmap_cases) {
      if (c.wide != wide)
        continue;
      SCOPED_TRACE(::testing::Message()
                   << wide << "," << c.target << "," << c.source << ","
                   << c.prop << "," << c.variant);
      source.calls = {};
      source.fail = 0;
      source.width = source.height = 2;
      source.format = formats[c.source];
      u32 format = 0, alpha = 0;
      float dx = 0, dy = 0;
      GuestAddr source_ptr = 0x19000, properties = 0x14000, out = 0x12020;
      if (!c.variant) {
        if (c.prop == 0)
          properties = 0;
        if (c.prop >= 2 && c.prop <= 5)
          format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (c.prop == 3)
          alpha = D2D1_ALPHA_MODE_PREMULTIPLIED;
        if (c.prop == 4 || c.prop == 7)
          alpha = D2D1_ALPHA_MODE_IGNORE;
        if (c.prop == 5)
          alpha = D2D1_ALPHA_MODE_STRAIGHT;
        if (c.prop == 6)
          format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (c.prop == 8) {
          format = 999;
          alpha = D2D1_ALPHA_MODE_PREMULTIPLIED;
        }
        if (c.prop == 9) {
          format = DXGI_FORMAT_B8G8R8A8_UNORM;
          alpha = 999;
        }
      } else {
        const u32 variant = c.variant - 1;
        if (variant == 1)
          dx = 120;
        if (variant == 2)
          dy = 144;
        if (variant == 3) {
          dx = 120;
          dy = 144;
        }
        if (variant == 4) {
          dx = -1;
          dy = 96;
        }
        if (variant == 5) {
          dx = NAN;
          dy = 96;
        }
        if (variant == 6) {
          dx = INFINITY;
          dy = 96;
        }
        if (variant == 7)
          source.width = 0;
        if (variant == 8)
          source.height = 0;
        if (variant >= 9 && variant <= 11)
          source.fail = variant - 8;
        if (variant == 12 || variant == 18)
          source_ptr = 0;
        if (variant == 13 || variant == 18)
          out = 0;
        if (variant == 15)
          properties = 1;
        if (variant == 16)
          source_ptr = 1;
        if (variant == 17)
          out = 1;
      }
      std::array<u8, bitmap_layout_properties> p{};
      std::memcpy(p.data() + bitmap_layout_format, &format, 4);
      std::memcpy(p.data() + bitmap_layout_alpha, &alpha, 4);
      std::memcpy(p.data() + bitmap_layout_dpiX, &dx, 4);
      std::memcpy(p.data() + bitmap_layout_dpiY, &dy, 4);
      (void)h.memory.write_bytes(0x14000, p.data(), p.size());
      if (wide)
        (void)h.memory.write_u64(0x12020, 0x12345678);
      else
        (void)h.memory.write_u32(0x12020, 0x12345678);
      h.args({targets[c.target], source_ptr, properties, out});
      h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
      EXPECT_EQ(h.cpu.pending_raise.active, c.fault);
      EXPECT_EQ(source.calls, c.calls);
      if (!c.fault)
        EXPECT_EQ(h.cpu.gpr.rax, c.hr);
      const auto object = h.pointer(0x12020);
      EXPECT_EQ(object == 0x12345678 ? 0u : object ? 2u : 1u, c.out);
      if (!c.fault && c.hr == S_OK) {
        h.args({object});
        h.manager.bitmap_method(h.marshaler, 2);
      }
    }
  }
}
TEST(D2D1Bitmap, NativeWindowsSoftwareAndHardwareSamplingPixelsBothWidths) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal unavailable";
  for (bool wide : {false, true}) {
    Harness h(wide);
    ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create();
    const GuestAddr targets[2][3] = {
        {0,
         h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED,
                  D2D1_RENDER_TARGET_TYPE_SOFTWARE),
         h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED,
                  D2D1_RENDER_TARGET_TYPE_HARDWARE)},
        {0,
         h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                  D2D1_RENDER_TARGET_TYPE_SOFTWARE),
         h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                  D2D1_RENDER_TARGET_TYPE_HARDWARE)}};
    ArbitraryWicSource source(h);
    std::array<u8, 64> seed{};
    const auto dib = h.gdi.create_bitmap_from_rgba(4, 4, seed),
               dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, dib), 0u);
    const i32 bounds[] = {0, 0, 4, 4};
    (void)h.memory.write_bytes(0x14080, reinterpret_cast<const u8 *>(bounds),
                               sizeof(bounds));
    for (u32 alpha : {0u, 1u})
      for (u32 type : {1u, 2u}) {
        h.args({targets[alpha][type], dc, 0x14080});
        h.manager.target_method(h.marshaler, target_BindDC);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      }
    for (const auto &c : wic_bitmap_pixel_cases) {
      if (c.wide != wide)
        continue;
      SCOPED_TRACE(::testing::Message()
                   << wide << "," << c.target << "," << c.format << ","
                   << c.alpha << "," << c.interpolation << "," << c.opacity
                   << "," << c.variant);
      source.format = c.format ? GUID_WICPixelFormat32bppPRGBA
                               : GUID_WICPixelFormat32bppPBGRA;
      const u32 p[] = {0, c.alpha, 0, 0};
      (void)h.memory.write_bytes(0x14000, reinterpret_cast<const u8 *>(p),
                                 sizeof(p));
      h.args(
          {targets[c.target_alpha == D2D1_ALPHA_MODE_IGNORE ? 1 : 0][c.target],
           0x19000, 0x14000, 0x12020});
      h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      const auto bitmap = h.pointer(0x12020);
      const float clear[] = {.1f, .2f, .3f, .5f}, full[] = {0, 0, 4, 4},
                  dest[] = {1, 1, 3, 3}, crop[] = {1, 0, 2, 1};
      (void)h.memory.write_bytes(0x14100, reinterpret_cast<const u8 *>(clear),
                                 16);
      (void)h.memory.write_bytes(
          0x14120, reinterpret_cast<const u8 *>(c.variant ? dest : full), 16);
      (void)h.memory.write_bytes(0x14140, reinterpret_cast<const u8 *>(crop),
                                 16);
      (void)h.memory.write_bytes(0x14160, reinterpret_cast<const u8 *>(dest),
                                 16);
      const auto target =
          targets[c.target_alpha == D2D1_ALPHA_MODE_IGNORE ? 1 : 0][c.target];
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({target, 0x14100});
      h.manager.target_method(h.marshaler, target_Clear);
      if (c.variant == 2) {
        h.args({target, 0x14160, D2D1_ANTIALIAS_MODE_ALIASED});
        h.manager.target_method(h.marshaler, target_PushAxisAlignedClip);
      }
      const float opacities[] = {0, .25f, .5f, .625f, 1};
      u32 opacity;
      std::memcpy(&opacity, &opacities[c.opacity], 4);
      h.args({target, bitmap, 0x14120, opacity, c.interpolation,
              c.variant == 1 ? 0x14140u : 0u});
      if (wide)
        h.cpu.sse.xmm[3].u32_[0] = opacity;
      h.manager.target_method(h.marshaler, target_DrawBitmap);
      if (c.variant == 2) {
        h.args({target});
        h.manager.target_method(h.marshaler, target_PopAxisAlignedClip);
      }
      h.args({bitmap});
      h.manager.bitmap_method(h.marshaler, 2);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      std::array<u32, 16> actual{};
      ASSERT_TRUE(h.gdi.read_device_surface(
          dc, [&](const GdiManager::ReadOnlyDeviceSurface &s) {
            for (u32 i = 0; i < 16; ++i) {
              const auto *p = s.rgba + i * 4;
              actual[i] = (u32(p[3]) << 24) | (u32(p[0]) << 16) |
                          (u32(p[1]) << 8) | p[2];
            }
          }));
      EXPECT_EQ(actual, c.pixels);
    }
  }
}
namespace {
#include "d2d1_bitmap_domains.inc"
}
TEST(D2D1Bitmap, WindowsResourceDomainsAndDeferredErrorTagsBothWidths) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal unavailable";
  for (const auto &c : bitmap_domains) {
    SCOPED_TRACE(::testing::Message() << c.wide << "," << c.source << ","
                                      << c.target << "," << c.foreign);
    Harness h(c.wide);
    ASSERT_TRUE(h.manager.set_backend(&backend));
    const auto factory = h.create(), other = c.foreign ? h.create() : factory;
    const auto a = h.target(factory, D2D1_ALPHA_MODE_IGNORE, c.source),
               b = h.target(other, D2D1_ALPHA_MODE_IGNORE, c.target);
    ArbitraryWicSource source(h);
    h.args({a, 0x19000, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    const auto bitmap = h.pointer(0x12020);
    std::array<u8, 64> initial{};
    const auto dib = h.gdi.create_bitmap_from_rgba(4, 4, initial),
               dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, dib), 0u);
    const i32 bounds[] = {0, 0, 4, 4};
    const float blue[] = {0, 0, 1, 1}, dest[] = {0, 0, 4, 4};
    (void)h.memory.write_bytes(0x14000, reinterpret_cast<const u8 *>(bounds),
                               16);
    (void)h.memory.write_bytes(0x14020, reinterpret_cast<const u8 *>(blue), 16);
    (void)h.memory.write_bytes(0x14040, reinterpret_cast<const u8 *>(dest), 16);
    h.args({b, dc, 0x14000});
    h.manager.target_method(h.marshaler, target_BindDC);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    h.args({b});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    h.args({b, 0x14020});
    h.manager.target_method(h.marshaler, target_Clear);
    if (c.wide)
      h.args({b, 17, 29});
    else
      h.args({b, 17, 0, 29, 0});
    h.manager.target_method(h.marshaler, target_SetTags);
    h.args({b, bitmap, 0x14040, 0x3f800000,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, 0});
    if (c.wide)
      h.cpu.sse.xmm[3].u32_[0] = 0x3f800000;
    h.manager.target_method(h.marshaler, target_DrawBitmap);
    h.args({b, 0x14500, 0x14508});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, c.result);
    EXPECT_EQ(h.memory.read_u64(0x14500).value(), c.tag1);
    EXPECT_EQ(h.memory.read_u64(0x14508).value(), c.tag2);
    u32 pixel = 0;
    ASSERT_TRUE(h.gdi.read_device_surface(
        dc, [&](const GdiManager::ReadOnlyDeviceSurface &s) {
          const auto *p = s.rgba;
          pixel =
              (u32(p[3]) << 24) | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
        }));
    EXPECT_EQ(pixel, c.pixel);
  }
}
TEST(D2D1Bitmap,
     GuestWicCallbacksMayReenterAndInvalidatedTargetsCannotPublishBitmaps) {
  using namespace d2d1_contract;
  for (bool wide : {false, true})
    for (bool destroy : {false, true}) {
      Harness h(wide);
      const auto factory = h.create(),
                 target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                   D2D1_RENDER_TARGET_TYPE_SOFTWARE);
      bool queried = false;
      h.manager.set_guest_interface_call([&](Marshaler &m, GuestAddr, u32 slot,
                                             const u64 *a, usize) -> u32 {
        if (slot == wic_GetPixelFormat) {
          h.args({target, 0x15000, 0x15004});
          h.manager.target_method(h.marshaler, target_GetDpi);
          const auto raw = h.memory.read_u32(0x15000).value();
          float dpi;
          std::memcpy(&dpi, &raw, 4);
          EXPECT_FLOAT_EQ(dpi, 96.f);
          queried = true;
          if (destroy) {
            h.args({target});
            h.manager.target_method(h.marshaler, 2);
          }
          (void)h.memory.write_bytes(a[1], GUID_WICPixelFormat32bppPBGRA.data(),
                                     16);
          return S_OK;
        }
        if (slot == wic_GetSize) {
          (void)m.write_u32(a[1], 2);
          (void)m.write_u32(a[2], 2);
          return S_OK;
        }
        if (slot == wic_CopyPixels) {
          const u8 bytes[16] = {};
          (void)h.memory.write_bytes(a[4], bytes, sizeof(bytes));
          return S_OK;
        }
        return E_NOTIMPL;
      });
      h.args({target, 0x19000, 0, 0x12020});
      h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
      EXPECT_TRUE(queried);
      EXPECT_EQ(h.cpu.gpr.rax, destroy ? E_NOTIMPL : S_OK);
      EXPECT_EQ(h.pointer(0x12020) == 0, destroy);
    }
}
TEST(D2D1Bitmap, A8AndBitmapMutationInterfacesFailExplicitly) {
  using namespace d2d1_contract;
  for (bool wide : {false, true}) {
    Harness h(wide);
    const auto factory = h.create(),
               target = h.target(factory, D2D1_ALPHA_MODE_IGNORE,
                                 D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    ArbitraryWicSource source(h);
    source.format = GUID_WICPixelFormat8bppAlpha;
    h.args({target, 0x19000, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(h.pointer(0x12020), 0u);
    source.format = GUID_WICPixelFormat32bppPBGRA;
    h.args({target, 0x19000, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateBitmapFromWicBitmap);
    ASSERT_EQ(h.cpu.gpr.rax, S_OK);
    const auto bitmap = h.pointer(0x12020);
    for (usize slot : {bitmap_CopyFromBitmap, bitmap_CopyFromRenderTarget,
                       bitmap_CopyFromMemory}) {
      h.args({bitmap, 0, 0, 0});
      h.manager.bitmap_method(h.marshaler, slot);
      EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    }
  }
}

// SDK/Learn-only validation, per user request: no Windows comparison matrix.
TEST(D2D1PathGeometry, RealSinkRecordsAllSegmentKindsAndRetainsFactoryBothWidths) {
 using namespace d2d1_contract;
 for(bool wide:{false,true}){
  Harness h(wide);h.args({0,0x11000,0,0x12000});h.manager.create_factory(h.marshaler);ASSERT_EQ(h.cpu.gpr.rax,S_OK);const auto factory=h.pointer(0x12000);
  h.args({factory,0x12000});h.manager.factory_method(h.marshaler,factory_CreatePathGeometry);ASSERT_EQ(h.cpu.gpr.rax,S_OK);const auto path=h.pointer(0x12000);ASSERT_NE(path,0u);
  h.args({path,0x12000});h.manager.geometry_method(h.marshaler,path_Open,false);ASSERT_EQ(h.cpu.gpr.rax,S_OK);const auto sink=h.pointer(0x12000);ASSERT_NE(sink,0u);
  h.args({path,0x12000});h.manager.geometry_method(h.marshaler,path_Open,false);EXPECT_EQ(h.cpu.gpr.rax,D2DERR_WRONG_STATE);EXPECT_EQ(h.pointer(0x12000),0u);
  h.args({factory});h.manager.factory_method(h.marshaler,factory_Release);EXPECT_EQ(h.cpu.gpr.rax,2u);
  if(wide)h.args({sink,0,D2D1_FIGURE_BEGIN_FILLED});else h.args({sink,0,0,D2D1_FIGURE_BEGIN_FILLED});h.manager.geometry_method(h.marshaler,sink_BeginFigure,true);
  if(wide)h.args({sink,0x3f800000});else h.args({sink,0x3f800000,0});h.manager.geometry_method(h.marshaler,sink_AddLine,true);
  for(auto slot:{sink_AddBezier,sink_AddQuadraticBezier,sink_AddArc}){h.args({sink,0x13000});h.manager.geometry_method(h.marshaler,slot,true);}
  h.args({sink,0x13000,2});h.manager.geometry_method(h.marshaler,sink_AddLines,true);
  h.args({sink,D2D1_FIGURE_END_OPEN});h.manager.geometry_method(h.marshaler,sink_EndFigure,true);
  h.args({sink});h.manager.geometry_method(h.marshaler,sink_Close,true);EXPECT_EQ(h.cpu.gpr.rax,S_OK);
  h.args({path,0x12000});h.manager.geometry_method(h.marshaler,path_GetFigureCount,false);EXPECT_EQ(h.cpu.gpr.rax,S_OK);EXPECT_EQ(h.memory.read_u32(0x12000).value(),1u);
  h.args({path,0x12000});h.manager.geometry_method(h.marshaler,path_GetSegmentCount,false);EXPECT_EQ(h.cpu.gpr.rax,S_OK);EXPECT_EQ(h.memory.read_u32(0x12000).value(),6u);
  // Geometric processing must not return a plausible empty result.
  h.args({path,0,0x12000});h.manager.geometry_method(h.marshaler,path_GetBounds,false);EXPECT_EQ(h.cpu.gpr.rax,E_NOTIMPL);
  h.args({path});h.manager.geometry_method(h.marshaler,path_Release,false);EXPECT_EQ(h.cpu.gpr.rax,0u);
  h.args({sink});h.manager.geometry_method(h.marshaler,sink_Release,true);EXPECT_EQ(h.cpu.gpr.rax,0u);
 }
}
TEST(D2D1PathGeometry, UnbalancedFigureAndLateFillModeFailAtCloseBothWidths) {
 using namespace d2d1_contract;
 for(bool wide:{false,true})for(bool end:{false,true}){
  Harness h(wide);h.args({0,0x11000,0,0x12000});h.manager.create_factory(h.marshaler);const auto factory=h.pointer(0x12000);
  h.args({factory,0x12000});h.manager.factory_method(h.marshaler,factory_CreatePathGeometry);const auto path=h.pointer(0x12000);
  h.args({path,0x12000});h.manager.geometry_method(h.marshaler,path_Open,false);const auto sink=h.pointer(0x12000);
  if(wide)h.args({sink,0,D2D1_FIGURE_BEGIN_FILLED});else h.args({sink,0,0,D2D1_FIGURE_BEGIN_FILLED});h.manager.geometry_method(h.marshaler,sink_BeginFigure,true);
  if(end){h.args({sink,D2D1_FIGURE_END_OPEN});h.manager.geometry_method(h.marshaler,sink_EndFigure,true);h.args({sink,D2D1_FILL_MODE_WINDING});h.manager.geometry_method(h.marshaler,sink_SetFillMode,true);}
  h.args({sink});h.manager.geometry_method(h.marshaler,sink_Close,true);EXPECT_EQ(h.cpu.gpr.rax,D2DERR_WRONG_STATE);
 }
}

TEST(D2D1GeometryDrawing, ConcavityHolesWindingStrokeAndTransformsBothWidths) {
  using namespace d2d1_contract;
  auto &backend = graphics::MetalDevice::instance();
  if (!backend.available())
    GTEST_SKIP() << "Metal required for local GPU acceptance";
  for (bool hardware : {false, true})
    for (bool wide : {false, true})
      for (u32 variant = 0; variant < 14; ++variant) {
        SCOPED_TRACE(::testing::Message()
                     << hardware << "," << wide << "," << variant);
        Harness h(wide);
        ASSERT_TRUE(h.manager.set_backend(&backend));
        const auto factory = h.create();
        const auto target =
            h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED,
                     hardware ? D2D1_RENDER_TARGET_TYPE_HARDWARE
                              : D2D1_RENDER_TARGET_TYPE_SOFTWARE);
        std::array<u8, 16 * 16 * 4> seed{};
        const auto bitmap = h.gdi.create_bitmap_from_rgba(16, 16, seed);
        const auto dc = h.gdi.create_compatible_dc(0);
        ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
        const i32 bound[] = {0, 0, 16, 16};
        (void)h.memory.write_bytes(0x13040, reinterpret_cast<const u8 *>(bound),
                                   sizeof(bound));
        h.args({target, dc, 0x13040});
        h.manager.target_method(h.marshaler, target_BindDC);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        const float red[] = {1, 0, 0,
                             (variant == 10 || variant == 11 || variant == 13)
                                 ? .5f
                                 : 1.f},
                    blue[] = {0, 0, 1, 1};
        (void)h.memory.write_bytes(0x13100, reinterpret_cast<const u8 *>(red),
                                   sizeof(red));
        (void)h.memory.write_bytes(0x13120, reinterpret_cast<const u8 *>(blue),
                                   sizeof(blue));
        h.args({target, 0x13100, 0, 0x12020});
        h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        const auto brush = h.pointer(0x12020);
        h.args({factory, 0x12040});
        h.manager.factory_method(h.marshaler, factory_CreatePathGeometry);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        const auto path = h.pointer(0x12040);
        h.args({path, 0x12050});
        h.manager.geometry_method(h.marshaler, path_Open, false);
        const auto sink = h.pointer(0x12050);
        if (variant == 2 || variant == 3 || variant == 13) {
          h.args({sink, D2D1_FILL_MODE_WINDING});
          h.manager.geometry_method(h.marshaler, sink_SetFillMode, true);
        }
        const auto point = [&](usize slot, float x, float y, u32 flag = 0) {
          u32 xb, yb;
          std::memcpy(&xb, &x, 4);
          std::memcpy(&yb, &y, 4);
          if (wide)
            h.args({sink, u64(xb) | (u64(yb) << 32), flag});
          else
            h.args({sink, xb, yb, flag});
          h.manager.geometry_method(h.marshaler, slot, true);
        };
        const auto end = [&](bool closed) {
        if(variant==0){h.args({sink,D2D1_PATH_SEGMENT_FORCE_UNSTROKED});h.manager.geometry_method(h.marshaler,sink_SetSegmentFlags,true);}
          h.args(
              {sink, closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN});
          h.manager.geometry_method(h.marshaler, sink_EndFigure, true);
        };
        const auto rectangle = [&](float a, float b, bool reverse = false) {
          point(sink_BeginFigure, a, a,
                variant == 5 ? D2D1_FIGURE_BEGIN_HOLLOW
                             : D2D1_FIGURE_BEGIN_FILLED);
          point(sink_AddLine, reverse ? a : b, reverse ? b : a);
          point(sink_AddLine, b, b);
          point(sink_AddLine, reverse ? b : a, reverse ? a : b);
          end(true);
        };
        if (variant == 0) {
          point(sink_BeginFigure, 2, 2);
          point(sink_AddLine, 14, 2);
          point(sink_AddLine, 14, 5);
          point(sink_AddLine, 5, 5);
          point(sink_AddLine, 5, 14);
          point(sink_AddLine, 2, 14);
          end(true);
        } else if (variant >= 1 && variant <= 3) {
          rectangle(2, 14);
          rectangle(6, 10, variant == 3);
        } else if (variant == 4) {
          point(sink_BeginFigure, 2, 2);
          point(sink_AddLine, 14, 2);
          point(sink_AddLine, 2, 14);
          end(false);
        } else if (variant == 11) {
          rectangle(2, 14);
          rectangle(2, 14, true);
        } else if (variant == 12) {
          point(sink_BeginFigure, 2, 2);
          point(sink_AddLine, 14, 14);
          point(sink_AddLine, 2, 14);
          point(sink_AddLine, 14, 2);
          end(true);
        } else if (variant == 13) {
          rectangle(2, 10);
          rectangle(6, 14);
        } else if (variant == 7) {
          point(sink_BeginFigure, 2, 2);
          point(sink_AddLine, 14, 2);
          point(sink_AddLine, 14, 14);
          end(false);
        } else if (variant == 8) {
          point(sink_BeginFigure, 2, 2);
          const float curve[] = {2, 14, 14, 14, 14, 2};
          (void)h.memory.write_bytes(
              0x13200, reinterpret_cast<const u8 *>(curve), sizeof(curve));
          h.args({sink, 0x13200});
          h.manager.geometry_method(h.marshaler, sink_AddBezier, true);
          end(true);
        } else
          rectangle(2, variant == 9 ? 6 : 14);
        h.args({sink});
        h.manager.geometry_method(h.marshaler, sink_Close, true);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        h.args({target});
        h.manager.target_method(h.marshaler, target_BeginDraw);
        h.args({target, 0x13120});
        h.manager.target_method(h.marshaler, target_Clear);
        if (variant == 9) {
          const float transform[] = {0, 1, -1, 0, 8, 0};
          (void)h.memory.write_bytes(0x13240,
                                     reinterpret_cast<const u8 *>(transform),
                                     sizeof(transform));
          h.args({target, 0x13240});
          h.manager.target_method(h.marshaler, target_SetTransform);
          h.args({target});
          h.floats(192, 192);
          h.manager.target_method(h.marshaler, target_SetDpi);
        }
        h.args(
            {target, path, brush,
             (variant == 6 || variant == 7 || variant == 11) ? 0x40000000u : 0u,
             0});
        if (wide)
          h.cpu.sse.xmm[3].u32_[0] = 0x40000000;
        h.manager.target_method(h.marshaler,
                                (variant == 6 || variant == 7 || variant == 11)
                                    ? target_DrawGeometry
                                    : target_FillGeometry);
        h.args({target, 0, 0});
        h.manager.target_method(h.marshaler, target_EndDraw);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
        const auto is_red = [&](int x, int y) {
          EXPECT_EQ(h.gdi.get_pixel(dc, x, y), 0x000000ffu);
        };
        const auto is_blue = [&](int x, int y) {
          EXPECT_EQ(h.gdi.get_pixel(dc, x, y), 0x00ff0000u);
        };
        if (variant == 0) {
          is_red(3, 10);
          is_red(10, 3);
          is_blue(10, 10);
        } else if (variant <= 3) {
          is_red(3, 3);
          if (variant == 2)
            is_red(8, 8);
          else
            is_blue(8, 8);
        } else if (variant == 4) {
          is_red(3, 3);
          is_blue(12, 12);
        } else if (variant == 5)
          is_blue(8, 8);
        else if (variant == 6) {
          is_red(1, 8);
          is_blue(8, 8);
          is_blue(0, 8);
        } else if (variant == 7) {
          is_red(8, 2);
          is_blue(8, 13);
        } else if (variant == 8) {
          is_red(8, 5);
          is_blue(8, 13);
        } else if (variant == 9) {
          is_red(8, 8);
          is_blue(1, 1);
        } else if (variant == 11) {
          EXPECT_EQ(h.gdi.get_pixel(dc, 1, 8), 0x007f0080u);
          is_blue(8, 8);
        } else if (variant == 12) {
          is_red(8, 3);
          is_red(8, 12);
          is_blue(3, 8);
        } else {
          const auto pixel = h.gdi.get_pixel(dc, 8, 8);
          EXPECT_GE(pixel & 255, 127u);
          EXPECT_LE(pixel & 255, 128u);
          EXPECT_GE((pixel >> 16) & 255, 127u);
          EXPECT_LE((pixel >> 16) & 255, 128u);
        }
      }
}

TEST(D2D1GeometryDrawing, FailuresAreDeferredWithTags) {
  using namespace d2d1_contract;
  for (bool wide : {false, true})
    for (unsigned reason = 0; reason < 11; ++reason) {
      SCOPED_TRACE(::testing::Message() << wide << "," << reason);
      Harness h(wide);
      const auto factory = h.create();
      const auto target = h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED,
                                   D2D1_RENDER_TARGET_TYPE_SOFTWARE);
      std::array<u8, 16 * 16 * 4> pixels{};
      const auto bitmap = h.gdi.create_bitmap_from_rgba(16, 16, pixels);
      const auto dc = h.gdi.create_compatible_dc(0);
      (void)h.gdi.select_object(dc, bitmap);
      const i32 bounds[] = {0, 0, 16, 16};
      (void)h.memory.write_bytes(0x13040, reinterpret_cast<const u8 *>(bounds),
                                 sizeof(bounds));
      if (reason) {
        h.args({target, dc, 0x13040});
        h.manager.target_method(h.marshaler, target_BindDC);
        ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      }
      const auto path_factory = reason == 8 ? h.create() : factory;
      h.args({path_factory, 0x12040});
      h.manager.factory_method(h.marshaler, factory_CreatePathGeometry);
      const auto path = h.pointer(0x12040);
      h.args({path, 0x12050});
      h.manager.geometry_method(h.marshaler, path_Open, false);
      const auto sink = h.pointer(0x12050);
      const auto point = [&](usize slot, float x, float y) {
        u32 xb, yb;
        std::memcpy(&xb, &x, 4);
        std::memcpy(&yb, &y, 4);
        if (wide)
          h.args({sink, u64(xb) | (u64(yb) << 32), 0});
        else
          h.args({sink, xb, yb, 0});
        h.manager.geometry_method(h.marshaler, slot, true);
      };
      point(sink_BeginFigure, reason == 7 ? 1e8f : 2, 2);
      if (reason == 1) {
        h.args({sink, 0x14000});
        h.manager.geometry_method(h.marshaler, sink_AddArc, true);
      } else if (reason == 5) {
        for (unsigned i = 0; i < 200; ++i)
          point(sink_AddLine, 1 + (i % 10), 1 + (i / 10));
      } else if (reason == 6) {
        point(sink_AddLine, 10, 2);
        point(sink_AddLine, 2, 2.01f);
      } else if (reason != 7) {
        if (reason == 4) {
          h.args({sink, D2D1_PATH_SEGMENT_FORCE_UNSTROKED});
          h.manager.geometry_method(h.marshaler, sink_SetSegmentFlags, true);
        }
        point(sink_AddLine, 14, 2);
        point(sink_AddLine, 14, 14);
      }
      if(reason==10){h.args({sink,D2D1_PATH_SEGMENT_FORCE_UNSTROKED});h.manager.geometry_method(h.marshaler,sink_SetSegmentFlags,true);}
      h.args({sink, reason==10?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN});
      h.manager.geometry_method(h.marshaler, sink_EndFigure, true);
      if(reason==10){h.args({sink,D2D1_PATH_SEGMENT_NONE});h.manager.geometry_method(h.marshaler,sink_SetSegmentFlags,true);}
      h.args({sink});
      h.manager.geometry_method(h.marshaler, sink_Close, true);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      const float red[] = {1, 0, 0, 1}, blue[] = {0, 0, 1, 1};
      (void)h.memory.write_bytes(0x13100, reinterpret_cast<const u8 *>(red),
                                 sizeof(red));
      (void)h.memory.write_bytes(0x13120, reinterpret_cast<const u8 *>(blue),
                                 sizeof(blue));
      const auto brush_target =
          reason == 9 ? h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED,
                                 D2D1_RENDER_TARGET_TYPE_SOFTWARE)
                      : target;
      h.args({brush_target, 0x13100, 0, 0x12020});
      h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
      const auto brush = h.pointer(0x12020);
      h.args(wide ? std::initializer_list<u64>{target, 31, 32}
                  : std::initializer_list<u64>{target, 31, 0, 32, 0});
      h.manager.target_method(h.marshaler, target_SetTags);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      if (reason) {
        h.args({target, 0x13120});
        h.manager.target_method(h.marshaler, target_Clear);
      }
      const bool stroke = reason == 2 || reason == 4 || reason == 6 || reason == 10;
      h.args({target, path, brush,
              stroke        ? 0x40000000u
              : reason == 3 ? 1u
                            : 0u,
              reason == 2 ? 1u : 0u});
      if (wide)
        h.cpu.sse.xmm[3].u32_[0] = 0x40000000;
      h.manager.target_method(h.marshaler, stroke ? target_DrawGeometry
                                                  : target_FillGeometry);
      h.args({target, 0x13400, 0x13408});
      h.manager.target_method(h.marshaler, target_EndDraw);
      EXPECT_EQ(h.cpu.gpr.rax, reason == 0   ? D2DERR_WRONG_STATE
                               : reason == 8 ? D2DERR_WRONG_FACTORY
                               : reason == 9 ? D2DERR_WRONG_RESOURCE_DOMAIN
                                             : E_NOTIMPL);
      EXPECT_EQ(h.memory.read_u64(0x13400).value(), 31u);
      EXPECT_EQ(h.memory.read_u64(0x13408).value(), 32u);
      // Pixel validity after a failed EndDraw is unspecified; do not infer it.
    }
}

TEST(D2D1GeometryDrawing, EllipticalArcSweepSizeRotationAndSdkTransportBothWidths) {
  using namespace d2d1_contract;
  auto &backend=graphics::MetalDevice::instance();
  if(!backend.available())GTEST_SKIP()<<"Metal required for arc acceptance";
  for(bool wide:{false,true})for(float rotation:{0.f,30.f})for(bool clockwise:{false,true})for(bool large:{false,true})for(bool stroke:{false,true}){
    SCOPED_TRACE(::testing::Message()<<wide<<","<<rotation<<","<<clockwise<<","<<large<<","<<stroke);
    Harness h(wide);ASSERT_TRUE(h.manager.set_backend(&backend));const auto factory=h.create();const auto target=h.target(factory,D2D1_ALPHA_MODE_PREMULTIPLIED,D2D1_RENDER_TARGET_TYPE_HARDWARE);
    std::array<u8,32*32*4> initial{};const auto bitmap=h.gdi.create_bitmap_from_rgba(32,32,initial);const auto dc=h.gdi.create_compatible_dc(0);ASSERT_NE(h.gdi.select_object(dc,bitmap),0u);
    const i32 bounds[]={0,0,32,32};(void)h.memory.write_bytes(0x13040,reinterpret_cast<const u8*>(bounds),sizeof(bounds));h.args({target,dc,0x13040});h.manager.target_method(h.marshaler,target_BindDC);ASSERT_EQ(h.cpu.gpr.rax,S_OK);
    const float red[]={1,0,0,1},blue[]={0,0,1,1};(void)h.memory.write_bytes(0x13100,reinterpret_cast<const u8*>(red),sizeof(red));(void)h.memory.write_bytes(0x13120,reinterpret_cast<const u8*>(blue),sizeof(blue));h.args({target,0x13100,0,0x12020});h.manager.target_method(h.marshaler,target_CreateSolidColorBrush);const auto brush=h.pointer(0x12020);
    h.args({factory,0x12040});h.manager.factory_method(h.marshaler,factory_CreatePathGeometry);const auto path=h.pointer(0x12040);h.args({path,0x12050});h.manager.geometry_method(h.marshaler,path_Open,false);const auto sink=h.pointer(0x12050);
    const double phi=rotation*3.14159265358979323846/180;const float x=12+5*std::cos(phi),y=12+5*std::sin(phi);u32 xb,yb;std::memcpy(&xb,&x,4);std::memcpy(&yb,&y,4);if(wide)h.args({sink,u64(xb)|(u64(yb)<<32),D2D1_FIGURE_BEGIN_FILLED});else h.args({sink,xb,yb,D2D1_FIGURE_BEGIN_FILLED});h.manager.geometry_method(h.marshaler,sink_BeginFigure,true);
    const float end[]={float(12-3*std::sin(phi)),float(12+3*std::cos(phi))},radii[]={5,3};
    (void)h.memory.write_bytes(0x14000+offset_D2D1_ARC_SEGMENT_point,reinterpret_cast<const u8*>(end),sizeof(end));(void)h.memory.write_bytes(0x14000+offset_D2D1_ARC_SEGMENT_size,reinterpret_cast<const u8*>(radii),sizeof(radii));(void)h.memory.write_bytes(0x14000+offset_D2D1_ARC_SEGMENT_rotationAngle,reinterpret_cast<const u8*>(&rotation),sizeof(rotation));
    (void)h.memory.write_u32(0x14000+offset_D2D1_ARC_SEGMENT_sweepDirection,clockwise?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);(void)h.memory.write_u32(0x14000+offset_D2D1_ARC_SEGMENT_arcSize,large?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL);
    h.args({sink,0x14000});h.manager.geometry_method(h.marshaler,sink_AddArc,true);h.args({sink,D2D1_FIGURE_END_OPEN});h.manager.geometry_method(h.marshaler,sink_EndFigure,true);h.args({sink});h.manager.geometry_method(h.marshaler,sink_Close,true);ASSERT_EQ(h.cpu.gpr.rax,S_OK);
    h.args({target});h.manager.target_method(h.marshaler,target_BeginDraw);h.args({target,0x13120});h.manager.target_method(h.marshaler,target_Clear);h.args({target,path,brush,stroke?0x3f800000u:0u,0});if(wide)h.cpu.sse.xmm[3].u32_[0]=0x3f800000;h.manager.target_method(h.marshaler,stroke?target_DrawGeometry:target_FillGeometry);h.args({target,0,0});h.manager.target_method(h.marshaler,target_EndDraw);ASSERT_EQ(h.cpu.gpr.rax,S_OK);
    double coverage=0;for(int row=0;row<32;++row)for(int col=0;col<32;++col)coverage+=(h.gdi.get_pixel(dc,col,row)&255)/255.;
    // Analytical closed-segment areas: rx*ry*(delta-sin(delta))/2
    // are about4.28 and42.84. Broad interior/area checks deliberately do not
    // assert unmeasured Windows boundary AA bytes.
    if(!stroke){if(large)EXPECT_GT(coverage,30);else EXPECT_LT(coverage,10);}
    EXPECT_GT(coverage,1);EXPECT_EQ(h.gdi.get_pixel(dc,0,0),0x00ff0000u);
  }
}

TEST(D2D1GeometryDiagnostics, ReportsArcLambdaJoinVectorsAndMoveCoordinates) {
  using namespace graphics;
  std::vector<PathEdge> edges;PathMathFailure failure;
  const PathElement arc[]={{PathVerb::Move,{0,0}},{PathVerb::Arc,{10,0,1,1,0}}};
  EXPECT_FALSE(path_edges(arc,false,0,{1,0,0,1,0,0},edges,&failure));
  EXPECT_EQ(failure.reason,PathFailureReason::InsufficientArcRadii);
  EXPECT_EQ(failure.values[7],25); // half-chord5 squared / radius1 squared
  const PathElement join[]={{PathVerb::Move,{0,0}},{PathVerb::Line,{10,0}},{PathVerb::Line,{0,.01f}}};
  EXPECT_FALSE(path_edges(join,true,1,{1,0,0,1,0,0},edges,&failure));
  EXPECT_EQ(failure.reason,PathFailureReason::OverLimitMiter);
  EXPECT_EQ(failure.values[4],10);EXPECT_GT(failure.values[7],10);
  const PathElement move[]={{PathVerb::Move,{1e8f,0}}};
  EXPECT_FALSE(path_edges(move,false,0,{1,0,0,1,0,0},edges,&failure));
  EXPECT_EQ(failure.reason,PathFailureReason::InvalidMove);
  EXPECT_EQ(failure.values[0],1e8);
}

TEST(D2D1GeometryDiagnostics, ArcRunsExactlyFromCurrentPointToItsEndpoint) {
  using namespace graphics;
  std::vector<PathEdge> edges;PathMathFailure failure;
  // Rounded corner: the ellipse start angle is pi, so cos/sin reconstruction
  // of the start point lands sin(pi)*27 off (0,27). D2D1_ARC_SEGMENT starts at
  // the current point; no rounding-length reversed edge may appear.
  const PathElement corner[]={{PathVerb::Move,{0,27}},{PathVerb::Arc,{27,0,27,27,0},true,false}};
  ASSERT_TRUE(path_edges(corner,false,0,{1,0,0,1,0,0},edges,&failure));
  ASSERT_FALSE(edges.empty());
  EXPECT_EQ(edges.front()[0],0.f);EXPECT_EQ(edges.front()[1],27.f);
  EXPECT_EQ(edges.back()[2],27.f);EXPECT_EQ(edges.back()[3],0.f);
  for(const auto& e:edges)EXPECT_LE(e[1],27.f);
  const PathElement outline[]={{PathVerb::Move,{0,300}},{PathVerb::Line,{0,27}},
    {PathVerb::Arc,{27,0,27,27,0},true,false},{PathVerb::Line,{300,0}},
    {PathVerb::Line,{300,300}},{PathVerb::Close}};
  EXPECT_TRUE(path_edges(outline,true,2,{1,0,0,1,0,0},edges,&failure))
      <<path_failure_name(failure.reason);
}

#include "winapi/graphics/d2d1/d2d1_text_contract.inc"
#include "winapi/graphics/dwrite/dwrite.hpp"
#include "graphics/metal/metal_device.hpp"
#include "../dwrite/dwrite_manager_harness.hpp"

namespace {
// DirectWrite next to a D2D harness, sharing its guest memory.
struct TextObjects {
  DWriteManager dwrite;
  GuestAddr format{0}, layout{0};
  TextObjects(Harness &h, const std::u16string &text, float width, float height)
      : dwrite(h.memory, h.wide) {
    dwrite.set_method_thunks(dwrite_test::DWriteManagerTest::method_thunks());
    h.manager.set_dwrite(&dwrite);
    // IID_IDWriteFactory {b859ee5a-d838-4b5b-a2e8-1adc7d93db48}.
    const u8 iid[16] = {0x5a, 0xee, 0x59, 0xb8, 0x38, 0xd8, 0x5b, 0x4b,
                        0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48};
    (void)h.memory.write_bytes(0x15000, iid, 16);
    h.args({0, 0x15000, 0x15100});
    dwrite.impl_DWriteCreateFactory(h.marshaler);
    const GuestAddr factory = h.pointer(0x15100);
    const std::u16string family = u"Arial", locale = u"en-us";
    (void)h.memory.write_bytes(0x15200, reinterpret_cast<const u8 *>(family.c_str()), 12);
    (void)h.memory.write_bytes(0x15240, reinterpret_cast<const u8 *>(locale.c_str()), 12);
    const float size = 20;
    u32 size_bits;
    std::memcpy(&size_bits, &size, 4);
    h.args({factory, 0x15200, 0, 400, 0, 5, size_bits, 0x15240, 0x15300});
    dwrite.impl_CreateTextFormat(h.marshaler);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    format = h.pointer(0x15300);
    (void)h.memory.write_bytes(0x15400, reinterpret_cast<const u8 *>(text.c_str()), text.size() * 2);
    u32 w, ht;
    std::memcpy(&w, &width, 4);
    std::memcpy(&ht, &height, 4);
    h.args({factory, 0x15400, text.size(), format, w, ht, 0x15380});
    dwrite.impl_CreateTextLayout(h.marshaler);
    EXPECT_EQ(h.cpu.gpr.rax, 0u);
    layout = h.pointer(0x15380);
  }
};

void draw_text_layout(Harness &h, GuestAddr target, float x, float y, GuestAddr layout,
                      GuestAddr brush, u32 options) {
  u32 xb, yb;
  std::memcpy(&xb, &x, 4);
  std::memcpy(&yb, &y, 4);
  if (h.wide) h.args({target, u64(xb) | (u64(yb) << 32), layout, brush, options});
  else h.args({target, xb, yb, layout, brush, options});
  h.manager.target_method(h.marshaler, d2d1_contract::target_DrawTextLayout);
}
} // namespace

TEST(D2D1Text, DrawTextLayoutPaintsGrayscaleGlyphCoverageBothWidths) {
  using namespace d2d1_contract;
  if (!dwrite::match_system_font(u"Arial", 400, 5, 0)) GTEST_SKIP() << "host has no Arial";
  auto &backend = graphics::MetalDevice::instance();
  for (bool wide : {false, true})
    for (u32 type : {D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1_RENDER_TARGET_TYPE_HARDWARE}) {
      if (type == D2D1_RENDER_TARGET_TYPE_HARDWARE && !backend.available()) continue;
      SCOPED_TRACE(::testing::Message() << wide << "," << type);
      Harness h(wide);
      ASSERT_TRUE(h.manager.set_backend(&backend));
      TextObjects text(h, u"Hi", 64, 32);
      const auto factory = h.create();
      const auto target = h.target(factory, D2D1_ALPHA_MODE_PREMULTIPLIED, type);
      std::array<u8, 64 * 32 * 4> initial{};
      const auto bitmap = h.gdi.create_bitmap_from_rgba(64, 32, initial);
      const auto dc = h.gdi.create_compatible_dc(0);
      ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
      const i32 bounds[] = {0, 0, 64, 32};
      (void)h.memory.write_bytes(0x13040, reinterpret_cast<const u8 *>(bounds), sizeof(bounds));
      h.args({target, dc, 0x13040});
      h.manager.target_method(h.marshaler, target_BindDC);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      const float red[] = {1, 0, 0, 1}, blue[] = {0, 0, 1, 1};
      (void)h.memory.write_bytes(0x13100, reinterpret_cast<const u8 *>(red), sizeof(red));
      (void)h.memory.write_bytes(0x13120, reinterpret_cast<const u8 *>(blue), sizeof(blue));
      h.args({target, 0x13100, 0, 0x12020});
      h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
      const auto brush = h.pointer(0x12020);
      h.args({target});
      h.manager.target_method(h.marshaler, target_BeginDraw);
      h.args({target, 0x13120});
      h.manager.target_method(h.marshaler, target_Clear);
      draw_text_layout(h, target, 2, 2, text.layout, brush, 0);
      h.args({target, 0, 0});
      h.manager.target_method(h.marshaler, target_EndDraw);
      ASSERT_EQ(h.cpu.gpr.rax, S_OK);
      // "Hi" in Arial 20 at (2,2): the H stem is ink, the far right is not.
      // Premultiplied red over opaque blue: red rises where blue falls.
      u32 red_pixels = 0;
      for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 64; ++col) {
          const u32 p = h.gdi.get_pixel(dc, col, row);
          if ((p & 0xff) > 0x80) ++red_pixels;
          if ((p & 0xff) != 0) EXPECT_EQ((p & 0xff) + ((p >> 16) & 0xff), 0xffu) << col << "," << row;
        }
      EXPECT_GT(red_pixels, 40u);
      EXPECT_EQ(h.gdi.get_pixel(dc, 63, 31), 0x00ff0000u);
      EXPECT_EQ(h.gdi.get_pixel(dc, 4, 10) & 0xff, 0xffu);  // inside the H's left stem
    }
}

TEST(D2D1Text, UnsupportedTextModesFailAtEndDrawBothWidths) {
  using namespace d2d1_contract;
  if (!dwrite::match_system_font(u"Arial", 400, 5, 0)) GTEST_SKIP() << "host has no Arial";
  for (bool wide : {false, true}) {
    Harness h(wide);
    TextObjects text(h, u"Hi", 64, 32);
    const auto factory = h.create();
    // Learn D2D1_TEXT_ANTIALIAS_MODE: DEFAULT on an IGNORE-alpha target is
    // ClearType, which Vitreo does not rasterize.
    const auto target = h.target(factory, D2D1_ALPHA_MODE_IGNORE, D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    std::array<u8, 64 * 32 * 4> initial{};
    const auto bitmap = h.gdi.create_bitmap_from_rgba(64, 32, initial);
    const auto dc = h.gdi.create_compatible_dc(0);
    ASSERT_NE(h.gdi.select_object(dc, bitmap), 0u);
    const i32 bounds[] = {0, 0, 64, 32};
    (void)h.memory.write_bytes(0x13040, reinterpret_cast<const u8 *>(bounds), sizeof(bounds));
    h.args({target, dc, 0x13040});
    h.manager.target_method(h.marshaler, target_BindDC);
    const float red[] = {1, 0, 0, 1};
    (void)h.memory.write_bytes(0x13100, reinterpret_cast<const u8 *>(red), sizeof(red));
    h.args({target, 0x13100, 0, 0x12020});
    h.manager.target_method(h.marshaler, target_CreateSolidColorBrush);
    const auto brush = h.pointer(0x12020);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    draw_text_layout(h, target, 0, 0, text.layout, brush, 0);
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, E_NOTIMPL);
    // GRAYSCALE on the same target draws.
    h.args({target, d2d1_text_contract::D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE});
    h.manager.target_method(h.marshaler, target_SetTextAntialiasMode);
    h.args({target});
    h.manager.target_method(h.marshaler, target_GetTextAntialiasMode);
    EXPECT_EQ(h.cpu.gpr.rax, d2d1_text_contract::D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    draw_text_layout(h, target, 0, 0, text.layout, brush, 0);
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, S_OK);
    // Learn DrawTextLayout: a non-brush ID2D1Resource effect is an error.
    h.args({factory, 0x12040});
    h.manager.factory_method(h.marshaler, factory_CreatePathGeometry);
    const auto path = h.pointer(0x12040);
    const auto drawing_effect = [&](GuestAddr effect) {
      // IDWriteTextLayout::SetDrawingEffect(effect, {0, 2}).
      if (wide) h.args({text.layout, effect, u64(2) << 32});
      else h.args({text.layout, effect, 0, 2});
      text.dwrite.text_method(h.marshaler, DWriteManager::TextInterface::Layout,
                              dwrite_text_contract::text_layout_SetDrawingEffect);
    };
    drawing_effect(path);
    h.args({target});
    h.manager.target_method(h.marshaler, target_BeginDraw);
    draw_text_layout(h, target, 0, 0, text.layout, brush, 0);
    h.args({target, 0, 0});
    h.manager.target_method(h.marshaler, target_EndDraw);
    EXPECT_EQ(h.cpu.gpr.rax, E_INVALIDARG);
  }
}
