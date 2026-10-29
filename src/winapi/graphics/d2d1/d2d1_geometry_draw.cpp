// SPDX-License-Identifier: LGPL-2.1-only
#include "d2d1_capabilities_contract.inc"
#include "d2d1_internal.hpp"
#include "graphics/common/path_geometry_math.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
namespace vitreo::winapi {
using namespace d2d1_contract;
namespace {
void gap(const char *what) {
  static std::mutex mutex;
  static std::set<std::string> once;
  std::lock_guard lock(mutex);
  if (once.insert(what).second)
    std::fprintf(
        stderr,
        "[vitreo] UNIMPLEMENTED d2d1.dll!ID2D1RenderTarget::Geometry: %s\n",
        what);
}
void math_gap(const graphics::PathMathFailure &f, bool stroke) {
  static std::mutex mutex;
  static std::set<std::pair<graphics::PathFailureReason, bool>> once;
  std::lock_guard lock(mutex);
  if (!once.insert({f.reason, stroke}).second)
    return;
  std::fprintf(
      stderr,
      "[vitreo] UNIMPLEMENTED d2d1.dll!ID2D1RenderTarget::%s: path-math=%s",
      stroke ? "DrawGeometry" : "FillGeometry",
      graphics::path_failure_name(f.reason));
  using R = graphics::PathFailureReason;
  const char *const arc_names[] = {"start_x",   "start_y", "end_x",    "end_y",
                                   "rx",        "ry",      "rotation", "lambda",
                                   "clockwise", "large"};
  const char *const join_names[] = {
      "in_x",        "in_y",       "out_x",        "out_y",
      "in_length",   "out_length", "turn_degrees", "miter_ratio",
      "miter_limit", "vertex_x",   "vertex_y"};
  const bool arc = f.reason == R::NonpositiveArcRadii ||
                   f.reason == R::CoincidentArcEndpoints ||
                   f.reason == R::InsufficientArcRadii ||
                   f.reason == R::DegenerateArcMath;
  for (usize i = 0; i < f.count; ++i) {
    if (arc && i < std::size(arc_names))
      std::fprintf(stderr, " %s=%.17g", arc_names[i], f.values[i]);
    else if (f.reason == R::OverLimitMiter && i < std::size(join_names))
      std::fprintf(stderr, " %s=%.17g", join_names[i], f.values[i]);
    else
      std::fprintf(stderr, " value%zu=%.17g", i, f.values[i]);
  }
  std::fprintf(stderr, "\n");
}
} // namespace
void D2DManager::draw_geometry(Marshaler &m, Resource &resource, bool stroke) {
  auto &t = *resource.target;
  if (!t.drawing || !t.dc) {
    t.latch(D2DERR_WRONG_STATE);
    return;
  }
  if (t.error != S_OK)
    return;
  const auto path = m_geometries.find(m.arg_ptr(1));
  const auto brush = m_resources.find(m.arg_ptr(2));
  if (path == m_geometries.end() || path->second.sink) {
    (void)m.raise_access_violation(false, m.arg_ptr(1));
    return;
  }
  if (brush == m_resources.end() || !brush->second.brush) {
    (void)m.raise_access_violation(false, m.arg_ptr(2));
    return;
  }
  if (path->second.factory != resource.factory ||
      brush->second.factory != resource.factory) {
    t.latch(D2DERR_WRONG_FACTORY);
    return;
  }
  if (brush->second.target != resource.target) {
    t.latch(D2DERR_WRONG_RESOURCE_DOMAIN);
    return;
  }
  const auto state = path->second.state;
  if (!state->closed) {
    t.latch(D2DERR_WRONG_STATE);
    return;
  }
  if (state->error != S_OK) {
    t.latch(state->error);
    return;
  }
  if ((stroke && m.arg_ptr(4)) || (!stroke && m.arg_ptr(3))) {
    gap(stroke ? "custom stroke style"
               : "opacity brush requires bitmap brush CLAMP support");
    t.latch(E_NOTIMPL);
    return;
  }
  const float width = stroke ? m.arg_f32(3) : 0;
  if (stroke && (!std::isfinite(width) || width <= 0)) {
    gap("zero/nonfinite/negative stroke width");
    t.latch(E_NOTIMPL);
    return;
  }
  std::vector<graphics::PathElement> elements;
  bool include = false;
  for (const auto &command : state->commands) {
    if (command.method == sink_SetSegmentFlags) {
      if (stroke && command.flags != D2D1_PATH_SEGMENT_NONE) {
        gap("stroke segment flags including implicit closing edge");
        t.latch(E_NOTIMPL);
        return;
      }
      continue;
    }
    graphics::PathElement element{};
    if (command.method == sink_BeginFigure) {
      include = stroke || command.flags == D2D1_FIGURE_BEGIN_FILLED;
      if (!include)
        continue;
      element.verb = graphics::PathVerb::Move;
    } else if (command.method == sink_EndFigure) {
      if (!include)
        continue;
      if (stroke && command.flags != D2D1_FIGURE_END_CLOSED)
        continue;
      element.verb = graphics::PathVerb::Close;
    } else {
      if (!include)
        continue;
      if (stroke && command.flags != D2D1_PATH_SEGMENT_NONE) {
        gap("stroke segment FORCE_UNSTROKED/FORCE_ROUND_LINE_JOIN flags");
        t.latch(E_NOTIMPL);
        return;
      }
      if (command.method == sink_AddArc) {
        if (command.bytes.size() != size_D2D1_ARC_SEGMENT) {
          gap("arc record size");
          t.latch(E_NOTIMPL);
          return;
        }
        u32 sweep = 0, large = 0;
        std::memcpy(&sweep,
                    command.bytes.data() +
                        offset_D2D1_ARC_SEGMENT_sweepDirection,
                    sizeof(sweep));
        std::memcpy(&large,
                    command.bytes.data() + offset_D2D1_ARC_SEGMENT_arcSize,
                    sizeof(large));
        if ((sweep != D2D1_SWEEP_DIRECTION_CLOCKWISE &&
             sweep != D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE) ||
            (large != D2D1_ARC_SIZE_SMALL && large != D2D1_ARC_SIZE_LARGE)) {
          gap("invalid arc sweep/size enum");
          t.latch(E_NOTIMPL);
          return;
        }
        element.verb = graphics::PathVerb::Arc;
        element.clockwise = sweep == D2D1_SWEEP_DIRECTION_CLOCKWISE;
        element.large_arc = large == D2D1_ARC_SIZE_LARGE;
        std::memcpy(element.points.data(),
                    command.bytes.data() + offset_D2D1_ARC_SEGMENT_point,
                    2 * sizeof(float));
        std::memcpy(element.points.data() + 2,
                    command.bytes.data() + offset_D2D1_ARC_SEGMENT_size,
                    2 * sizeof(float));
        std::memcpy(element.points.data() + 4,
                    command.bytes.data() +
                        offset_D2D1_ARC_SEGMENT_rotationAngle,
                    sizeof(float));
        elements.push_back(element);
        continue;
      }
      if (command.method == sink_AddLine || command.method == sink_AddLines)
        element.verb = graphics::PathVerb::Line;
      else if (command.method == sink_AddBezier ||
               command.method == sink_AddBeziers)
        element.verb = graphics::PathVerb::Cubic;
      else if (command.method == sink_AddQuadraticBezier ||
               command.method == sink_AddQuadraticBeziers)
        element.verb = graphics::PathVerb::Quadratic;
      else {
        gap("unknown retained segment");
        t.latch(E_NOTIMPL);
        return;
      }
    }
    if (command.bytes.size() > sizeof(element.points)) {
      gap("unsupported retained segment layout");
      t.latch(E_NOTIMPL);
      return;
    }
    if (!command.bytes.empty())
      std::memcpy(element.points.data(), command.bytes.data(),
                  command.bytes.size());
    elements.push_back(element);
  }
  auto transform = t.transform;
  for (usize i : {usize(0), usize(2), usize(4)})
    transform[i] *= t.dpi_x / dip_screen_dpi;
  for (usize i : {usize(1), usize(3), usize(5)})
    transform[i] *= t.dpi_y / dip_screen_dpi;
  std::vector<graphics::PathEdge> edges;
  graphics::PathMathFailure failure;
  if (!graphics::path_edges(elements, stroke, width, transform, edges,
                            &failure)) {
    math_gap(failure, stroke);
    t.latch(E_NOTIMPL);
    return;
  }
  if (edges.empty())
    return;
  auto color = brush->second.color;
  const float opacity = brush->second.opacity;
  if (!std::isfinite(opacity) ||
      std::any_of(color.begin(), color.end(),
                  [](float v) { return !std::isfinite(v); })) {
    gap("nonfinite solid brush");
    t.latch(E_NOTIMPL);
    return;
  }
  const float alpha =
      std::clamp(color[3], 0.f, 1.f) * std::clamp(opacity, 0.f, 1.f);
  if (alpha == 0)
    return;
  const std::array<float, 4> premultiplied{
      std::clamp(color[0], 0.f, 1.f) * alpha,
      std::clamp(color[1], 0.f, 1.f) * alpha,
      std::clamp(color[2], 0.f, 1.f) * alpha, alpha};
  const bool alternate = !stroke && state->fill == D2D1_FILL_MODE_ALTERNATE,
             aa = t.antialias == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
  fill_device_edges(t, edges, premultiplied, alternate, aa);
}

void D2DManager::fill_device_edges(Target &t, const std::vector<graphics::PathEdge> &edges,
                                   const std::array<float, 4> &premultiplied,
                                   bool alternate, bool aa) {
  const float alpha = premultiplied[3];
  std::array<i32, 4> clip{0, 0, i32(t.width), i32(t.height)};
  if (!t.clips.empty()) {
    const auto &c = t.clips.back();
    const auto edge = [](float x, u32 size) {
      return i32(std::ceil(std::clamp(x - .5f, 0.f, float(size))));
    };
    clip = {edge(c[0], t.width), edge(c[1], t.height), edge(c[2], t.width),
            edge(c[3], t.height)};
  }
  if (t.gpu) {
    if (!t.gpu->draw_edges(edges, premultiplied, alternate, aa, clip)) {
      t.latch(D2DERR_RECREATE_TARGET);
      return;
    }
    t.dirty = true;
    return;
  }
  const auto quantize = [](float x) {
    return std::nearbyint(std::clamp(x, 0.f, 1.f) * 255.f) / 255.f;
  };
  for (i32 y = clip[1]; y < clip[3]; ++y)
    for (i32 x = clip[0]; x < clip[2]; ++x) {
      const float coverage =
          graphics::path_coverage(edges, float(x), float(y), alternate, aa);
      if (coverage == 0)
        continue;
      auto *pixel = t.pixels.get() + (usize(y) * t.width + x) * 4;
      const float a = quantize(alpha) * coverage;
      for (usize c = 0; c < 4; ++c)
        pixel[c] = quantize(quantize(premultiplied[c]) * coverage +
                            quantize(pixel[c]) * (1 - a));
    }
  t.dirty = true;
}
} // namespace vitreo::winapi
