// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <array>
#include <memory>
#include <span>
#include <vitreo/common.hpp>
namespace vitreo::graphics {
class GraphicsBackendDevice;
class BackendPipeline;
class BackendTexture2D;
// A backend-owned GPU texture with actual solid-color rasterization. Inputs
// are geometry/uniforms, never a CPU-rendered image. Readback is a completion
// barrier, for frontends that must publish into CPU-addressable DC storage.
class HardwareSolidSurface {
public:
  static std::unique_ptr<HardwareSolidSurface> create(GraphicsBackendDevice &);
  ~HardwareSolidSurface();
  HardwareSolidSurface(const HardwareSolidSurface &) = delete;
  HardwareSolidSurface &operator=(const HardwareSolidSurface &) = delete;
  bool resize(u32 width, u32 height);
  bool draw_bitmap(const BackendTexture2D &,
                   const std::array<float, 4> &rectangle,
                   const std::array<float, 4> &uv, float opacity,
                   bool ignore_alpha, bool linear,
                   const std::array<i32, 4> &scissor);
  bool draw(const std::array<float, 4> &rectangle,
            const std::array<float, 4> &premultiplied_color, bool blend,
            const std::array<i32, 4> &scissor);
  bool draw_edges(std::span<const std::array<float,4>> edges,
                  const std::array<float,4>& premultiplied_color,
                  bool alternate,bool antialias,const std::array<i32,4>& scissor);
  // Raw existing-DC data initializes a new drawing batch. This is interop
  // destination storage, not a CPU-rendered primitive image.
  std::span<u8> interop_buffer() noexcept;
  bool upload_dc_pixels();
  bool readback(std::span<u8> bgra);
  bool readback(std::span<const u8> &completed_pixels);

private:
  explicit HardwareSolidSurface(GraphicsBackendDevice &);
  GraphicsBackendDevice &m_device;
  std::unique_ptr<BackendPipeline> m_copy, m_blend, m_bitmap, m_path;
  std::unique_ptr<BackendTexture2D> m_texture;
  u32 m_width{0}, m_height{0};
  bool m_pending{false};
  std::unique_ptr<u8[]> m_readback;
  usize m_readback_size{0};
};
} // namespace vitreo::graphics
