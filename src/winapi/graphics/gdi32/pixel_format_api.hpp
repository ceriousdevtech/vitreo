#pragma once
/// @file pixel_format_api.hpp
/// @brief The pixel-format calls shared by gdi32 and opengl32.
///
/// Windows' opengl32 wglChoosePixelFormat / wglDescribePixelFormat /
/// wglGetPixelFormat / wglSetPixelFormat have the same return, last error and
/// output as gdi32's ChoosePixelFormat / DescribePixelFormat / GetPixelFormat /
/// SetPixelFormat on the same input (wgl_contract_probe64.expected: every
/// wglpf-X line equals its wglpf-X-gdi line), and a format set through one is
/// seen by the other (wglpf-cross-*). Both entry points run these functions.
/// Arguments are read from the marshaler in the Win32 order.

namespace vitreo::winapi {

class GdiManager;
class Marshaler;

void pixel_format_choose(GdiManager& gdi, Marshaler& m);    // (hdc, ppfd)
void pixel_format_describe(GdiManager& gdi, Marshaler& m);  // (hdc, index, nBytes, ppfd)
void pixel_format_get(GdiManager& gdi, Marshaler& m);       // (hdc)
void pixel_format_set(GdiManager& gdi, Marshaler& m);       // (hdc, index, ppfd)

}  // namespace vitreo::winapi
