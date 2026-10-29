// SPDX-License-Identifier: LGPL-2.1-only
#include "core/memory/guest_memory.hpp"
#include "d2d1_capabilities_contract.inc"
#include "d2d1_internal.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>
namespace vitreo::winapi {
using namespace d2d1_contract;
namespace {
void unsupported_bitmap(const char *method, const char *reason) {
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(std::string(method) + reason).second)
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d2d1.dll!%s: %s\n", method,
                 reason);
}
struct Scratch {
  GuestMemory &memory;
  GuestAddr address{};
  usize size{};
  Scratch(GuestMemory &memory, usize bytes)
      : memory(memory),
        size((bytes + GUEST_PAGE_SIZE - 1) & ~(GUEST_PAGE_SIZE - 1)) {
    address =
        memory.map_first_fit(0x62000000, size, MemoryProtection::ReadWrite);
  }
  ~Scratch() {
    if (address)
      (void)memory.unmap_region(address, size);
  }
};
struct ScopeExit {
  std::function<void()> action;
  ~ScopeExit() { action(); }
};
} // namespace
GuestAddr D2DManager::allocate_bitmap(GuestAddr factory,
                                      std::shared_ptr<Target> target,
                                      std::shared_ptr<Bitmap> bitmap) {
  const auto base = m_memory.map_first_fit(0x60000000, GUEST_PAGE_SIZE,
                                           MemoryProtection::ReadWrite);
  if (!base)
    return 0;
  const usize width = m_is_64bit ? 8 : 4;
  auto put = [&](GuestAddr at, GuestAddr value) {
    return m_is_64bit ? m_memory.write_u64(at, value).is_ok()
                      : m_memory.write_u32(at, static_cast<u32>(value)).is_ok();
  };
  bool ok = put(base, base + width);
  for (usize i = 0; ok && i < m_bitmap_methods.size(); ++i)
    ok = put(base + width + i * width, m_bitmap_methods[i]);
  if (!ok) {
    (void)m_memory.unmap_region(base, GUEST_PAGE_SIZE);
    return 0;
  }
  Resource resource;
  resource.factory = factory;
  resource.target = std::move(target);
  resource.bitmap = std::move(bitmap);
  m_resources.emplace(base, std::move(resource));
  ++m_factories.at(factory).references;
  return base;
}
void D2DManager::create_bitmap_from_wic(Marshaler &m, GuestAddr self) {
  const GuestAddr source = m.arg(1), properties = m.arg(2), out = m.arg(3);
  // Native Windows faults on null source/output before initializing output.
  u64 source_table = 0;
  if (m_memory
          .read_bytes(source, reinterpret_cast<Byte *>(&source_table),
                      m_is_64bit ? 8 : 4)
          .is_err()) {
    (void)m.raise_access_violation(false, source);
    return;
  }
  if (!write_pointer(m, out, 0))
    return;
  u32 format = DXGI_FORMAT_UNKNOWN, alpha = D2D1_ALPHA_MODE_UNKNOWN;
  float dx = 0, dy = 0;
  if (properties) {
    std::array<u8, bitmap_layout_properties> data{};
    if (!read_data(m, properties, data.data(), data.size()))
      return;
    std::memcpy(&format, data.data() + bitmap_layout_format, 4);
    std::memcpy(&alpha, data.data() + bitmap_layout_alpha, 4);
    std::memcpy(&dx, data.data() + bitmap_layout_dpiX, 4);
    std::memcpy(&dy, data.data() + bitmap_layout_dpiY, 4);
  }
  if (dx == 0 && dy == 0)
    dx = dy = dip_screen_dpi;
  if (!(dx > 0) || !(dy > 0)) {
    m.set_return(E_INVALIDARG);
    return;
  }
  std::shared_ptr<Target> target;
  GuestAddr factory = 0;
  GuestInterfaceCall callback;
  graphics::GraphicsBackendDevice *backend = nullptr;
  {
    std::lock_guard lock(m_mutex);
    auto it = m_resources.find(self);
    if (it == m_resources.end() || it->second.brush || it->second.bitmap) {
      (void)m.raise_access_violation(false, self);
      return;
    }
    target = it->second.target;
    factory = it->second.factory;
    callback = m_guest_interface_call;
    backend = m_backend;
    ++m_factories.at(factory).references;
  }
  ScopeExit pin{[&] {
    std::lock_guard lock(m_mutex);
    release_factory(factory);
  }};
  if (!callback) {
    unsupported_bitmap("ID2D1RenderTarget::CreateBitmapFromWicBitmap",
                       "guest WIC interface bridge unavailable");
    m.set_return(E_NOTIMPL);
    return;
  }
  Scratch scratch(m_memory, 64);
  if (!scratch.address) {
    m.set_return(E_OUTOFMEMORY);
    return;
  }
  const auto base = scratch.address;
  const auto call = [&](u32 slot, std::initializer_list<u64> arguments) {
    return callback(m, source, slot, arguments.begin(), arguments.size());
  };
  u32 hr = call(wic_GetPixelFormat, {source, base});
  if (static_cast<i32>(hr) < 0) {
    m.set_return(hr);
    return;
  }
  std::array<u8, 16> guid{};
  if (!read_data(m, base, guid.data(), guid.size()))
    return;
  if (guid == GUID_WICPixelFormat8bppAlpha) {
    unsupported_bitmap("ID2D1RenderTarget::CreateBitmapFromWicBitmap",
                       "A8 bitmap storage and sampling");
    m.set_return(E_NOTIMPL);
    return;
  }
  u32 source_format = 0, source_alpha = 0;
  if (guid == GUID_WICPixelFormat32bppPBGRA ||
      guid == GUID_WICPixelFormat32bppBGRA ||
      guid == GUID_WICPixelFormat32bppBGR) {
    source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    source_alpha =
        guid == GUID_WICPixelFormat32bppPBGRA ? D2D1_ALPHA_MODE_PREMULTIPLIED
        : guid == GUID_WICPixelFormat32bppBGR ? D2D1_ALPHA_MODE_IGNORE
                                              : D2D1_ALPHA_MODE_STRAIGHT;
  }
  if (guid == GUID_WICPixelFormat32bppPRGBA ||
      guid == GUID_WICPixelFormat32bppRGBA ||
      guid == GUID_WICPixelFormat32bppRGB) {
    source_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source_alpha =
        guid == GUID_WICPixelFormat32bppPRGBA ? D2D1_ALPHA_MODE_PREMULTIPLIED
        : guid == GUID_WICPixelFormat32bppRGB ? D2D1_ALPHA_MODE_IGNORE
                                              : D2D1_ALPHA_MODE_STRAIGHT;
  }
  if (!source_format) {
    m.set_return(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
    return;
  }
  if (alpha == D2D1_ALPHA_MODE_UNKNOWN)
    alpha = source_alpha;
  // Native WIC alpha compatibility precedes DXGI matching; unknown alpha
  // values are rejected after matching. No format/alpha conversion occurs.
  if (alpha == D2D1_ALPHA_MODE_STRAIGHT ||
      (alpha == D2D1_ALPHA_MODE_PREMULTIPLIED &&
       source_alpha == D2D1_ALPHA_MODE_IGNORE)) {
    m.set_return(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
    return;
  }
  if (format == DXGI_FORMAT_UNKNOWN)
    format = source_format;
  if (format != source_format) {
    m.set_return(E_INVALIDARG);
    return;
  }
  if (alpha != D2D1_ALPHA_MODE_PREMULTIPLIED &&
      alpha != D2D1_ALPHA_MODE_IGNORE) {
    m.set_return(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
    return;
  }
  hr = call(wic_GetSize, {source, base + 16, base + 20});
  if (static_cast<i32>(hr) < 0) {
    m.set_return(hr);
    return;
  }
  auto width = m_memory.read_u32(base + 16),
       height = m_memory.read_u32(base + 20);
  if (width.is_err() || height.is_err()) {
    m.set_return(E_POINTER);
    return;
  }
  const u64 bytes = u64(width.value()) * height.value() * 4;
  if (width.value() > std::numeric_limits<u32>::max() / 4 ||
      bytes > std::numeric_limits<u32>::max() - 64) {
    unsupported_bitmap("ID2D1RenderTarget::CreateBitmapFromWicBitmap",
                       "oversized bitmap transport");
    m.set_return(E_NOTIMPL);
    return;
  }
  Scratch pixels(m_memory, static_cast<usize>(bytes) + 64);
  if (!pixels.address) {
    m.set_return(E_OUTOFMEMORY);
    return;
  }
  const std::array<i32, 4> rectangle{0, 0, static_cast<i32>(width.value()),
                                     static_cast<i32>(height.value())};
  if (!write_data(m, pixels.address, rectangle.data(), bitmap_layout_wicrect))
    return;
  hr = call(wic_CopyPixels, {source, pixels.address, width.value() * 4, bytes,
                             pixels.address + 64});
  if (static_cast<i32>(hr) < 0) {
    m.set_return(hr);
    return;
  }
  if (!width.value()) {
    m.set_return(E_INVALIDARG);
    return;
  }
  auto bitmap = std::make_shared<Bitmap>();
  bitmap->width = width.value();
  bitmap->height = height.value();
  bitmap->format = format;
  bitmap->alpha = alpha;
  bitmap->dpi_x = dx;
  bitmap->dpi_y = dy;
  bitmap->bytes.resize(static_cast<usize>(bytes));
  if (bytes && !read_data(m, pixels.address + 64, bitmap->bytes.data(),
                          bitmap->bytes.size()))
    return;
  if (target->gpu && bytes) {
    graphics::BackendTexture2DDesc desc{};
    desc.width = bitmap->width;
    desc.height = bitmap->height;
    desc.shader_resource = true;
    desc.pixel_format = format == DXGI_FORMAT_B8G8R8A8_UNORM
                            ? graphics::PixelFormat::BGRA8Unorm
                            : graphics::PixelFormat::RGBA8Unorm;
    bitmap->texture =
        backend ? backend->create_texture_2d(desc, bitmap->bytes.data(),
                                             bitmap->width * 4)
                : nullptr;
    if (!bitmap->texture || !bitmap->texture->valid()) {
      unsupported_bitmap(
          "ID2D1RenderTarget::CreateBitmapFromWicBitmap",
          "backend texture allocation/format failure detail unavailable");
      m.set_return(E_NOTIMPL);
      return;
    }
  }
  {
    std::lock_guard lock(m_mutex);
    auto current = m_resources.find(self);
    if (current == m_resources.end() || current->second.target != target) {
      unsupported_bitmap("ID2D1RenderTarget::CreateBitmapFromWicBitmap",
                         "target released during guest callback");
      m.set_return(E_NOTIMPL);
      return;
    }
    const auto object = allocate_bitmap(factory, target, bitmap);
    if (!object) {
      m.set_return(E_OUTOFMEMORY);
      return;
    }
    if (!write_pointer(m, out, object)) {
      m_resources.erase(object);
      (void)m_memory.unmap_region(object, GUEST_PAGE_SIZE);
      release_factory(factory);
      return;
    }
  }
  m.set_return(S_OK);
}
void D2DManager::draw_bitmap(Marshaler &m, Resource &resource) {
  auto &t = *resource.target;
  if (!t.drawing) {
    t.latch(D2DERR_WRONG_STATE);
    return;
  }
  auto it = m_resources.find(m.arg(1));
  if (it == m_resources.end() || !it->second.bitmap) {
    (void)m.raise_access_violation(false, m.arg(1));
    return;
  }
  if (it->second.factory != resource.factory) {
    t.latch(D2DERR_WRONG_FACTORY);
    return;
  }
  if (bool(it->second.target->gpu) != bool(t.gpu)) {
    t.latch(D2DERR_WRONG_RESOURCE_DOMAIN);
    return;
  }
  auto bitmap = it->second.bitmap;
  const auto fail = [&](const char *reason) {
    unsupported_bitmap("ID2D1RenderTarget::DrawBitmap", reason);
    t.latch(E_NOTIMPL);
  };
  const float opacity = m.arg_f32(3);
  const u32 interpolation = static_cast<u32>(m.arg(4));
  if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
    fail("nonfinite/out-of-range opacity");
    return;
  }
  if (interpolation != D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR &&
      interpolation != D2D1_BITMAP_INTERPOLATION_MODE_LINEAR) {
    fail("unknown bitmap interpolation mode");
    return;
  }
  std::array<float, 4> source{0, 0,
                              bitmap->width * dip_screen_dpi / bitmap->dpi_x,
                              bitmap->height * dip_screen_dpi / bitmap->dpi_y};
  if (m.arg(5) && !read_data(m, m.arg(5), source.data(), sizeof(source)))
    return;
  std::array<float, 4> destination{0, 0, source[2] - source[0],
                                   source[3] - source[1]};
  if (m.arg(2) &&
      !read_data(m, m.arg(2), destination.data(), sizeof(destination)))
    return;
  for (float value : source)
    if (!std::isfinite(value)) {
      fail("nonfinite source rectangle");
      return;
    }
  for (float value : destination)
    if (!std::isfinite(value)) {
      fail("nonfinite destination rectangle");
      return;
    }
  if (source[2] <= source[0] || source[3] <= source[1] ||
      destination[2] <= destination[0] || destination[3] <= destination[1] ||
      opacity == 0)
    return;
  for (float value : t.transform)
    if (!std::isfinite(value)) {
      fail("nonfinite transform");
      return;
    }
  if (t.transform[1] != 0 || t.transform[2] != 0 || t.transform[0] <= 0 ||
      t.transform[3] <= 0) {
    fail("rotated/sheared/reflected bitmap transform");
    return;
  }
  source = {source[0] * bitmap->dpi_x / dip_screen_dpi,
            source[1] * bitmap->dpi_y / dip_screen_dpi,
            source[2] * bitmap->dpi_x / dip_screen_dpi,
            source[3] * bitmap->dpi_y / dip_screen_dpi};
  if (source[0] < 0 || source[1] < 0 || source[2] > bitmap->width ||
      source[3] > bitmap->height) {
    fail("source rectangle outside bitmap");
    return;
  }
  destination = {(destination[0] * t.transform[0] + t.transform[4]) * t.dpi_x /
                     dip_screen_dpi,
                 (destination[1] * t.transform[3] + t.transform[5]) * t.dpi_y /
                     dip_screen_dpi,
                 (destination[2] * t.transform[0] + t.transform[4]) * t.dpi_x /
                     dip_screen_dpi,
                 (destination[3] * t.transform[3] + t.transform[5]) * t.dpi_y /
                     dip_screen_dpi};
  for (float value : destination)
    if (!std::isfinite(value) || value != std::floor(value)) {
      fail("fractional bitmap destination rasterization");
      return;
    }
  if (!t.width || !t.height || !bitmap->width || !bitmap->height)
    return;
  std::array<i32, 4> clip{0, 0, static_cast<i32>(t.width),
                          static_cast<i32>(t.height)};
  if (!t.clips.empty()) {
    const auto &c = t.clips.back();
    clip = {static_cast<i32>(std::ceil(c[0] - .5f)),
            static_cast<i32>(std::ceil(c[1] - .5f)),
            static_cast<i32>(std::ceil(c[2] - .5f)),
            static_cast<i32>(std::ceil(c[3] - .5f))};
  }
  if (t.gpu) {
    if (!bitmap->texture) {
      fail("hardware bitmap texture unavailable");
      return;
    }
    const std::array<float, 4> uv{
        source[0] / bitmap->width, source[1] / bitmap->height,
        source[2] / bitmap->width, source[3] / bitmap->height};
    t.pending_bitmaps.push_back(bitmap);
    if (!t.gpu->draw_bitmap(
            *bitmap->texture, destination, uv, opacity,
            bitmap->alpha == D2D1_ALPHA_MODE_IGNORE,
            interpolation == D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, clip)) {
      t.latch(D2DERR_RECREATE_TARGET);
      return;
    }
    t.dirty = true;
    return;
  }
  const auto pixel = [&](i32 x, i32 y) {
    x = std::clamp<i32>(x, 0, bitmap->width - 1);
    y = std::clamp<i32>(y, 0, bitmap->height - 1);
    const auto *p = bitmap->bytes.data() + (usize(y) * bitmap->width + x) * 4;
    return std::array<float, 4>{
        float(p[bitmap->format == DXGI_FORMAT_B8G8R8A8_UNORM ? 2 : 0]) / 255,
        p[1] / 255.f,
        float(p[bitmap->format == DXGI_FORMAT_B8G8R8A8_UNORM ? 0 : 2]) / 255,
        bitmap->alpha == D2D1_ALPHA_MODE_IGNORE ? 1.f : p[3] / 255.f};
  };
  const auto quantized = [](float value) {
    const float scaled = std::clamp(value, 0.f, 1.f) * 255;
    const auto lower = static_cast<u32>(scaled);
    const auto fraction = scaled - lower;
    return (lower + (fraction > .5f || (fraction == .5f && (lower & 1)))) /
           255.f;
  };
  for (u32 y = 0; y < t.height; ++y)
    for (u32 x = 0; x < t.width; ++x) {
      if (x + .5f < destination[0] || x + .5f >= destination[2] ||
          y + .5f < destination[1] || y + .5f >= destination[3] ||
          i32(x) < clip[0] || i32(x) >= clip[2] || i32(y) < clip[1] ||
          i32(y) >= clip[3])
        continue;
      const float sx = source[0] + (x + .5f - destination[0]) *
                                       (source[2] - source[0]) /
                                       (destination[2] - destination[0]),
                  sy = source[1] + (y + .5f - destination[1]) *
                                       (source[3] - source[1]) /
                                       (destination[3] - destination[1]);
      std::array<float, 4> sampled;
      if (interpolation == D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR)
        sampled = pixel(std::floor(sx), std::floor(sy));
      else {
        const auto ix = static_cast<i32>(std::floor(sx - .5f)),
                   iy = static_cast<i32>(std::floor(sy - .5f));
        const float fx = sx - .5f - ix, fy = sy - .5f - iy;
        const auto a = pixel(ix, iy), b = pixel(ix + 1, iy),
                   c = pixel(ix, iy + 1), d = pixel(ix + 1, iy + 1);
        for (usize channel = 0; channel < 4; ++channel)
          sampled[channel] =
              (a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
              (c[channel] * (1 - fx) + d[channel] * fx) * fy;
      }
      for (float &channel : sampled)
        channel = quantized(channel * opacity);
      float *destination_pixel = t.pixels.get() + (usize(y) * t.width + x) * 4;
      for (usize channel = 0; channel < 4; ++channel)
        destination_pixel[channel] =
            sampled[channel] + destination_pixel[channel] * (1 - sampled[3]);
    }
  t.dirty = true;
}
void D2DManager::bitmap_method(Marshaler &m, usize slot) {
  std::lock_guard lock(m_mutex);
  const auto self = m.arg(0);
  auto it = m_resources.find(self);
  if (it == m_resources.end() || !it->second.bitmap) {
    (void)m.raise_access_violation(false, self);
    return;
  }
  auto &resource = it->second;
  if (resource_identity(m, resource, self, false, slot))
    return;
  auto &bitmap = *resource.bitmap;
  switch (slot) {
  case bitmap_GetSize: {
    const float result[2] = {bitmap.width * dip_screen_dpi / bitmap.dpi_x,
                             bitmap.height * dip_screen_dpi / bitmap.dpi_y};
    if (write_data(m, m.arg(1), result, sizeof(result)))
      m.set_return(m.arg(1));
    return;
  }
  case bitmap_GetPixelSize: {
    const u32 result[2] = {bitmap.width, bitmap.height};
    if (write_data(m, m.arg(1), result, sizeof(result)))
      m.set_return(m.arg(1));
    return;
  }
  case bitmap_GetPixelFormat: {
    const u32 result[2] = {bitmap.format, bitmap.alpha};
    if (write_data(m, m.arg(1), result, sizeof(result)))
      m.set_return(m.arg(1));
    return;
  }
  case bitmap_GetDpi:
    if (!write_data(m, m.arg(1), &bitmap.dpi_x, 4))
      return;
    (void)write_data(m, m.arg(2), &bitmap.dpi_y, 4);
    return;
  default: {
    const auto method =
        std::string("ID2D1Bitmap::") + bitmap_methods[slot].name;
    unsupported_bitmap(method.c_str(), "bitmap update method unsupported");
    m.set_return(E_NOTIMPL);
    return;
  }
  }
}
} // namespace vitreo::winapi
