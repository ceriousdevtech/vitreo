#include "graphics/common/surface_presenter.hpp"

#include "graphics/contracts/cpu_surface.hpp"
#include "graphics/contracts/indexed_color_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vitreo::graphics {
namespace {

enum class FitPolicy {
    AspectFill,     // fractional scale OK (default)
    IntegerPrefer2, // integer when floor(scale) >= 2
    IntegerAlways,  // integer even when that means 1×
};

[[nodiscard]] FitPolicy fit_policy_from_env() noexcept {
    const char* env = std::getenv("VITREO_INTEGER_SCALE");
    // Default: fractional aspect-fit + linear present filter. Integer/nearest
    // upscale makes thin bitmap UI fonts (an options dialog)
    // look crushed; pixel-art titles opt in with VITREO_INTEGER_SCALE=1.
    if (env == nullptr || env[0] == '\0') return FitPolicy::AspectFill;
    if (env[0] == '0' && env[1] == '\0') return FitPolicy::AspectFill;
    return FitPolicy::IntegerAlways;
}

[[nodiscard]] PresentationRect make_centered(double source_width,
                                             double source_height,
                                             double target_width,
                                             double target_height,
                                             double scale) noexcept {
    const double width = source_width * scale;
    const double height = source_height * scale;
    return {
        (target_width - width) * 0.5,
        (target_height - height) * 0.5,
        width,
        height,
    };
}

} // namespace

PresentationRect aspect_fit_rect(double source_width, double source_height,
                                 double target_width, double target_height) noexcept {
    if (source_width <= 0 || source_height <= 0 ||
        target_width <= 0 || target_height <= 0) {
        return {};
    }

    const double scale = std::min(target_width / source_width,
                                  target_height / source_height);
    const int integer_scale =
        static_cast<int>(std::floor(scale + 1e-6));
    const FitPolicy policy = fit_policy_from_env();

    if (policy == FitPolicy::IntegerAlways && integer_scale >= 1) {
        return make_centered(source_width, source_height, target_width,
                             target_height,
                             static_cast<double>(integer_scale));
    }
    if (policy == FitPolicy::IntegerPrefer2 && integer_scale >= 2) {
        return make_centered(source_width, source_height, target_width,
                             target_height,
                             static_cast<double>(integer_scale));
    }
    return make_centered(source_width, source_height, target_width,
                         target_height, scale);
}

bool presentation_scale_is_integer(double source_width, double source_height,
                                   const PresentationRect& fitted) noexcept {
    if (source_width <= 0 || source_height <= 0 ||
        fitted.width <= 0 || fitted.height <= 0) {
        return false;
    }
    const double sx = fitted.width / source_width;
    const double sy = fitted.height / source_height;
    const double rx = std::round(sx);
    const double ry = std::round(sy);
    return std::fabs(sx - rx) < 1e-3 && std::fabs(sy - ry) < 1e-3 &&
           rx >= 1.0 && ry >= 1.0;
}

bool expand_indexed_bgra(const CpuSurface& surface,
                         const u8* pixels, usize pixel_bytes,
                         std::vector<u8>& output) {
    const auto& palette = surface.palette();
    const usize required = static_cast<usize>(surface.pitch()) * surface.height();
    if (!pixels || pixel_bytes < required || surface.bits_per_pixel() != 8 ||
        !palette) {
        return false;
    }

    output.resize(static_cast<usize>(surface.width()) * surface.height() * 4);
    for (u32 y = 0; y < surface.height(); ++y) {
        const u8* source = pixels + static_cast<usize>(y) * surface.pitch();
        u8* destination = output.data() +
                          static_cast<usize>(y) * surface.width() * 4;
        for (u32 x = 0; x < surface.width(); ++x) {
            const PaletteEntry* color = palette->entries(source[x], 1);
            destination[x * 4 + 0] = color->blue;
            destination[x * 4 + 1] = color->green;
            destination[x * 4 + 2] = color->red;
            destination[x * 4 + 3] = 0xFF;
        }
    }
    return true;
}

} // namespace vitreo::graphics
