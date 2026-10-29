#pragma once
/// @file msimg32.hpp
/// @brief MSIMG32.dll API stubs (image blending)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class GdiManager;

class Msimg32Manager {
public:
    Msimg32Manager() = default;

    /// The blend/blit operations need DC and bitmap internals, which live in
    /// GdiManager. Without it they cannot touch pixels and must fail honestly
    /// rather than report a blend that never happened.
    void set_gdi_manager(GdiManager* gdi) noexcept { m_gdi = gdi; }

    void impl_TransparentBlt(Marshaler& m);
    void impl_AlphaBlend(Marshaler& m);
    void impl_GradientFill(Marshaler& m);

private:
    GdiManager* m_gdi = nullptr;
};

DllExports create_msimg32_thunks(Msimg32Manager& mgr);

} // namespace vitreo::winapi
