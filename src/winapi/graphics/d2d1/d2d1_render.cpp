// SPDX-License-Identifier: LGPL-2.1-only
#include "core/memory/guest_memory.hpp"
#include "d2d1_internal.hpp"
#include "d2d1_text_contract.inc"
#include "d2d1_capabilities_contract.inc"
#include "graphics/common/hardware_solid_surface.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_set>
namespace vitreo::winapi {
using namespace d2d1_contract;
namespace {
constexpr usize region_size = GUEST_PAGE_SIZE;
void unsupported(const char *method, const char *reason) {
  static std::mutex mutex;
  static std::unordered_set<std::string> reported;
  std::lock_guard lock(mutex);
  const std::string key = std::string(method) + reason;
  if (reported.insert(key).second)
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d2d1.dll!%s: %s\n", method,
                 reason);
}
float clamp(float value) { return std::clamp(value, 0.f, 1.f); }
u8 quantize(float value) {
  const float scaled = clamp(value) * 255.f;
  const auto lower = static_cast<u32>(scaled);
  const float fraction = scaled - lower;
  return static_cast<u8>(lower +
                         (fraction > .5f || (fraction == .5f && (lower & 1u))));
}
bool finite(const float *values, usize count) {
  for (usize i = 0; i < count; ++i)
    if (!std::isfinite(values[i]))
      return false;
  return true;
}
} // namespace
bool D2DManager::read_data(Marshaler &m, GuestAddr address, void *data,
                           usize size) {
  if (m_memory.read_bytes(address, static_cast<Byte *>(data), size).is_ok())
    return true;
  (void)m.raise_access_violation(false, address);
  return false;
}
bool D2DManager::write_data(Marshaler &m, GuestAddr address, const void *data,
                            usize size) {
  if (m_memory.write_bytes(address, static_cast<const Byte *>(data), size)
          .is_ok())
    return true;
  (void)m.raise_access_violation(true, address);
  return false;
}
GuestAddr D2DManager::allocate_resource(GuestAddr factory, bool brush,
                                        std::shared_ptr<Target> target) {
  const auto base = m_memory.map_first_fit(0x60000000u, region_size,
                                           MemoryProtection::ReadWrite);
  if (!base)
    return 0;
  const usize width = m_is_64bit ? 8 : 4;
  const GuestAddr vtable = base + width;
  auto put = [&](GuestAddr address, GuestAddr value) {
    return m_is_64bit
               ? m_memory.write_u64(address, value).is_ok()
               : m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
  };
  bool ok = put(base, vtable);
  const usize count = brush ? m_brush_methods.size() : m_target_methods.size();
  for (usize i = 0; ok && i < count; ++i)
    ok = put(vtable + i * width,
             brush ? m_brush_methods[i] : m_target_methods[i]);
  if (!ok) {
    (void)m_memory.unmap_region(base, region_size);
    return 0;
  }
  m_resources.emplace(base, Resource{1, factory, brush, std::move(target)});
  ++m_factories.at(factory).references;
  return base;
}
void D2DManager::release_factory(GuestAddr factory) {
  auto it = m_factories.find(factory);
  if (it == m_factories.end())
    return;
  if (--it->second.references == 0) {
    m_factories.erase(it);
    (void)m_memory.unmap_region(factory, region_size);
  }
}
bool D2DManager::resource_identity(Marshaler &m, Resource &resource,
                                   GuestAddr self, bool brush, usize slot) {
  if (slot == 0) {
    if (m.arg(2) == 0) {
      m.set_return(E_POINTER);
      return true;
    }
    if (!write_pointer(m, m.arg(2), 0))
      return true;
    std::array<u8, 16> iid{};
    if (!read_iid(m, m.arg(1), iid))
      return true;
    const bool supported =
        iid == unknown_iid || iid == iid_ID2D1Resource ||
        (resource.bitmap ? (iid == iid_ID2D1Image || iid == iid_ID2D1Bitmap) : brush ? (iid == iid_ID2D1Brush || iid == iid_ID2D1SolidColorBrush)
               : (iid == iid_ID2D1RenderTarget ||
                  iid == iid_ID2D1DCRenderTarget));
    if (!supported) {
      m.set_return(E_NOINTERFACE);
      return true;
    }
    if (!write_pointer(m, m.arg(2), self))
      return true;
    ++resource.references;
    m.set_return(S_OK);
    return true;
  }
  if (slot == 1) {
    m.set_return(++resource.references);
    return true;
  }
  if (slot == 2) {
    const auto factory = resource.factory;
    const u32 left = --resource.references;
    if (!left) {
      m_resources.erase(self);
      (void)m_memory.unmap_region(self, region_size);
      release_factory(factory);
    }
    m.set_return(left);
    return true;
  }
  if (slot == 3) {
    if (write_pointer(m, m.arg(1), resource.factory))
      ++m_factories.at(resource.factory).references;
    return true;
  }
  return false;
}
void D2DManager::create_dc_target(Marshaler &m, GuestAddr factory) {
  if (!write_pointer(m, m.arg(2), 0))
    return;
  std::array<u32, layout_props / sizeof(u32)> p{};
  if (!read_data(m, m.arg(1), p.data(), layout_props))
    return;
  const u32 type = p[layout_type / 4], format = p[layout_format / 4],
            alpha = p[layout_format / 4 + 1], usage = p[layout_usage / 4],
            level = p[layout_level / 4];
  float dpi_x, dpi_y;
  std::memcpy(&dpi_x, &p[layout_dpiX / 4], 4);
  std::memcpy(&dpi_y, &p[layout_dpiY / 4], 4);
  if (type > D2D1_RENDER_TARGET_TYPE_HARDWARE ||
      alpha > D2D1_ALPHA_MODE_IGNORE ||
      usage > (D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING |
               D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE) ||
      (level != 0 && level != D3D_FEATURE_LEVEL_9_1 &&
       level != D3D_FEATURE_LEVEL_10_0) ||
      !std::isfinite(dpi_x) || !std::isfinite(dpi_y) || dpi_x < 0 ||
      dpi_y < 0 || ((dpi_x == 0) != (dpi_y == 0))) {
    m.set_return(E_INVALIDARG);
    return;
  }
  if (format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      (alpha != D2D1_ALPHA_MODE_PREMULTIPLIED &&
       alpha != D2D1_ALPHA_MODE_IGNORE)) {
    m.set_return(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
    return;
  }
  if (!m_gdi) {
    unsupported("ID2D1Factory::CreateDCRenderTarget",
                "GDI raster bridge unavailable");
    m.set_return(E_NOTIMPL);
    return;
  }
  std::unique_ptr<graphics::HardwareSolidSurface> gpu;
  // DEFAULT chooses genuine GPU rasterization when available. A hardware
  // minimum not yet certified by this frontend may still fall back for
  // DEFAULT, while SOFTWARE explicitly stays on the CPU raster backend.
  if (type != D2D1_RENDER_TARGET_TYPE_SOFTWARE && level == 0 && m_backend &&
      m_backend->available())
    gpu = graphics::HardwareSolidSurface::create(*m_backend);
  if (type == D2D1_RENDER_TARGET_TYPE_HARDWARE && !gpu) {
    unsupported("ID2D1Factory::CreateDCRenderTarget",
                level ? "hardware feature-level certification unavailable"
                      : "hardware rendering unavailable");
    m.set_return(E_NOTIMPL);
    return;
  }
  auto target = std::make_shared<Target>();
  target->gpu = std::move(gpu);
  target->alpha = alpha;
  target->usage = usage;
  target->dpi_x = dpi_x == 0 ? dip_screen_dpi : dpi_x;
  target->dpi_y = dpi_y == 0 ? dip_screen_dpi : dpi_y;
  const auto address = allocate_resource(factory, false, std::move(target));
  if (!address) {
    m.set_return(E_OUTOFMEMORY);
    return;
  }
  if (!write_pointer(m, m.arg(2), address)) {
    m_resources.erase(address);
    (void)m_memory.unmap_region(address, region_size);
    release_factory(factory);
    return;
  }
  m.set_return(S_OK);
}
void D2DManager::target_method(Marshaler &m, usize slot) {
  // A final Release drops the target's IDWriteRenderingParams reference
  // after m_mutex is released: DirectWrite is never entered under it.
  GuestAddr release_after = 0;
  if (slot == 2) {
    std::lock_guard lock(m_mutex);
    const auto it = m_resources.find(m.arg(0));
    if (it != m_resources.end() && !it->second.brush && !it->second.bitmap &&
        it->second.target && it->second.references == 1)
      release_after = std::exchange(it->second.target->text_rendering_params, 0);
  }
  target_method_locked(m, slot);
  if (release_after && m_guest_interface_call)
    (void)m_guest_interface_call(m, release_after, 2, nullptr, 0);
}

void D2DManager::target_method_locked(Marshaler &m, usize slot) {
  if(slot==target_CreateBitmapFromWicBitmap){create_bitmap_from_wic(m,m.arg(0));return;}
  if (slot == target_DrawText || slot == target_DrawTextLayout ||
      slot == target_DrawGlyphRun || slot == target_SetTextRenderingParams ||
      slot == target_GetTextRenderingParams) {
    draw_text(m, slot);
    return;
  }
  std::lock_guard lock(m_mutex);
  const auto self = m.arg(0);
  auto it = m_resources.find(self);
  if (it == m_resources.end() || it->second.brush || it->second.bitmap) {
    (void)m.raise_access_violation(false, self);
    return;
  }
  auto &resource = it->second;
  if (resource_identity(m, resource, self, false, slot))
    return;
  auto &t = *resource.target;
  const auto finish = [&](bool end) {
    if (end && !t.drawing)
      t.latch(D2DERR_WRONG_STATE);
    if (end && !t.clips.empty())
      t.latch(D2DERR_PUSH_POP_UNBALANCED);
    if (t.dirty && t.dc && t.error == S_OK) {
      std::span<const u8> hardware_pixels;
      const bool completed = !t.gpu || t.gpu->readback(hardware_pixels);
      if (!completed)
        t.latch(D2DERR_RECREATE_TARGET);
      else
        t.pending_bitmaps.clear();
      bool unsupported_transform = false;
      const bool found =
          completed &&
          m_gdi->with_device_surface(
              t.dc, [&](GdiManager::DeviceSurface &surface) {
                constexpr double identity[6] = {1, 0, 0, 1, 0, 0};
                if (!std::equal(std::begin(identity), std::end(identity),
                                surface.xform) ||
                    surface.mono) {
                  unsupported_transform = true;
                  return;
                }
                for (u32 y = 0; y < t.height; ++y)
                  for (u32 x = 0; x < t.width; ++x) {
                    const i64 dx = static_cast<i64>(t.rectangle[0]) + x,
                              dy = static_cast<i64>(t.rectangle[1]) + y;
                    if (dx < 0 || dy < 0 || dx >= surface.width ||
                        dy >= surface.height)
                      continue;
                    if (surface.clip && surface.clip->valid) {
                      bool inside = false;
                      for (const auto &r : surface.clip->parts())
                        if (dx >= r[0] && dx < r[2] && dy >= r[1] &&
                            dy < r[3]) {
                          inside = true;
                          break;
                        }
                      if (!inside)
                        continue;
                    }
                    auto *destination =
                        surface.rgba + (dy * surface.width + dx) * 4;
                    const usize offset =
                        (static_cast<usize>(y) * t.width + x) * 4;
                    if (t.gpu) {
                      const auto *source = hardware_pixels.data() + offset;
                      destination[0] = source[2];
                      destination[1] = source[1];
                      destination[2] = source[0];
                      destination[3] = source[3];
                    } else {
                      const auto *source = t.pixels.get() + offset;
                      for (usize c = 0; c < 4; ++c)
                        destination[c] = quantize(source[c]);
                    }
                    // GDI window redirection surfaces use opaque paint
                    // coverage; memory/DIB surfaces advertise alpha=0 and
                    // retain D2D alpha.
                    destination[3] |= surface.alpha;
                  }
              });
      if (!found)
        t.latch(E_INVALIDARG);
      if (unsupported_transform) {
        unsupported("ID2D1DCRenderTarget::EndDraw",
                    "transformed or monochrome destination DC");
        t.latch(E_NOTIMPL);
      }
    }
    const u64 first = t.error == S_OK ? 0 : t.error_tag1,
              second = t.error == S_OK ? 0 : t.error_tag2;
    if (m.arg(1) && !write_data(m, m.arg(1), &first, 8))
      return;
    if (m.arg(2) && !write_data(m, m.arg(2), &second, 8))
      return;
    const auto result = t.error;
    t.error = S_OK;
    t.error_tag1 = t.error_tag2 = 0;
    t.dirty = false;
    if (end) {
      t.drawing = false;
      t.clips.clear();
    }
    m.set_return(result);
  };
  switch (slot) {
  case target_FillGeometry:draw_geometry(m,resource,false);return;
  case target_DrawGeometry:draw_geometry(m,resource,true);return;
  case target_DrawBitmap:draw_bitmap(m,resource);return;
  case target_IsSupported: {
    std::array<u32, layout_props / sizeof(u32)> p{};
    if (!read_data(m, m.arg(1), p.data(), layout_props))
      return;
    const u32 type = p[layout_type / 4], format = p[layout_format / 4],
              alpha = p[layout_format / 4 + 1], usage = p[layout_usage / 4],
              level = p[layout_level / 4];
    // This queries the existing software target, rather than whether a new
    // target could fall back to software. SDK explicitly excludes hardware
    // feature levels for software-target IsSupported; DPI is never evaluated.
    const u32 usage_mask = D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING |
                           D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
    const bool supported =
        (type == D2D1_RENDER_TARGET_TYPE_DEFAULT ||
         type == (t.gpu ? D2D1_RENDER_TARGET_TYPE_HARDWARE
                        : D2D1_RENDER_TARGET_TYPE_SOFTWARE)) &&
        level == 0 &&
        (format == DXGI_FORMAT_UNKNOWN ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
        (alpha == D2D1_ALPHA_MODE_UNKNOWN || alpha == t.alpha) &&
        (usage & ~usage_mask) == 0 &&
        ((usage & D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING) == 0 ||
         (t.usage & D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING) != 0);
    m.set_return(supported ? 1 : 0);
    return;
  }
  case target_BindDC: {
    std::array<i32, 4> r{};
    if (!read_data(m, m.arg(2), r.data(), sizeof(r)))
      return;
    if (m.arg(1) > std::numeric_limits<Handle>::max() ||
        !m_gdi->read_device_surface(static_cast<Handle>(m.arg(1)),
                                    [](const auto &) {})) {
      m.set_return(E_INVALIDARG);
      return;
    }
    const i64 w = static_cast<i64>(r[2]) - r[0],
              h = static_cast<i64>(r[3]) - r[1];
    if (w < 0 || h < 0) {
      unsupported("ID2D1DCRenderTarget::BindDC", "reversed subrectangle");
      m.set_return(E_NOTIMPL);
      return;
    }
    if (w && static_cast<u64>(h) > std::numeric_limits<usize>::max() /
                                       (4 * sizeof(float)) /
                                       static_cast<u64>(w)) {
      m.set_return(E_OUTOFMEMORY);
      return;
    }
    const usize count = static_cast<usize>(w) * static_cast<usize>(h) * 4;
    std::unique_ptr<float[]> buffer;
    if (t.gpu) {
      if (!t.gpu->resize(static_cast<u32>(w), static_cast<u32>(h))) {
        m.set_return(E_OUTOFMEMORY);
        return;
      }
    } else {
      buffer.reset(new (std::nothrow) float[count]{});
      if (count && !buffer) {
        m.set_return(E_OUTOFMEMORY);
        return;
      }
    }
    t.dc = static_cast<Handle>(m.arg(1));
    t.rectangle = r;
    t.width = static_cast<u32>(w);
    t.height = static_cast<u32>(h);
    t.pixels = std::move(buffer);
    t.dirty = false;
    m.set_return(S_OK);
    return;
  }
  case target_CreateSolidColorBrush: {
    if (!write_pointer(m, m.arg(3), 0))
      return;
    std::array<float, 4> color{};
    if (!read_data(m, m.arg(1), color.data(), sizeof(color)))
      return;
    std::array<float, 7> properties{1, 1, 0, 0, 1, 0, 0};
    if (m.arg(2) && !read_data(m, m.arg(2), properties.data(), layout_brush))
      return;
    if (!finite(color.data(), color.size()) ||
        !finite(properties.data(), properties.size())) {
      unsupported("ID2D1RenderTarget::CreateSolidColorBrush",
                  "nonfinite color or brush properties");
      m.set_return(E_NOTIMPL);
      return;
    }
    const auto address =
        allocate_resource(resource.factory, true, resource.target);
    if (!address) {
      m.set_return(E_OUTOFMEMORY);
      return;
    }
    auto &brush = m_resources.at(address);
    brush.color = color;
    brush.opacity = clamp(properties[0]);
    std::copy(properties.begin() + 1, properties.end(),
              brush.transform.begin());
    if (!write_pointer(m, m.arg(3), address)) {
      const auto factory = brush.factory;
      m_resources.erase(address);
      (void)m_memory.unmap_region(address, region_size);
      release_factory(factory);
      return;
    }
    m.set_return(S_OK);
    return;
  }
  case target_BeginDraw: {
    if (t.drawing) {
      t.latch(D2DERR_WRONG_STATE);
      return;
    }
    t.drawing = true;
    if (!t.dc || t.error != S_OK)
      return;
    // Windows re-reads the bound DC for each new batch, including changes
    // after BindDC or between batches. Preserve raw RGBA (also alpha=0).
    const auto interop = t.gpu ? t.gpu->interop_buffer() : std::span<u8>{};
    const bool captured = m_gdi->read_device_surface(
        t.dc, [&](const GdiManager::ReadOnlyDeviceSurface &surface) {
          for (u32 y = 0; y < t.height; ++y)
            for (u32 x = 0; x < t.width; ++x) {
              const i64 sx = static_cast<i64>(t.rectangle[0]) + x,
                        sy = static_cast<i64>(t.rectangle[1]) + y;
              const u8 zero[4] = {};
              const u8 *source = zero;
              if (sx >= 0 && sy >= 0 && sx < surface.width &&
                  sy < surface.height)
                source = surface.rgba + (sy * surface.width + sx) * 4;
              const usize offset = (static_cast<usize>(y) * t.width + x) * 4;
              if (t.gpu) {
                interop[offset] = source[2];
                interop[offset + 1] = source[1];
                interop[offset + 2] = source[0];
                interop[offset + 3] = source[3];
              } else
                for (usize c = 0; c < 4; ++c)
                  t.pixels[offset + c] = source[c] / 255.f;
            }
        });
    if (!captured)
      t.latch(E_INVALIDARG);
    // This can wait/encode GPU work: it is deliberately outside the GDI lock.
    else if (t.gpu && !t.gpu->upload_dc_pixels())
      t.latch(D2DERR_RECREATE_TARGET);
    return;
  }
  case target_EndDraw:
    finish(true);
    return;
  case target_Flush:
    finish(false);
    return;
  case target_SetTags:
    t.tag1 = m_is_64bit ? m.arg(1) : m.arg(1) | (m.arg(2) << 32);
    t.tag2 = m_is_64bit ? m.arg(2) : m.arg(3) | (m.arg(4) << 32);
    return;
  case target_GetTags:
    if (m.arg(1) && !write_data(m, m.arg(1), &t.tag1, 8))
      return;
    if (m.arg(2))
      (void)write_data(m, m.arg(2), &t.tag2, 8);
    return;
  case target_SetTransform:
    if (!read_data(m, m.arg(1), t.transform.data(), sizeof(t.transform)))
      return;
    return;
  case target_GetTransform:
    (void)write_data(m, m.arg(1), t.transform.data(), sizeof(t.transform));
    return;
  case target_SetAntialiasMode:
    if (m.arg(1) > D2D1_ANTIALIAS_MODE_ALIASED) {
      unsupported("ID2D1RenderTarget::SetAntialiasMode", "unknown mode");
      t.latch(E_NOTIMPL);
    } else
      t.antialias = static_cast<u32>(m.arg(1));
    return;
  case target_GetAntialiasMode:
    m.set_return(t.antialias);
    return;
  case target_SetTextAntialiasMode:
    if (m.arg(1) > d2d1_text_contract::D2D1_TEXT_ANTIALIAS_MODE_ALIASED) {
      unsupported("ID2D1RenderTarget::SetTextAntialiasMode", "unknown mode");
      t.latch(E_NOTIMPL);
    } else
      t.text_antialias = static_cast<u32>(m.arg(1));
    return;
  case target_GetTextAntialiasMode:
    m.set_return(t.text_antialias);
    return;
  case target_GetPixelFormat: {
    const u32 p[2] = {DXGI_FORMAT_B8G8R8A8_UNORM, t.alpha};
    if (write_data(m, m.arg(1), p, sizeof(p)))
      m.set_return(m.arg(1));
    return;
  }
  case target_SetDpi: {
    float x = m.arg_f32(1), y = m.arg_f32(2);
    if (x == 0 && y == 0) {
      const auto &factory = m_factories.at(resource.factory);
      t.dpi_x = factory.dpi_x;
      t.dpi_y = factory.dpi_y;
    } else if (x > 0 && y > 0 && std::isfinite(x) && std::isfinite(y)) {
      t.dpi_x = x;
      t.dpi_y = y;
    }
    return;
  }
  case target_GetDpi:
    if (!write_data(m, m.arg(1), &t.dpi_x, 4))
      return;
    (void)write_data(m, m.arg(2), &t.dpi_y, 4);
    return;
  case target_GetSize: {
    const float size[2] = {t.width * dip_screen_dpi / t.dpi_x,
                           t.height * dip_screen_dpi / t.dpi_y};
    if (write_data(m, m.arg(1), size, sizeof(size)))
      m.set_return(m.arg(1));
    return;
  }
  case target_GetPixelSize: {
    const u32 size[2] = {t.width, t.height};
    if (write_data(m, m.arg(1), size, sizeof(size)))
      m.set_return(m.arg(1));
    return;
  }
  case target_PushAxisAlignedClip: {
    if (!t.drawing) {
      t.latch(D2DERR_WRONG_STATE);
      return;
    }
    std::array<float, 4> clip{};
    if (!read_data(m, m.arg(1), clip.data(), sizeof(clip)))
      return;
    if (m.arg(2) != D2D1_ANTIALIAS_MODE_ALIASED ||
        !finite(clip.data(), clip.size()) ||
        !finite(t.transform.data(), t.transform.size()) ||
        t.transform[1] != 0 || t.transform[2] != 0) {
      unsupported("ID2D1RenderTarget::PushAxisAlignedClip",
                  "antialiased or non-axis-preserving clip");
      t.latch(E_NOTIMPL);
      return;
    }
    const float left = (clip[0] * t.transform[0] + t.transform[4]) * t.dpi_x /
                       dip_screen_dpi,
                right = (clip[2] * t.transform[0] + t.transform[4]) * t.dpi_x /
                        dip_screen_dpi;
    const float top = (clip[1] * t.transform[3] + t.transform[5]) * t.dpi_y /
                      dip_screen_dpi,
                bottom = (clip[3] * t.transform[3] + t.transform[5]) * t.dpi_y /
                         dip_screen_dpi;
    clip = {std::min(left, right), std::min(top, bottom), std::max(left, right),
            std::max(top, bottom)};
    if (!t.clips.empty()) {
      const auto &previous = t.clips.back();
      clip = {std::max(clip[0], previous[0]), std::max(clip[1], previous[1]),
              std::min(clip[2], previous[2]), std::min(clip[3], previous[3])};
    }
    t.clips.push_back(clip);
    return;
  }
  case target_PopAxisAlignedClip:
    if (t.clips.empty())
      t.latch(D2DERR_POP_CALL_DID_NOT_MATCH_PUSH);
    else
      t.clips.pop_back();
    return;
  case target_Clear:
  case target_FillRectangle: {
    if (!t.drawing || !t.dc) {
      t.latch(D2DERR_WRONG_STATE);
      return;
    }
    if (t.error != S_OK)
      return;
    std::array<float, 4> color{};
    float opacity = 1;
    std::array<float, 4> r{0, 0, static_cast<float>(t.width),
                           static_cast<float>(t.height)};
    if (slot == target_Clear) {
      if (m.arg(1) && !read_data(m, m.arg(1), color.data(), sizeof(color)))
        return;
    } else {
      if (!read_data(m, m.arg(1), r.data(), sizeof(r)))
        return;
      const auto brush = m_resources.find(m.arg(2));
      if (brush == m_resources.end() || !brush->second.brush) {
        (void)m.raise_access_violation(false, m.arg(2));
        return;
      }
      if (brush->second.factory != resource.factory) {
        t.latch(D2DERR_WRONG_FACTORY);
        return;
      }
      if (brush->second.target != resource.target) {
        t.latch(D2DERR_WRONG_RESOURCE_DOMAIN);
        return;
      }
      color = brush->second.color;
      opacity = brush->second.opacity;
      if (!finite(r.data(), r.size()) ||
          !finite(t.transform.data(), t.transform.size()) ||
          t.transform[1] != 0 || t.transform[2] != 0) {
        unsupported("ID2D1RenderTarget::FillRectangle",
                    "nonfinite or rotated/sheared rectangle transform");
        t.latch(E_NOTIMPL);
        return;
      }
      if (r[2] <= r[0] || r[3] <= r[1])
        return;
      const float x0 = (r[0] * t.transform[0] + t.transform[4]) * t.dpi_x /
                       dip_screen_dpi,
                  x1 = (r[2] * t.transform[0] + t.transform[4]) * t.dpi_x /
                       dip_screen_dpi;
      const float y0 = (r[1] * t.transform[3] + t.transform[5]) * t.dpi_y /
                       dip_screen_dpi,
                  y1 = (r[3] * t.transform[3] + t.transform[5]) * t.dpi_y /
                       dip_screen_dpi;
      r = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1),
           std::max(y0, y1)};
      if (!finite(r.data(), r.size()) ||
          (t.antialias == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE &&
           std::any_of(r.begin(), r.end(),
                       [](float edge) { return edge != std::floor(edge); }))) {
        unsupported("ID2D1RenderTarget::FillRectangle",
                    "fractional per-primitive antialias coverage precision");
        t.latch(E_NOTIMPL);
        return;
      }
    }
    if (!finite(color.data(), color.size())) {
      unsupported("ID2D1RenderTarget::Clear", "nonfinite color");
      t.latch(E_NOTIMPL);
      return;
    }
    const float alpha =
        slot == target_Clear && t.alpha == D2D1_ALPHA_MODE_IGNORE
            ? 1
            : clamp(color[3]) * opacity;
    if (t.gpu) {
      std::array<i32, 4> clip{0, 0, static_cast<i32>(t.width),
                              static_cast<i32>(t.height)};
      if (!t.clips.empty()) {
        const auto &source = t.clips.back();
        const auto edge = [](float value, u32 dimension) {
          return static_cast<i32>(std::ceil(
              std::clamp(value - .5f, 0.f, static_cast<float>(dimension))));
        };
        clip = {edge(source[0], t.width), edge(source[1], t.height),
                edge(source[2], t.width), edge(source[3], t.height)};
      }
      const std::array<float, 4> premultiplied{clamp(color[0]) * alpha,
                                               clamp(color[1]) * alpha,
                                               clamp(color[2]) * alpha, alpha};
      if (!t.gpu->draw(r, premultiplied, slot == target_FillRectangle, clip)) {
        t.latch(D2DERR_RECREATE_TARGET);
        return;
      }
      t.dirty = true;
      return;
    }
    for (u32 y = 0; y < t.height; ++y)
      for (u32 x = 0; x < t.width; ++x) {
        if (!t.clips.empty()) {
          const auto &clip = t.clips.back();
          if (x + .5f < clip[0] || x + .5f >= clip[2] || y + .5f < clip[1] ||
              y + .5f >= clip[3])
            continue;
        }
        float coverage = 1;
        if (slot == target_FillRectangle) {
          if (t.antialias == D2D1_ANTIALIAS_MODE_ALIASED)
            coverage = (x + .5f >= r[0] && x + .5f < r[2] && y + .5f >= r[1] &&
                        y + .5f < r[3])
                           ? 1
                           : 0;
          else
            coverage =
                std::max(0.f, std::min(x + 1.f, r[2]) -
                                  std::max(static_cast<float>(x), r[0])) *
                std::max(0.f, std::min(y + 1.f, r[3]) -
                                  std::max(static_cast<float>(y), r[1]));
        }
        if (coverage <= 0)
          continue;
        float *pixel =
            t.pixels.get() + (static_cast<usize>(y) * t.width + x) * 4;
        const u8 source_alpha = quantize(quantize(alpha) / 255.f * coverage);
        const float inverse = 1.f - source_alpha / 255.f;
        for (usize c = 0; c < 3; ++c) {
          const float value =
              quantize(clamp(color[c]) * alpha) * coverage +
              (slot == target_Clear ? 0 : quantize(pixel[c]) * inverse);
          pixel[c] = quantize(value / 255.f) / 255.f;
        }
        const float result_alpha =
            source_alpha +
            (slot == target_Clear ? 0 : quantize(pixel[3]) * inverse);
        // IGNORE forces opacity for Clear (the source alpha above), not
        // arbitrary untouched/seeded DC bytes or ordinary source-over fills.
        pixel[3] = quantize(result_alpha / 255.f) / 255.f;
      }
    t.dirty = true;
    return;
  }
  default:
    break;
  }
  const auto &method = target_methods[slot];
  unsupported((std::string("ID2D1RenderTarget::") + method.name).c_str(),
              "render-target operation unavailable");
  if (std::string_view(method.result) == "HRESULT") {
    if (std::string_view(method.name).starts_with("Create")) {
      const usize output = m_is_64bit ? method.arguments : method.bytes / 4 - 1;
      if (!write_pointer(m, m.arg(output), 0))
        return;
    }
    m.set_return(E_NOTIMPL);
  } else {
    t.latch(E_NOTIMPL);
    m.set_return(0);
  }
}
void D2DManager::brush_method(Marshaler &m, usize slot) {
  std::lock_guard lock(m_mutex);
  const auto self = m.arg(0);
  auto it = m_resources.find(self);
  if (it == m_resources.end() || !it->second.brush) {
    (void)m.raise_access_violation(false, self);
    return;
  }
  auto &resource = it->second;
  if (resource_identity(m, resource, self, true, slot))
    return;
  switch (slot) {
  case brush_SetOpacity: {
    const auto opacity = m.arg_f32(1);
    if (std::isfinite(opacity))
      resource.opacity = clamp(opacity);
    else {
      unsupported("ID2D1Brush::SetOpacity", "nonfinite opacity");
      resource.target->latch(E_NOTIMPL);
    }
    return;
  }
  case brush_GetOpacity:
    m.set_return_f32(resource.opacity);
    return;
  case brush_SetTransform:
    (void)read_data(m, m.arg(1), resource.transform.data(),
                    sizeof(resource.transform));
    return;
  case brush_GetTransform:
    (void)write_data(m, m.arg(1), resource.transform.data(),
                     sizeof(resource.transform));
    return;
  case brush_SetColor:
    (void)read_data(m, m.arg(1), resource.color.data(), sizeof(resource.color));
    return;
  case brush_GetColor:
    if (write_data(m, m.arg(1), resource.color.data(), sizeof(resource.color)))
      m.set_return(m.arg(1));
    return;
  default:
    unsupported("ID2D1SolidColorBrush", "unknown method");
    resource.target->latch(E_NOTIMPL);
    return;
  }
}
} // namespace vitreo::winapi
