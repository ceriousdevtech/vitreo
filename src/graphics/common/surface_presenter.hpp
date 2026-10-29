#pragma once

#include <vitreo/common.hpp>

#include <vector>

namespace vitreo::graphics {

class CpuSurface;

struct PresentationRect {
    double x{0};
    double y{0};
    double width{0};
    double height{0};
};

/// Aspect-fit @p source into @p target, centred, preserving aspect ratio.
///
/// Default is fractional scale (CAMetalLayer linear filter) so UI bitmap fonts
/// stay readable when upscaling. `VITREO_INTEGER_SCALE=1` forces the largest
/// integer scale that fits (nearest filter — pixel-crisp, black bars when the
/// scale would otherwise be fractional). `VITREO_INTEGER_SCALE=0` is the same
/// as the default.
[[nodiscard]] PresentationRect aspect_fit_rect(double source_width,
                                               double source_height,
                                               double target_width,
                                               double target_height) noexcept;

/// True when @p fitted is an integer multiple of the source size. Used to pick
/// nearest vs linear CAMetalLayer magnification filters.
[[nodiscard]] bool presentation_scale_is_integer(
    double source_width, double source_height,
    const PresentationRect& fitted) noexcept;

[[nodiscard]] bool expand_indexed_bgra(const CpuSurface& surface,
                                       const u8* pixels, usize pixel_bytes,
                                       std::vector<u8>& output);

class ISurfacePresenter {
public:
    virtual ~ISurfacePresenter() = default;

    [[nodiscard]] virtual bool present_indexed(u32 target,
                                               const CpuSurface& surface,
                                               const u8* pixels,
                                               usize pixel_bytes) = 0;

    /// Drop any cached identity for @p surface before the caller destroys it.
    /// DirectDraw Release of the primary used to free CpuSurface while the
    /// Metal presenter still held a raw pointer from the last present.
    virtual void invalidate_surface(const CpuSurface* surface) noexcept {
        (void)surface;
    }
};

} // namespace vitreo::graphics