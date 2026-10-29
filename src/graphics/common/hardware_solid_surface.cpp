// SPDX-License-Identifier: LGPL-2.1-only
#include "hardware_solid_surface.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_render_pass.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <new>
#include <string>
namespace vitreo::graphics {
namespace {
constexpr const char *vertex_source = R"(
#include <metal_stdlib>
using namespace metal;
struct Parameters { float4 rectangle; float2 size; float2 reserved; };
struct Position { float4 position [[position]]; };
vertex Position vertex_main(uint id [[vertex_id]], constant Parameters& p [[buffer(0)]]) {
  constexpr float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
  const float2 pixel=mix(p.rectangle.xy,p.rectangle.zw,corners[id]);
  Position result;
  result.position=float4(pixel.x*2.0f/p.size.x-1.0f,1.0f-pixel.y*2.0f/p.size.y,0,1);
  return result;
}
)";
constexpr const char *fragment_source = R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 fragment_main(constant float4& color [[buffer(0)]]) {
  // UNORM8 premultiplied source precision before hardware source-over blend.
  return rint(clamp(color,0.0f,1.0f)*255.0f)/255.0f;
}
)";
constexpr const char *bitmap_vertex_source = R"(
#include <metal_stdlib>
using namespace metal;
struct Parameters {float4 rectangle;float4 uv;float2 size;float2 reserved;};
struct Position {float4 position [[position]];float2 uv [[user(locn0)]];};
vertex Position vertex_main(uint id [[vertex_id]],constant Parameters& p [[buffer(0)]]){
 constexpr float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
 float2 pixel=mix(p.rectangle.xy,p.rectangle.zw,corners[id]);Position result;
 result.position=float4(pixel.x*2/p.size.x-1,1-pixel.y*2/p.size.y,0,1);
 result.uv=mix(p.uv.xy,p.uv.zw,corners[id]);return result;
}
)";
constexpr const char *bitmap_fragment_source = R"(
#include <metal_stdlib>
using namespace metal;
struct Position {float4 position [[position]];float2 uv [[user(locn0)]];};
fragment float4 fragment_main(Position in [[stage_in]],constant float2& p [[buffer(0)]],texture2d<float> image [[texture(0)]],sampler image_sampler [[sampler(0)]]){
 float4 pixel=image.sample(image_sampler,in.uv);if(p.y!=0)pixel.a=1;
 return rint(clamp(pixel*p.x,0.0f,1.0f)*255.0f)/255.0f;
}
)";
constexpr const char *path_fragment_source = R"(
#include <metal_stdlib>
using namespace metal;
struct Position {float4 position [[position]];};
struct Edges {float4 color;uint count;uint alternate;uint samples;uint reserved;float4 edges[192];};
fragment float4 fragment_main(Position in [[stage_in]],constant Edges& p [[buffer(0)]]){
 uint covered=0;float2 base=floor(in.position.xy);
 for(uint y=0;y<p.samples;++y)for(uint x=0;x<p.samples;++x){float2 point=base+(float2(x,y)+0.5f)/float(p.samples);int winding=0;
  for(uint i=0;i<p.count;++i){float4 e=p.edges[i];if((e.y<=point.y&&e.w>point.y)||(e.w<=point.y&&e.y>point.y)){
   float at=e.x+(point.y-e.y)*(e.z-e.x)/(e.w-e.y);if(at>point.x)winding+=e.w>e.y?1:-1;
  }}if(p.alternate?(winding%2!=0):(winding!=0))++covered;
 }
 if(covered==0)discard_fragment();float coverage=float(covered)/float(p.samples*p.samples);
 return rint(clamp(p.color,0.0f,1.0f)*255.0f)/255.0f*coverage;
}
)";
std::unique_ptr<BackendPipeline> pipeline(GraphicsBackendDevice &device,
                                          bool blend, bool bitmap = false,
                                          bool path = false) {
  BackendPipelineDesc desc{};
  desc.vertex_shader_msl = bitmap ? bitmap_vertex_source : vertex_source;
  desc.fragment_shader_msl = path     ? path_fragment_source
                             : bitmap ? bitmap_fragment_source
                                      : fragment_source;
  desc.color_format = PixelFormat::BGRA8Unorm;
  desc.vertex_math_mode = desc.fragment_math_mode = BackendMathMode::Safe;
  desc.blend_enabled = blend;
  desc.source_blend = desc.source_blend_alpha = BackendBlendFactor::One;
  desc.destination_blend = desc.destination_blend_alpha =
      BackendBlendFactor::OneMinusSourceAlpha;
  desc.cache_key = std::hash<std::string>{}(desc.vertex_shader_msl +
                                            desc.fragment_shader_msl +
                                            (blend ? "blend" : "copy"));
  return device.create_pipeline(desc);
}
} // namespace
HardwareSolidSurface::HardwareSolidSurface(GraphicsBackendDevice &device)
    : m_device(device) {}
std::unique_ptr<HardwareSolidSurface>
HardwareSolidSurface::create(GraphicsBackendDevice &device) {
  if (!device.available())
    return nullptr;
  auto result =
      std::unique_ptr<HardwareSolidSurface>(new HardwareSolidSurface(device));
  result->m_copy = pipeline(device, false);
  result->m_blend = pipeline(device, true);
  result->m_bitmap = pipeline(device, true, true);
  result->m_path = pipeline(device, true, false, true);
  if (!result->m_copy || !result->m_copy->valid() || !result->m_blend ||
      !result->m_blend->valid() || !result->m_bitmap ||
      !result->m_bitmap->valid() || !result->m_path || !result->m_path->valid())
    return nullptr;
  return result;
}
HardwareSolidSurface::~HardwareSolidSurface() {
  if (m_pending && !m_device.wait_idle())
    std::fprintf(
        stderr,
        "[vitreo] GPU solid surface completion failed during disposal\n");
}
bool HardwareSolidSurface::resize(u32 width, u32 height) {
  if (!m_device.available() ||
      width > std::numeric_limits<unsigned>::max() / 4 ||
      height > static_cast<u32>(std::numeric_limits<i32>::max()))
    return false;
  if (m_pending) {
    if (!m_device.wait_idle())
      return false;
    m_pending = false;
  }
  if (!width || !height) {
    m_texture.reset();
    m_readback.reset();
    m_readback_size = 0;
    m_width = width;
    m_height = height;
    return true;
  }
  BackendTexture2DDesc desc{};
  desc.width = width;
  desc.height = height;
  desc.pixel_format = PixelFormat::BGRA8Unorm;
  desc.render_target = true;
  auto texture = m_device.create_texture_2d(desc, nullptr, 0);
  if (!texture || !texture->valid())
    return false;
  const u64 bytes = static_cast<u64>(width) * height * 4;
  if (bytes > std::numeric_limits<usize>::max())
    return false;
  auto staging =
      std::unique_ptr<u8[]>(new (std::nothrow) u8[static_cast<usize>(bytes)]);
  if (!staging)
    return false;
  m_texture = std::move(texture);
  m_readback = std::move(staging);
  m_readback_size = static_cast<usize>(bytes);
  m_width = width;
  m_height = height;
  return true;
}
bool HardwareSolidSurface::draw(const std::array<float, 4> &rectangle,
                                const std::array<float, 4> &color, bool blend,
                                const std::array<i32, 4> &clip) {
  if (!m_device.available())
    return false;
  if (!m_width || !m_height)
    return true;
  if (!m_texture || !m_texture->valid())
    return false;
  std::array<i32, 4> scissor{std::max(0, clip[0]), std::max(0, clip[1]),
                             std::min(static_cast<i32>(m_width), clip[2]),
                             std::min(static_cast<i32>(m_height), clip[3])};
  if (scissor[2] <= scissor[0] || scissor[3] <= scissor[1] ||
      rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
    return true;
  BackendRenderPassDesc desc{};
  desc.color_targets[0] = m_texture.get();
  desc.preserve_color_contents = true;
  auto pass = m_device.create_render_pass_standalone(desc);
  if (!pass || !pass->valid())
    return false;
  struct Parameters {
    std::array<float, 4> rectangle;
    std::array<float, 2> size;
    std::array<float, 2> reserved;
  };
  const Parameters parameters{
      rectangle,
      {static_cast<float>(m_width), static_cast<float>(m_height)},
      {0, 0}};
  BackendDrawCall call{};
  call.pipeline = blend ? m_blend.get() : m_copy.get();
  call.vertex_count = 6;
  call.topology = BackendPrimitiveTopology::TriangleList;
  call.vertex_constants = &parameters;
  call.vertex_constants_size = sizeof(parameters);
  call.fragment_constants = color.data();
  call.fragment_constants_size = sizeof(color);
  call.viewport = {
      0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
  call.scissor_enabled = true;
  call.scissor = scissor;
  call.cull_mode = BackendCullMode::None;
  call.depth_test_enabled = call.depth_write_enabled = false;
  const bool drawn = pass->draw(call);
  const bool ended = pass->end();
  // Even failed submissions can contain recorded GPU work; wrappers remain
  // owned until a completion barrier (readback, resize or destruction).
  m_pending = true;
  return drawn && ended;
}
bool HardwareSolidSurface::draw_edges(
    std::span<const std::array<float, 4>> edges,
    const std::array<float, 4> &color, bool alternate, bool antialias,
    const std::array<i32, 4> &clip) {
  if (edges.size() > 192 || !m_device.available() || !m_path)
    return false;
  if (edges.empty() || !m_width || !m_height)
    return true;
  if (!m_texture)
    return false;
  std::array<i32, 4> scissor{std::max(0, clip[0]), std::max(0, clip[1]),
                             std::min(i32(m_width), clip[2]),
                             std::min(i32(m_height), clip[3])};
  if (scissor[2] <= scissor[0] || scissor[3] <= scissor[1])
    return true;
  std::array<float, 4> rectangle{edges[0][0], edges[0][1], edges[0][0],
                                 edges[0][1]};
  for (const auto &e : edges) {
    rectangle[0] = std::min({rectangle[0], e[0], e[2]});
    rectangle[1] = std::min({rectangle[1], e[1], e[3]});
    rectangle[2] = std::max({rectangle[2], e[0], e[2]});
    rectangle[3] = std::max({rectangle[3], e[1], e[3]});
  }
  rectangle = {std::floor(rectangle[0]), std::floor(rectangle[1]),
               std::ceil(rectangle[2]), std::ceil(rectangle[3])};
  struct Vertex {
    std::array<float, 4> rectangle;
    std::array<float, 2> size, reserved;
  };
  const Vertex vertex{rectangle, {float(m_width), float(m_height)}, {}};
  struct Fragment {
    std::array<float, 4> color;
    u32 count, alternate, samples, reserved;
    std::array<std::array<float, 4>, 192> edges;
  };
  Fragment fragment{};
  fragment.color = color;
  fragment.count = edges.size();
  fragment.alternate = alternate;
  fragment.samples = antialias ? 4 : 1;
  std::copy(edges.begin(), edges.end(), fragment.edges.begin());
  BackendRenderPassDesc desc{};
  desc.color_targets[0] = m_texture.get();
  desc.preserve_color_contents = true;
  auto pass = m_device.create_render_pass_standalone(desc);
  if (!pass || !pass->valid())
    return false;
  BackendDrawCall call{};
  call.pipeline = m_path.get();
  call.vertex_count = 6;
  call.topology = BackendPrimitiveTopology::TriangleList;
  call.vertex_constants = &vertex;
  call.vertex_constants_size = sizeof(vertex);
  call.fragment_constants = &fragment;
  call.fragment_constants_size = sizeof(fragment);
  call.viewport = {0, 0, float(m_width), float(m_height), 0, 1};
  call.scissor_enabled = true;
  call.scissor = scissor;
  call.cull_mode = BackendCullMode::None;
  call.depth_test_enabled = call.depth_write_enabled = false;
  const bool drawn = pass->draw(call), ended = pass->end();
  m_pending = true;
  return drawn && ended;
}
bool HardwareSolidSurface::draw_bitmap(const BackendTexture2D &image,
                                       const std::array<float, 4> &rectangle,
                                       const std::array<float, 4> &uv,
                                       float opacity, bool ignore_alpha,
                                       bool linear,
                                       const std::array<i32, 4> &clip) {
  if (!m_device.available() || !image.valid())
    return false;
  if (!m_width || !m_height)
    return true;
  if (!m_texture || !m_texture->valid())
    return false;
  std::array<i32, 4> scissor{std::max(0, clip[0]), std::max(0, clip[1]),
                             std::min(static_cast<i32>(m_width), clip[2]),
                             std::min(static_cast<i32>(m_height), clip[3])};
  if (scissor[2] <= scissor[0] || scissor[3] <= scissor[1])
    return true;
  BackendRenderPassDesc desc{};
  desc.color_targets[0] = m_texture.get();
  desc.preserve_color_contents = true;
  auto pass = m_device.create_render_pass_standalone(desc);
  if (!pass || !pass->valid())
    return false;
  struct Parameters {
    std::array<float, 4> rectangle, uv;
    std::array<float, 2> size, reserved;
  };
  const Parameters parameters{
      rectangle, uv, {float(m_width), float(m_height)}, {0, 0}};
  const std::array<float, 2> pixel_parameters{opacity,
                                              ignore_alpha ? 1.f : 0.f};
  BackendDrawCall call{};
  call.pipeline = m_bitmap.get();
  call.vertex_count = 6;
  call.topology = BackendPrimitiveTopology::TriangleList;
  call.vertex_constants = &parameters;
  call.vertex_constants_size = sizeof(parameters);
  call.fragment_constants = pixel_parameters.data();
  call.fragment_constants_size = sizeof(pixel_parameters);
  call.textures[0] = &image;
  call.samplers[0].min_filter = call.samplers[0].mag_filter =
      linear ? BackendFilter::Linear : BackendFilter::Nearest;
  call.samplers[0].address_u = call.samplers[0].address_v =
      BackendAddressMode::Clamp;
  call.samplers[0].mipmaps_enabled = false;
  call.viewport = {0, 0, float(m_width), float(m_height), 0, 1};
  call.scissor_enabled = true;
  call.scissor = scissor;
  call.cull_mode = BackendCullMode::None;
  call.depth_test_enabled = call.depth_write_enabled = false;
  const bool drawn = pass->draw(call), ended = pass->end();
  m_pending = true;
  return drawn && ended;
}
std::span<u8> HardwareSolidSurface::interop_buffer() noexcept {
  return {m_readback.get(), m_readback_size};
}
bool HardwareSolidSurface::upload_dc_pixels() {
  if (!m_device.available())
    return false;
  if (!m_readback_size)
    return true;
  if (!m_texture || !m_texture->update(0, m_readback.get(), m_width * 4))
    return false;
  m_pending = true;
  return true;
}
bool HardwareSolidSurface::readback(std::span<u8> pixels) {
  const u64 needed = static_cast<u64>(m_width) * m_height * 4;
  if (pixels.size() != needed)
    return false;
  if (!needed)
    return true;
  if (!m_device.available() || !m_texture ||
      !m_texture->readback(0, pixels.data(), m_width * 4))
    return false;
  m_pending = false;
  return true;
}
bool HardwareSolidSurface::readback(std::span<const u8> &completed_pixels) {
  completed_pixels = {};
  if (!readback(std::span<u8>(m_readback.get(), m_readback_size)))
    return false;
  completed_pixels = std::span<const u8>(m_readback.get(), m_readback_size);
  return true;
}
} // namespace vitreo::graphics
