// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "d2d1_bitmap_contract.inc"
#include "d2d1_contract.inc"
#include "d2d1_geometry_contract.inc"
#include "d2d1_resource_contract.inc"
#include "winapi/core/thunk_registry.hpp"
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vitreo/common.hpp>
namespace vitreo {
class GuestMemory;
}
namespace vitreo::graphics {
class GraphicsBackendDevice;
}
namespace vitreo::winapi {
class Marshaler;
class GdiManager;
class DWriteManager;
class D2DManager {
public:
  D2DManager(GuestMemory &, bool is_64bit, GdiManager * = nullptr);
  ~D2DManager();
  bool set_backend(graphics::GraphicsBackendDevice *);
  using GuestInterfaceCall =
      std::function<u32(Marshaler &, GuestAddr, u32, const u64 *, usize)>;
  void set_guest_interface_call(GuestInterfaceCall);
  void set_dpi_provider(std::function<std::pair<float, float>()>);
  bool bind_thunks(const ThunkRegistry &);
  void create_factory(Marshaler &);
  void factory_method(Marshaler &, usize slot);
  void target_method(Marshaler &, usize slot);
  void target_method_locked(Marshaler &, usize slot);
  void brush_method(Marshaler &, usize slot);
  void bitmap_method(Marshaler &, usize slot);
  void geometry_method(Marshaler &, usize slot, bool sink);
  /// DirectWrite formats the text DrawText / DrawTextLayout render.
  void set_dwrite(DWriteManager *dwrite) { m_dwrite = dwrite; }

private:
  struct Factory {
    u32 references{1};
    u32 threading;
    float dpi_x, dpi_y;
  };
  struct Target;
  struct Bitmap;
  struct Resource {
    u32 references{1};
    GuestAddr factory{0};
    bool brush{false};
    std::shared_ptr<Target> target;
    std::array<float, 4> color{};
    float opacity{1};
    std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
    std::shared_ptr<Bitmap> bitmap;
  };
  struct GeometryState;
  struct GeometryObject {
    u32 references{1};GuestAddr factory{};bool sink{};
    std::shared_ptr<GeometryState> state;
  };
  std::unordered_map<GuestAddr,GeometryObject> m_geometries;
  std::array<GuestAddr,std::size(d2d1_contract::path_methods)> m_path_methods{};
  std::array<GuestAddr,std::size(d2d1_contract::sink_methods)> m_sink_methods{};
  GuestAddr allocate_geometry(GuestAddr,bool,std::shared_ptr<GeometryState>);
  void create_path_geometry(Marshaler&,GuestAddr);
  DWriteManager *m_dwrite{nullptr};
  GuestMemory &m_memory;
  bool m_is_64bit;
  bool m_bound{false};
  GdiManager *m_gdi;
  graphics::GraphicsBackendDevice *m_backend{nullptr};
  GuestInterfaceCall m_guest_interface_call;
  std::mutex m_mutex;
  std::unordered_map<GuestAddr, Factory> m_factories;
  std::array<GuestAddr, std::size(d2d1_contract::factory_methods)> m_methods{};
  std::array<GuestAddr, std::size(d2d1_contract::target_methods)>
      m_target_methods{};
  std::array<GuestAddr, std::size(d2d1_contract::brush_methods)>
      m_brush_methods{};
  std::array<GuestAddr, std::size(d2d1_contract::bitmap_methods)>
      m_bitmap_methods{};
  std::unordered_map<GuestAddr, Resource> m_resources;
  std::array<bool, std::size(d2d1_contract::factory_methods)> m_warned{};
  std::function<std::pair<float, float>()> m_dpi;
  void create_dc_target(Marshaler &, GuestAddr factory);
  void create_bitmap_from_wic(Marshaler &, GuestAddr target);
  void draw_bitmap(Marshaler &, Resource &);
  void draw_geometry(Marshaler &,Resource&,bool stroke);
  /// Coverage fill of device-space edges with a premultiplied colour, on
  /// the target's GPU surface or CPU pixels (geometry, text decorations).
  void fill_device_edges(Target &, const std::vector<std::array<float, 4>> &edges,
                         const std::array<float, 4> &premultiplied, bool alternate,
                         bool antialias);
  /// DrawText / DrawTextLayout / DrawGlyphRun (d2d1_text.cpp). Entered
  /// without m_mutex: DirectWrite formats first under its own lock.
  void draw_text(Marshaler &, usize slot);
  struct TextRun;
  void render_text(Marshaler &, Target &, Resource &, GuestAddr brush,
                   const std::vector<TextRun> &, u32 options);
  GuestAddr allocate_bitmap(GuestAddr factory, std::shared_ptr<Target>,
                            std::shared_ptr<Bitmap>);
  GuestAddr allocate_resource(GuestAddr factory, bool brush,
                              std::shared_ptr<Target>);
  void release_factory(GuestAddr factory);
  bool resource_identity(Marshaler &, Resource &, GuestAddr self, bool brush,
                         usize slot);
  bool read_data(Marshaler &, GuestAddr, void *, usize);
  bool write_data(Marshaler &, GuestAddr, const void *, usize);
  bool write_pointer(Marshaler &, GuestAddr, GuestAddr);
  bool read_iid(Marshaler &, GuestAddr, std::array<u8, 16> &);
};
DllExports create_d2d1_thunks(D2DManager &);
} // namespace vitreo::winapi
