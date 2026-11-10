// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d8_contract.hpp
/// @brief Direct3D 8 ABI tables generated from the SDK headers.
///
/// Wraps `d3d8_contract.inc`, which tools/gen_d3d8_contract.py writes from the
/// mingw-w64 d3d8.h / d3d8types.h / d3d8caps.h (and the D3D9 headers the
/// frontend is layered on). Nothing in it is typed by hand; CTest
/// `d3d8_contract_generated` re-runs the generator in check mode.

#include <vitreo/common.hpp>
#include "winapi/graphics/d3d8/d3d8.hpp"

#include <array>

namespace vitreo::winapi::d3d8c {

/// {name, x86 stdcall callee-pop bytes including the implicit `this`}.
using D3D8MethodDesc = ::vitreo::winapi::D3D8MethodInfo;

/// Byte offset of a member in the D3D8 structure and of the member of the
/// same name in the corresponding D3D9 structure; -1 where absent.
struct FieldPair {
    i32 d3d8;
    i32 d3d9;
};

struct D3D8StateRow {
    const char* name;
    u32 value;
    /// The D3D9 D3DRENDERSTATETYPE of the same name, or -1.
    i32 d3d9;
};

struct D3D8StageStateRow {
    const char* name;
    u32 value;
    /// The D3D9 D3DTEXTURESTAGESTATETYPE of the same name, or -1.
    i32 d3d9_stage;
    /// The D3D9 D3DSAMPLERSTATETYPE it moved to (D3DSAMP_<suffix>), or -1.
    i32 d3d9_sampler;
};

#include "winapi/graphics/d3d8/d3d8_contract.inc"

}  // namespace vitreo::winapi::d3d8c
