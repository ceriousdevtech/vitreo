// SPDX-License-Identifier: LGPL-2.1-only
#include "path_geometry_math.hpp"
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
namespace vitreo::graphics {
namespace {
void fail(PathMathFailure *out, PathFailureReason reason,
          std::initializer_list<double> values = {}) {
  if (!out || out->reason != PathFailureReason::None)
    return;
  out->reason = reason;
  out->count = std::min(values.size(), out->values.size());
  std::copy_n(values.begin(), out->count, out->values.begin());
}
// Microsoft Learn StrokeStyleProperties: flat caps, miter joins, limit10.
constexpr double default_miter_limit = 10;
// Endpoint-to-center equations: W3C SVG implementation notes B.2.4.
// Only the shared ellipse mathematics is used; SVG's degenerate/radius-repair
// policies are not assumed to be Windows behavior.
bool append_arc(CGMutablePathRef path, const PathElement &e,
                PathMathFailure *failure) {
  const auto start = CGPathGetCurrentPoint(path);
  const auto &p = e.points;
  const double rx = p[2], ry = p[3],
               phi = std::remainder(double(p[4]), 360.0) * std::numbers::pi /
                     180.0;
  const auto arc_fail = [&](PathFailureReason reason, double lambda = 0) {
    fail(failure, reason,
         {start.x, start.y, p[0], p[1], rx, ry, p[4], lambda,
          double(e.clockwise), double(e.large_arc)});
    return false;
  };
  if (rx <= 0 || ry <= 0)
    return arc_fail(PathFailureReason::NonpositiveArcRadii);
  if (start.x == p[0] && start.y == p[1])
    return arc_fail(PathFailureReason::CoincidentArcEndpoints);
  const double cs = std::cos(phi), sn = std::sin(phi),
               dx = (start.x - p[0]) * .5, dy = (start.y - p[1]) * .5;
  const double x = cs * dx + sn * dy, y = -sn * dx + cs * dy;
  const double lambda = x * x / (rx * rx) + y * y / (ry * ry);
  if (!(lambda > 0))
    return arc_fail(PathFailureReason::DegenerateArcMath, lambda);
  if (lambda > 1)
    return arc_fail(PathFailureReason::InsufficientArcRadii,
                    lambda); // no radius enlargement
  const double coefficient = (e.large_arc == e.clockwise ? -1.0 : 1.0) *
                             std::sqrt(std::max(0.0, (1 - lambda) / lambda));
  const double cx = coefficient * rx * y / ry, cy = -coefficient * ry * x / rx;
  const double center_x = cs * cx - sn * cy + (start.x + p[0]) * .5,
               center_y = sn * cx + cs * cy + (start.y + p[1]) * .5;
  const double ux = (x - cx) / rx, uy = (y - cy) / ry, vx = (-x - cx) / rx,
               vy = (-y - cy) / ry;
  const double theta = std::atan2(uy, ux);
  double delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  if (e.clockwise && delta < 0)
    delta += 2 * std::numbers::pi;
  if (!e.clockwise && delta > 0)
    delta -= 2 * std::numbers::pi;
  // D2D1_ARC_SEGMENT runs from the current point to `point` exactly. Emit
  // cubics (<= 90 degrees each) anchored on those endpoints; CGPathAddArc
  // would join its recomputed start with a rounding-length line segment.
  const auto map = [&](double ux, double uy) {
    return CGPoint{cs * rx * ux - sn * ry * uy + center_x,
                   sn * rx * ux + cs * ry * uy + center_y};
  };
  const int pieces =
      std::max(1, int(std::ceil(std::abs(delta) / (std::numbers::pi / 2) -
                                1e-9)));
  const double step = delta / pieces, k = 4.0 / 3.0 * std::tan(step / 4);
  for (int i = 0; i < pieces; ++i) {
    const double a0 = theta + step * i, a1 = a0 + step;
    const double c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1),
                 s1 = std::sin(a1);
    const auto control1 = map(c0 - k * s0, s0 + k * c0),
               control2 = map(c1 + k * s1, s1 - k * c1);
    const auto end = i + 1 == pieces ? CGPoint{p[0], p[1]} : map(c1, s1);
    CGPathAddCurveToPoint(path, nullptr, control1.x, control1.y, control2.x,
                          control2.y, end.x, end.y);
  }
  return true;
}
struct StrokeJoins {
  PathMathFailure *failure{};
  CGPoint current{}, start{}, first{}, last{};
  bool have_first{}, have_last{}, ok{true};
  static CGPoint difference(CGPoint a, CGPoint b) {
    return {a.x - b.x, a.y - b.y};
  }
  static bool nonzero(CGPoint v) { return v.x != 0 || v.y != 0; }
  void join(CGPoint a, CGPoint b) {
    const double length = std::hypot(a.x, a.y) * std::hypot(b.x, b.y);
    if (!length)
      return;
    const double cosine =
        std::clamp((a.x * b.x + a.y * b.y) / length, -1.0, 1.0);
    if ((1 + cosine) * .5 <= 1 / (default_miter_limit * default_miter_limit)) {
      fail(failure, PathFailureReason::OverLimitMiter,
           {a.x, a.y, b.x, b.y, std::hypot(a.x, a.y), std::hypot(b.x, b.y),
            std::acos(cosine) * 180 / std::numbers::pi,
            std::sqrt(2 / (1 + cosine)), default_miter_limit, current.x,
            current.y});
      ok = false;
    }
  }
  void segment(CGPoint initial, CGPoint final, CGPoint end) {
    if (!nonzero(initial) || !nonzero(final)) {
      fail(failure, PathFailureReason::DegenerateTangent,
           {initial.x, initial.y, final.x, final.y, end.x, end.y});
      ok = false;
      return;
    }
    if (have_last)
      join(last, initial);
    if (!have_first) {
      first = initial;
      have_first = true;
    }
    last = final;
    have_last = true;
    current = end;
  }
  void line(CGPoint end) {
    auto v = difference(end, current);
    if (nonzero(v))
      segment(v, v, end);
  }
};
void check_joins(void *context, const CGPathElement *e) {
  auto &c = *static_cast<StrokeJoins *>(context);
  if (!c.ok)
    return;
  switch (e->type) {
  case kCGPathElementMoveToPoint:
    c.current = c.start = e->points[0];
    c.have_first = c.have_last = false;
    break;
  case kCGPathElementAddLineToPoint:
    c.line(e->points[0]);
    break;
  case kCGPathElementAddQuadCurveToPoint: {
    auto first = c.difference(e->points[0], c.current),
         last = c.difference(e->points[1], e->points[0]);
    if (!c.nonzero(first))
      first = c.difference(e->points[1], c.current);
    if (!c.nonzero(last))
      last = c.difference(e->points[1], c.current);
    if (c.nonzero(first) || c.nonzero(last))
      c.segment(first, last, e->points[1]);
    break;
  }
  case kCGPathElementAddCurveToPoint: {
    auto first = c.difference(e->points[0], c.current),
         last = c.difference(e->points[2], e->points[1]);
    if (!c.nonzero(first))
      first = c.difference(e->points[1], c.current);
    if (!c.nonzero(first))
      first = c.difference(e->points[2], c.current);
    if (!c.nonzero(last))
      last = c.difference(e->points[2], e->points[0]);
    if (!c.nonzero(last))
      last = c.difference(e->points[2], c.current);
    if (c.nonzero(first) || c.nonzero(last))
      c.segment(first, last, e->points[2]);
    break;
  }
  case kCGPathElementCloseSubpath:
    c.line(c.start);
    if (c.have_first && c.have_last)
      c.join(c.last, c.first);
    break;
  }
}
struct Flatten {
  std::vector<PathEdge> &edges;
  PathMathFailure *failure{};
  CGPoint current{}, start{};
  bool ok{true};
  void line(CGPoint p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e7 ||
        std::abs(p.y) > 1e7) {
      fail(failure, PathFailureReason::InvalidPoint, {p.x, p.y, 1e7});
      ok = false;
      return;
    }
    if (p.x != current.x || p.y != current.y)
      edges.push_back(
          {float(current.x), float(current.y), float(p.x), float(p.y)});
    current = p;
    if (edges.size() > 192) {
      fail(failure, PathFailureReason::EdgeBudget, {double(edges.size()), 192});
      ok = false;
    }
  }
  static CGPoint mid(CGPoint a, CGPoint b) {
    return {(a.x + b.x) * .5, (a.y + b.y) * .5};
  }
  void cubic(CGPoint a, CGPoint b, CGPoint c, CGPoint d, unsigned depth = 0) {
    if (!ok)
      return;
    const auto distance = [&](CGPoint p) {
      const double x = d.x - a.x, y = d.y - a.y;
      return std::abs((p.x - a.x) * y - (p.y - a.y) * x) /
             std::max(1e-12, std::hypot(x, y));
    };
    // Device-space subdivision is geometric preprocessing, not pixel coverage.
    if (std::max(distance(b), distance(c)) <= 1.0 / 32 &&
        std::hypot(b.x - a.x, b.y - a.y) + std::hypot(c.x - b.x, c.y - b.y) +
                std::hypot(d.x - c.x, d.y - c.y) -
                std::hypot(d.x - a.x, d.y - a.y) <=
            1.0 / 32) {
      line(d);
      return;
    }
    if (depth >= 20) {
      fail(failure, PathFailureReason::SubdivisionLimit,
           {double(depth), a.x, a.y, d.x, d.y});
      ok = false;
      return;
    }
    const auto ab = mid(a, b), bc = mid(b, c), cd = mid(c, d),
               abc = mid(ab, bc), bcd = mid(bc, cd), center = mid(abc, bcd);
    cubic(a, ab, abc, center, depth + 1);
    cubic(center, bcd, cd, d, depth + 1);
  }
};
void apply(void *context, const CGPathElement *e) {
  auto &f = *static_cast<Flatten *>(context);
  if (!f.ok)
    return;
  switch (e->type) {
  case kCGPathElementMoveToPoint:
    if (!std::isfinite(e->points[0].x) || !std::isfinite(e->points[0].y) ||
        std::abs(e->points[0].x) > 1e7 || std::abs(e->points[0].y) > 1e7) {
      fail(f.failure, PathFailureReason::InvalidMove,
           {e->points[0].x, e->points[0].y, 1e7});
      f.ok = false;
      break;
    }
    f.current = f.start = e->points[0];
    break;
  case kCGPathElementAddLineToPoint:
    f.line(e->points[0]);
    break;
  case kCGPathElementAddQuadCurveToPoint: {
    const auto a = f.current, b = e->points[0], c = e->points[1];
    f.cubic(a, {a.x + (b.x - a.x) * 2 / 3, a.y + (b.y - a.y) * 2 / 3},
            {c.x + (b.x - c.x) * 2 / 3, c.y + (b.y - c.y) * 2 / 3}, c);
    break;
  }
  case kCGPathElementAddCurveToPoint:
    f.cubic(f.current, e->points[0], e->points[1], e->points[2]);
    break;
  case kCGPathElementCloseSubpath:
    f.line(f.start);
    break;
  }
}
} // namespace
bool path_edges(std::span<const PathElement> elements, bool stroke, float width,
                const std::array<float, 6> &t, std::vector<PathEdge> &output,
                PathMathFailure *failure) {
  output.clear();
  if (failure)
    *failure = {};
  for (float v : t)
    if (!std::isfinite(v)) {
      fail(failure, PathFailureReason::InvalidTransform, {v});
      return false;
    }
  CGMutablePathRef path = CGPathCreateMutable();
  if (!path) {
    fail(failure, PathFailureReason::Allocation);
    return false;
  }
  for (const auto &e : elements) {
    for (float v : e.points)
      if (!std::isfinite(v)) {
        fail(failure, PathFailureReason::NonfiniteInput, {v, double(e.verb)});
        CGPathRelease(path);
        return false;
      }
    const auto &p = e.points;
    switch (e.verb) {
    case PathVerb::Move:
      CGPathMoveToPoint(path, nullptr, p[0], p[1]);
      break;
    case PathVerb::Line:
      CGPathAddLineToPoint(path, nullptr, p[0], p[1]);
      break;
    case PathVerb::Quadratic:
      CGPathAddQuadCurveToPoint(path, nullptr, p[0], p[1], p[2], p[3]);
      break;
    case PathVerb::Cubic:
      CGPathAddCurveToPoint(path, nullptr, p[0], p[1], p[2], p[3], p[4], p[5]);
      break;
    case PathVerb::Arc:
      if (!append_arc(path, e, failure)) {
        CGPathRelease(path);
        return false;
      }
      break;
    case PathVerb::Close:
      CGPathCloseSubpath(path);
      break;
    }
  }
  CGPathRef outline = nullptr;
  if (stroke) {
    if (!std::isfinite(width) || width <= 0) {
      fail(failure, PathFailureReason::InvalidStrokeWidth, {width});
      CGPathRelease(path);
      return false;
    }
    StrokeJoins joins;
    joins.failure = failure;
    CGPathApply(path, &joins, check_joins);
    if (!joins.ok) {
      CGPathRelease(path);
      return false;
    }
    outline =
        CGPathCreateCopyByStrokingPath(path, nullptr, width, kCGLineCapButt,
                                       kCGLineJoinMiter, default_miter_limit);
    if (!outline) {
      fail(failure, PathFailureReason::Allocation);
      CGPathRelease(path);
      return false;
    }
  }
  const CGAffineTransform transform =
      CGAffineTransformMake(t[0], t[1], t[2], t[3], t[4], t[5]);
  CGPathRef device =
      CGPathCreateCopyByTransformingPath(stroke ? outline : path, &transform);
  Flatten f{output, failure};
  if (device)
    CGPathApply(device, &f, apply);
  else {
    fail(failure, PathFailureReason::Allocation);
    f.ok = false;
  }
  if (device)
    CGPathRelease(device);
  if (outline)
    CGPathRelease(outline);
  CGPathRelease(path);
  // Retain the failed partial edge count for diagnostics, never for drawing.
  return f.ok;
}
const char *path_failure_name(PathFailureReason reason) {
  switch (reason) {
#define PATH_REASON(name)                                                      \
  case PathFailureReason::name:                                                \
    return #name
    PATH_REASON(None);
    PATH_REASON(InvalidTransform);
    PATH_REASON(Allocation);
    PATH_REASON(NonfiniteInput);
    PATH_REASON(NonpositiveArcRadii);
    PATH_REASON(CoincidentArcEndpoints);
    PATH_REASON(InsufficientArcRadii);
    PATH_REASON(DegenerateArcMath);
    PATH_REASON(InvalidStrokeWidth);
    PATH_REASON(OverLimitMiter);
    PATH_REASON(DegenerateTangent);
    PATH_REASON(InvalidMove);
    PATH_REASON(InvalidPoint);
    PATH_REASON(EdgeBudget);
    PATH_REASON(SubdivisionLimit);
#undef PATH_REASON
  }
  return "Unknown";
}
float path_coverage(std::span<const PathEdge> edges, float x, float y,
                    bool alternate, bool antialias) {
  const unsigned n = antialias ? 4 : 1;
  unsigned covered = 0;
  for (unsigned sy = 0; sy < n; ++sy)
    for (unsigned sx = 0; sx < n; ++sx) {
      const float px = x + (sx + .5f) / n, py = y + (sy + .5f) / n;
      int winding = 0;
      for (const auto &e : edges) {
        if ((e[1] <= py && e[3] > py) || (e[3] <= py && e[1] > py)) {
          const float at = e[0] + (py - e[1]) * (e[2] - e[0]) / (e[3] - e[1]);
          if (at > px)
            winding += e[3] > e[1] ? 1 : -1;
        }
      }
      if (alternate ? (winding % 2 != 0) : (winding != 0))
        ++covered;
    }
  return float(covered) / (n * n);
}
} // namespace vitreo::graphics
