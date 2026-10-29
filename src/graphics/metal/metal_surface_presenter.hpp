#pragma once

#include <memory>

namespace vitreo::graphics {
class ISurfacePresenter;
}

namespace vitreo::windowing {
class CocoaBridge;
}

namespace vitreo::graphics {

[[nodiscard]] std::unique_ptr<ISurfacePresenter> create_metal_surface_presenter(
    windowing::CocoaBridge& cocoa_bridge);

} // namespace vitreo::graphics