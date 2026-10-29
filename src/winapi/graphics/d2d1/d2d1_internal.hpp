// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "d2d1.hpp"
#include "graphics/common/hardware_solid_surface.hpp"
#include "graphics/contracts/backend_texture_2d.hpp"
#include "winapi/kernel32/handle.hpp"
namespace vitreo::winapi {
struct D2DManager::GeometryState {
 struct Command {usize method;u32 flags;std::vector<u8> bytes;};
 bool opened{},closed{},figure{};u32 error{d2d1_contract::S_OK},fill{d2d1_contract::D2D1_FILL_MODE_ALTERNATE},flags{},figures{},segments{};bool closed_edge{};
 std::vector<Command> commands;
 void latch(u32 hr){if(error==d2d1_contract::S_OK)error=hr;}
};
struct D2DManager::Bitmap {
  u32 width{}, height{}, format{}, alpha{};
  float dpi_x{96}, dpi_y{96};
  std::vector<u8> bytes;
  std::unique_ptr<graphics::BackendTexture2D> texture;
};
struct D2DManager::Target {
  Handle dc{0};
  std::array<i32, 4> rectangle{};
  u32 width{0}, height{0};
  u32 alpha{0}, usage{0};
  float dpi_x{0}, dpi_y{0};
  u32 antialias{0}, text_antialias{0};
  GuestAddr text_rendering_params{0};  // held IDWriteRenderingParams reference
  std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
  std::unique_ptr<float[]> pixels;
  // Destroy GPU helper/completion barrier before releasing sampled textures.
  std::vector<std::shared_ptr<Bitmap>> pending_bitmaps;
  std::unique_ptr<graphics::HardwareSolidSurface> gpu;
  std::vector<std::array<float, 4>> clips;
  bool drawing{false}, dirty{false};
  u32 error{d2d1_contract::S_OK};
  u64 tag1{0}, tag2{0}, error_tag1{0}, error_tag2{0};
  void latch(u32 value) {
    if (error == d2d1_contract::S_OK) {
      error = value;
      error_tag1 = tag1;
      error_tag2 = tag2;
    }
  }
};
} // namespace vitreo::winapi
