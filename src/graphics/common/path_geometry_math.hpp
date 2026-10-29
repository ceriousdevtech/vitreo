// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <array>
#include <span>
#include <vector>
#include <vitreo/common.hpp>
namespace vitreo::graphics {
enum class PathVerb { Move, Line, Quadratic, Cubic, Arc, Close };
struct PathElement {
  PathVerb verb;
  std::array<float, 6> points{};
  bool clockwise{}, large_arc{};
};
using PathEdge = std::array<float, 4>;
enum class PathFailureReason {
  None,
  InvalidTransform,
  Allocation,
  NonfiniteInput,
  NonpositiveArcRadii,
  CoincidentArcEndpoints,
  InsufficientArcRadii,
  DegenerateArcMath,
  InvalidStrokeWidth,
  OverLimitMiter,
  DegenerateTangent,
  InvalidMove,
  InvalidPoint,
  EdgeBudget,
  SubdivisionLimit
};
struct PathMathFailure {
  PathFailureReason reason{PathFailureReason::None};
  std::array<double, 12> values{};
  usize count{};
};
const char *path_failure_name(PathFailureReason);
// Geometry only: no raster image or CPU coverage is produced.
bool path_edges(std::span<const PathElement>, bool stroke, float stroke_width,
                const std::array<float, 6> &device_transform,
                std::vector<PathEdge> &, PathMathFailure *failure = nullptr);
float path_coverage(std::span<const PathEdge>, float x, float y, bool alternate,
                    bool antialias);
} // namespace vitreo::graphics
