// SPDX-License-Identifier: MIT
//
// Substantial portion of Microsoft's "D3D11 Fixed Function Tessellator
// Reference" (May 7, 2012), adapted as described below. The MIT grant
// requires that this copyright notice and permission notice travel with
// every copy:
//
// Copyright (c) Microsoft Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
/// @file tessellator.hpp
/// @brief The D3D11 fixed-function tessellator, for the tessellation
///        emulation's exact path.
///
/// D3D11.3 11.7.1 points at the reference fixed-function tessellator,
/// Microsoft's CHWTessellator (tessellator.hpp / tessellator.cpp, May 7,
/// 2012). This is a port of that class, statement for statement, in a form
/// that compiles both as C++ (the model the tests run, and what the host
/// uses to bound a patch's output) and as Metal Shading Language (one GPU
/// thread per patch): the body below is written once, stringified for MSL,
/// and compiled here for C++. Its differences from the reference are
/// representation only: the object's members are a TessState passed by
/// reference, points and indices go to a sink (counted, or written at a
/// patch's offsets), and the X360 comparison mode is left out.
///
/// Why not Metal's tessellator: it takes half-precision factors (a D3D
/// factor is float32 converted to 16.16 fixed point, 11.7.14), has no
/// isoline domain and no point or line output, and neither its vertices nor
/// its connectivity can be captured for stream output or a geometry shader.
/// It still draws plain triangle output (the fast path).

#pragma once

#include <vitreo/common.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::graphics::tess {

// ---------------------------------------------------------------------------
// Dual C++ / MSL body. Inside it: `uint`/`int`/`float` only, arrays of
// fixed size, no statics or recursion; TESS_THREAD / TESS_DEVICE spell the
// MSL address spaces (empty in C++), TESS_AS_INT / TESS_AS_FLOAT bit casts.
// ---------------------------------------------------------------------------
#define VITREO_TESS_BODY(...) __VA_ARGS__ \
    [[nodiscard]] inline const char* body_source() { return #__VA_ARGS__; }

#define TESS_THREAD
#define TESS_DEVICE
#define constant_int constexpr int
#define constant_uint constexpr unsigned int
#define constant_float constexpr float
using uint = unsigned int;
[[nodiscard]] inline int tess_as_int(float f) {
    int i;
    std::memcpy(&i, &f, sizeof(i));
    return i;
}
[[nodiscard]] inline float tess_as_float(int i) {
    float f;
    std::memcpy(&f, &i, sizeof(f));
    return f;
}
#define TESS_AS_INT(f) tess_as_int(f)
#define TESS_AS_FLOAT(i) tess_as_float(i)
using std::ceil;

VITREO_TESS_BODY(

// D3D11_TESSELLATOR_PARTITIONING, _OUTPUT_PRIMITIVE (the reference's enums)
constant_int TESS_INTEGER = 0; constant_int TESS_POW2 = 1;
constant_int TESS_FRACTIONAL_ODD = 2; constant_int TESS_FRACTIONAL_EVEN = 3;
constant_int TESS_OUT_POINT = 0; constant_int TESS_OUT_LINE = 1;
constant_int TESS_OUT_TRIANGLE_CW = 2; constant_int TESS_OUT_TRIANGLE_CCW = 3;
constant_int TESS_PARITY_EVEN = 0; constant_int TESS_PARITY_ODD = 1;

constant_uint FXP_FRACTION_BITS = 16;
constant_uint FXP_FRACTION_MASK = 0x0000ffff;
constant_uint FXP_INTEGER_MASK = 0x7fff0000;
constant_uint FXP_ONE = 1 << 16;
constant_uint FXP_ONE_THIRD = 0x00005555;
constant_uint FXP_TWO_THIRDS = 0x0000aaaa;
constant_uint FXP_ONE_HALF = 0x00008000;

struct TessFactorContext {
    uint fxpInvNumSegmentsOnFloorTessFactor;
    uint fxpInvNumSegmentsOnCeilTessFactor;
    uint fxpHalfTessFactorFraction;
    int numHalfTessFactorPoints;
    int splitPointOnFloorHalfTessFactor;
};

/// Where points and indices go. `write` 0 counts only. A point is a uint4:
/// u and v as float bits (a tri domain's w is 1 - u - v, exact: both are
/// 16.16 fixed point), the patch's index in the hull kernel's output, and
/// its SV_PrimitiveID. An index is point_base + the patch-local index; with
/// `write_primitive_ids` each primitive's SV_PrimitiveID also goes to
/// primitive_ids (for a geometry shader after the domain shader).
struct TessSink {
    TESS_DEVICE uint* points;
    TESS_DEVICE uint* indices;
    TESS_DEVICE uint* primitive_ids;
    uint point_base;
    uint index_base;
    uint patch_index;
    uint primitive_id;
    uint vertices_per_primitive;
    uint write;
    uint write_primitive_ids;
};

struct TessState {
    int partitioning;
    int originalPartitioning;
    int parity;
    int originalParity;
    int outputPrimitive;
    int numPoints;
    int numIndices;
    int usingPatchedIndices;
    int usingPatchedIndices2;
    // INDEX_PATCH_CONTEXT
    int insidePointIndexDeltaToRealValue;
    int insidePointIndexBadValue;
    int insidePointIndexReplacementValue;
    int outsidePointIndexPatchBase;
    int outsidePointIndexDeltaToRealValue;
    int outsidePointIndexBadValue;
    int outsidePointIndexReplacementValue;
    // INDEX_PATCH_CONTEXT2
    int baseIndexToInvert;
    int indexInversionEndPoint;
    int cornerCaseBadValue;
    int cornerCaseReplacementValue;
    TessSink sink;
};

// --- D3D-compliant float math (the reference's isNaN / flush / fmin / fmax)
inline bool tess_is_nan(float a) {
    const int u = TESS_AS_INT(a);
    return ((u & 0x7f800000) == 0x7f800000) && ((u & 0x007fffff) != 0);
}
inline float tess_flush(float a) {
    const int b = TESS_AS_INT(a) & 0x7fffffff;
    if (b < 0x00800000) return TESS_AS_FLOAT(TESS_AS_INT(a) & (int)0x80000000);
    return a;
}
inline float tess_fmin(float a, float b) {
    const float fa = tess_flush(a);
    const float fb = tess_flush(b);
    if (tess_is_nan(fb)) return a;
    if (fa == 0 && fb == 0) return (TESS_AS_INT(fa) & (int)0x80000000) != 0 ? a : b;
    return fa < fb ? a : b;
}
inline float tess_fmax(float a, float b) {
    const float fa = tess_flush(a);
    const float fb = tess_flush(b);
    if (tess_is_nan(fb)) return a;
    if (fa == 0 && fb == 0) return (TESS_AS_INT(fb) & (int)0x80000000) != 0 ? a : b;
    return fa >= fb ? a : b;
}
inline int tess_min(int a, int b) { return a < b ? a : b; }
inline int tess_max(int a, int b) { return a > b ? a : b; }

// --- fixed point (floatToIDotF<15, 16, unsigned>, fixedToFloat, ...)
inline uint tess_float_to_fixed(float input) {
    int output = TESS_AS_INT(input);
    const int unbiasedExponent = ((output & 0x7f800000) >> 23) - 127;
    const int isNegative = output & (int)0x80000000;
    const int maxPosValueFloat = (127 + 15) << 23;
    if (unbiasedExponent == 128 && (output & 0x007fffff) != 0) {
        output = 0;  // NaN
    } else if (isNegative == 0 && output >= maxPosValueFloat) {
        output = -1;  // c_iMaxResult
    } else if (isNegative != 0) {
        output = 0;  // c_iMinResult: an unsigned format clamps to 0
    } else if (unbiasedExponent < -16 - 1) {
        output = 0;
    } else {
        output = (output & 0x007fffff) | 0x00800000;
        const int extraBits = 23 - 16 - unbiasedExponent;
        if (extraBits >= 0) {
            const int LSB = 1 << extraBits;
            const int extraBitsMask = LSB - 1;
            const int halfLsb = LSB >> 1;  // round bias
            if ((output & LSB) != 0 || (output & extraBitsMask) > halfLsb) {
                output += halfLsb;
            }
            output >>= extraBits;
        } else {
            output <<= -extraBits;
        }
    }
    return (uint)output;
}
inline float tess_fixed_to_float(uint input) {
    return (float)(input >> FXP_FRACTION_BITS) +
           (float)(input & FXP_FRACTION_MASK) / (float)(1 << FXP_FRACTION_BITS);
}
inline bool tess_is_even(float input) { return (((int)input) & 1) == 0; }
inline uint tess_fxp_ceil(uint input) {
    if ((input & FXP_FRACTION_MASK) != 0) return (input & FXP_INTEGER_MASK) + FXP_ONE;
    return input;
}
inline uint tess_fxp_floor(uint input) { return input & FXP_INTEGER_MASK; }

inline uint tess_fixed_reciprocal(int n) {
    const uint table[65] = {
        0xffffffff,
        0x10000, 0x8000, 0x5555, 0x4000, 0x3333, 0x2aab, 0x2492, 0x2000,
        0x1c72, 0x199a, 0x1746, 0x1555, 0x13b1, 0x1249, 0x1111, 0x1000,
        0xf0f, 0xe39, 0xd79, 0xccd, 0xc31, 0xba3, 0xb21, 0xaab,
        0xa3d, 0x9d9, 0x97b, 0x925, 0x8d4, 0x889, 0x842, 0x800,
        0x7c2, 0x788, 0x750, 0x71c, 0x6eb, 0x6bd, 0x690, 0x666,
        0x63e, 0x618, 0x5f4, 0x5d1, 0x5b0, 0x591, 0x572, 0x555,
        0x539, 0x51f, 0x505, 0x4ec, 0x4d5, 0x4be, 0x4a8, 0x492,
        0x47e, 0x46a, 0x457, 0x444, 0x432, 0x421, 0x410, 0x400};
    return table[n < 0 ? 0 : (n > 64 ? 64 : n)];
}

inline int tess_remove_msb(int val) {
    int check;
    if (val <= 0x0000ffff) { check = (val <= 0x000000ff) ? 0x00000080 : 0x00008000; }
    else { check = (val <= 0x00ffffff) ? 0x00800000 : (int)0x80000000; }
    for (int i = 0; i < 8; i++, check = (int)((uint)check >> 1)) {
        if ((val & check) != 0) return (val & ~check);
    }
    return 0;
}

inline bool tess_odd(TESS_THREAD const TessState& s) { return s.parity == TESS_PARITY_ODD; }
inline bool tess_hw_integer(TESS_THREAD const TessState& s) {
    return s.partitioning == TESS_INTEGER || s.partitioning == TESS_POW2;
}

// --- output
inline void tess_define_point(TESS_THREAD TessState& s, uint fxpU, uint fxpV, int offset) {
    if (s.sink.write == 0) return;
    const uint at = (s.sink.point_base + (uint)offset) * 4;
    s.sink.points[at + 0] = (uint)TESS_AS_INT(tess_fixed_to_float(fxpU));
    s.sink.points[at + 1] = (uint)TESS_AS_INT(tess_fixed_to_float(fxpV));
    s.sink.points[at + 2] = s.sink.patch_index;
    s.sink.points[at + 3] = s.sink.primitive_id;
}
inline int tess_patch_index_value(TESS_THREAD const TessState& s, int index) {
    if (s.usingPatchedIndices != 0) {
        if (index >= s.outsidePointIndexPatchBase) {
            if (index == s.outsidePointIndexBadValue) {
                index = s.outsidePointIndexReplacementValue;
            } else {
                index += s.outsidePointIndexDeltaToRealValue;
            }
        } else {
            if (index == s.insidePointIndexBadValue) {
                index = s.insidePointIndexReplacementValue;
            } else {
                index += s.insidePointIndexDeltaToRealValue;
            }
        }
    } else if (s.usingPatchedIndices2 != 0) {
        if (index >= s.baseIndexToInvert) {
            if (index == s.cornerCaseBadValue) {
                index = s.cornerCaseReplacementValue;
            } else {
                index = s.indexInversionEndPoint - index;
            }
        } else if (index == s.cornerCaseBadValue) {
            index = s.cornerCaseReplacementValue;
        }
    }
    return index;
}
inline void tess_define_index(TESS_THREAD TessState& s, int index, int offset) {
    index = tess_patch_index_value(s, index);
    if (s.sink.write == 0) return;
    s.sink.indices[s.sink.index_base + (uint)offset] = s.sink.point_base + (uint)index;
    if (s.sink.write_primitive_ids != 0 &&
        (uint)offset % s.sink.vertices_per_primitive == 0) {
        s.sink.primitive_ids[(s.sink.index_base + (uint)offset) /
                             s.sink.vertices_per_primitive] = s.sink.primitive_id;
    }
}
inline void tess_define_clockwise_triangle(TESS_THREAD TessState& s, int index0, int index1,
                                           int index2, int base) {
    // inputs a clockwise triangle, stores a CW or CCW triangle depending on the state
    tess_define_index(s, index0, base);
    if (s.outputPrimitive == TESS_OUT_TRIANGLE_CW) {
        tess_define_index(s, index1, base + 1);
        tess_define_index(s, index2, base + 2);
    } else {
        tess_define_index(s, index2, base + 1);
        tess_define_index(s, index1, base + 2);
    }
}
inline void tess_dump_all_points(TESS_THREAD TessState& s) {
    for (int p = 0; p < s.numPoints; p++) tess_define_index(s, p, s.numIndices++);
}
inline void tess_dump_all_points_as_in_order_line_list(TESS_THREAD TessState& s) {
    for (int p = 1; p < s.numPoints; p++) {
        tess_define_index(s, p - 1, s.numIndices++);
        tess_define_index(s, p, s.numIndices++);
    }
}

// --- 1D placement
inline int tess_num_points_for_tess_factor(TESS_THREAD const TessState& s, uint fxpTessFactor) {
    if (tess_odd(s)) {
        return (int)((tess_fxp_ceil(FXP_ONE_HALF + (fxpTessFactor + 1) / 2) * 2) >> FXP_FRACTION_BITS);
    }
    return (int)((tess_fxp_ceil((fxpTessFactor + 1) / 2) * 2) >> FXP_FRACTION_BITS) + 1;
}
inline void tess_compute_tess_factor_context(TESS_THREAD const TessState& s, uint fxpTessFactor,
                                             TESS_THREAD TessFactorContext& ctx) {
    uint fxpHalfTessFactor = (fxpTessFactor + 1) / 2;
    if (tess_odd(s) || fxpHalfTessFactor == FXP_ONE_HALF) {
        fxpHalfTessFactor += FXP_ONE_HALF;
    }
    const uint fxpFloorHalfTessFactor = tess_fxp_floor(fxpHalfTessFactor);
    const uint fxpCeilHalfTessFactor = tess_fxp_ceil(fxpHalfTessFactor);
    ctx.fxpHalfTessFactorFraction = fxpHalfTessFactor - fxpFloorHalfTessFactor;
    ctx.numHalfTessFactorPoints = (int)(fxpCeilHalfTessFactor >> FXP_FRACTION_BITS);
    if (fxpCeilHalfTessFactor == fxpFloorHalfTessFactor) {
        ctx.splitPointOnFloorHalfTessFactor = ctx.numHalfTessFactorPoints + 1;
    } else if (tess_odd(s)) {
        if (fxpFloorHalfTessFactor == FXP_ONE) {
            ctx.splitPointOnFloorHalfTessFactor = 0;
        } else {
            ctx.splitPointOnFloorHalfTessFactor =
                (tess_remove_msb((int)(fxpFloorHalfTessFactor >> FXP_FRACTION_BITS) - 1) << 1) + 1;
        }
    } else {
        ctx.splitPointOnFloorHalfTessFactor =
            (tess_remove_msb((int)(fxpFloorHalfTessFactor >> FXP_FRACTION_BITS)) << 1) + 1;
    }
    int numFloorSegments = (int)((fxpFloorHalfTessFactor * 2) >> FXP_FRACTION_BITS);
    int numCeilSegments = (int)((fxpCeilHalfTessFactor * 2) >> FXP_FRACTION_BITS);
    if (tess_odd(s)) {
        numFloorSegments -= 1;
        numCeilSegments -= 1;
    }
    ctx.fxpInvNumSegmentsOnFloorTessFactor = tess_fixed_reciprocal(numFloorSegments);
    ctx.fxpInvNumSegmentsOnCeilTessFactor = tess_fixed_reciprocal(numCeilSegments);
}
inline uint tess_place_point_in_1d(TESS_THREAD const TessState& s,
                                   TESS_THREAD const TessFactorContext& ctx, int point) {
    bool flip;
    if (point >= ctx.numHalfTessFactorPoints) {
        point = (ctx.numHalfTessFactorPoints << 1) - point;
        if (tess_odd(s)) point -= 1;
        flip = true;
    } else {
        flip = false;
    }
    if (point == ctx.numHalfTessFactorPoints) {
        return FXP_ONE_HALF;  // special casing middle since 16 bit fixed math can't reproduce 0.5 exactly
    }
    const uint indexOnCeilHalfTessFactor = (uint)point;
    uint indexOnFloorHalfTessFactor = indexOnCeilHalfTessFactor;
    if (point > ctx.splitPointOnFloorHalfTessFactor) indexOnFloorHalfTessFactor -= 1;
    const uint fxpLocationOnFloorHalfTessFactor =
        indexOnFloorHalfTessFactor * ctx.fxpInvNumSegmentsOnFloorTessFactor;
    const uint fxpLocationOnCeilHalfTessFactor =
        indexOnCeilHalfTessFactor * ctx.fxpInvNumSegmentsOnCeilTessFactor;
    uint fxpLocation = fxpLocationOnFloorHalfTessFactor * (FXP_ONE - ctx.fxpHalfTessFactorFraction) +
                       fxpLocationOnCeilHalfTessFactor * ctx.fxpHalfTessFactorFraction;
    fxpLocation = (fxpLocation + FXP_ONE_HALF) >> FXP_FRACTION_BITS;
    if (flip) fxpLocation = FXP_ONE - fxpLocation;
    return fxpLocation;
}

// --- stitching
constant_int TESS_DIAGONALS_INSIDE_TO_OUTSIDE = 0;
constant_int TESS_DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE = 1;
constant_int TESS_DIAGONALS_MIRRORED = 2;

inline void tess_stitch_regular(TESS_THREAD TessState& s, bool trapezoid, int diagonals, int base,
                                int numInsideEdgePoints, int insideEdgePointBaseOffset,
                                int outsideEdgePointBaseOffset) {
    int insidePoint = insideEdgePointBaseOffset;
    int outsidePoint = outsideEdgePointBaseOffset;
    if (trapezoid) {
        tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
        base += 3; outsidePoint++;
    }
    int p;
    if (diagonals == TESS_DIAGONALS_INSIDE_TO_OUTSIDE) {
        for (p = 0; p < numInsideEdgePoints - 1; p++) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, outsidePoint + 1, base);
            base += 3;
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint + 1, insidePoint + 1, base);
            base += 3;
            insidePoint++; outsidePoint++;
        }
    } else if (diagonals == TESS_DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE) {
        for (p = 0; p < numInsideEdgePoints / 2 - 1; p++) {
            tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
            base += 3;
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint + 1, insidePoint + 1, base);
            base += 3;
            insidePoint++; outsidePoint++;
        }
        tess_define_clockwise_triangle(s, outsidePoint, insidePoint + 1, insidePoint, base);
        base += 3;
        tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint + 1, base);
        base += 3;
        insidePoint++; outsidePoint++; p += 2;
        for (; p < numInsideEdgePoints; p++) {
            tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
            base += 3;
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint + 1, insidePoint + 1, base);
            base += 3;
            insidePoint++; outsidePoint++;
        }
    } else {
        for (p = 0; p < numInsideEdgePoints / 2; p++) {
            tess_define_clockwise_triangle(s, outsidePoint, insidePoint + 1, insidePoint, base);
            base += 3;
            tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint + 1, base);
            base += 3;
            insidePoint++; outsidePoint++;
        }
        for (; p < numInsideEdgePoints - 1; p++) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, outsidePoint + 1, base);
            base += 3;
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint + 1, insidePoint + 1, base);
            base += 3;
            insidePoint++; outsidePoint++;
        }
    }
    if (trapezoid) {
        tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
        base += 3;
    }
}

inline void tess_stitch_transition(TESS_THREAD TessState& s, int base,
                                   int insideEdgePointBaseOffset, int insideNumHalfTessFactorPoints,
                                   int insideEdgeTessFactorParity,
                                   int outsideEdgePointBaseOffset, int outsideNumHalfTessFactorPoints,
                                   int outsideTessFactorParity) {
    // The ruler-function split order of a half edge (see the reference).
    const int finalPointPositionTable[33] = {
        0, 32, 16, 8, 17, 4, 18, 9, 19, 2, 20, 10, 21, 5, 22, 11, 23,
        1, 24, 12, 25, 6, 26, 13, 27, 3, 28, 14, 29, 7, 30, 15, 31};
    const int loopStart[33] = {1,1,17,9,9,5,5,5,5,3,3,3,3,3,3,3,3,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
    const int loopEnd[33] = {0,0,17,17,25,25,25,25,29,29,29,29,29,29,29,29,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,32};
    if (insideEdgeTessFactorParity == TESS_PARITY_ODD) insideNumHalfTessFactorPoints -= 1;
    if (outsideTessFactorParity == TESS_PARITY_ODD) outsideNumHalfTessFactorPoints -= 1;
    int outsidePoint = outsideEdgePointBaseOffset;
    int insidePoint = insideEdgePointBaseOffset;
    const int iStart = tess_min(loopStart[insideNumHalfTessFactorPoints],
                                loopStart[outsideNumHalfTessFactorPoints]);
    const int iEnd = tess_max(loopEnd[insideNumHalfTessFactorPoints],
                              loopEnd[outsideNumHalfTessFactorPoints]);
    if (finalPointPositionTable[0] < outsideNumHalfTessFactorPoints) {
        tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
        base += 3; outsidePoint++;
    }
    for (int i = iStart; i <= iEnd; i++) {
        if (finalPointPositionTable[i] < insideNumHalfTessFactorPoints) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, insidePoint + 1, base);
            base += 3; insidePoint++;
        }
        if (finalPointPositionTable[i] < outsideNumHalfTessFactorPoints) {
            tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
            base += 3; outsidePoint++;
        }
    }
    if (insideEdgeTessFactorParity != outsideTessFactorParity ||
        insideEdgeTessFactorParity == TESS_PARITY_ODD) {
        if (insideEdgeTessFactorParity == outsideTessFactorParity) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, insidePoint + 1, base);
            base += 3;
            tess_define_clockwise_triangle(s, insidePoint + 1, outsidePoint, outsidePoint + 1, base);
            base += 3;
            insidePoint++;
            outsidePoint++;
        } else if (insideEdgeTessFactorParity == TESS_PARITY_EVEN) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, outsidePoint + 1, base);
            base += 3;
            outsidePoint++;
        } else {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, insidePoint + 1, base);
            base += 3;
            insidePoint++;
        }
    }
    for (int i = iEnd; i >= iStart; i--) {
        if (finalPointPositionTable[i] < outsideNumHalfTessFactorPoints) {
            tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
            base += 3; outsidePoint++;
        }
        if (finalPointPositionTable[i] < insideNumHalfTessFactorPoints) {
            tess_define_clockwise_triangle(s, insidePoint, outsidePoint, insidePoint + 1, base);
            base += 3; insidePoint++;
        }
    }
    if (finalPointPositionTable[0] < outsideNumHalfTessFactorPoints) {
        tess_define_clockwise_triangle(s, outsidePoint, outsidePoint + 1, insidePoint, base);
        base += 3; outsidePoint++;
    }
}

// --- clamps shared by the domains
inline void tess_edge_bounds(TESS_THREAD const TessState& s, TESS_THREAD float& lowerBound,
                             TESS_THREAD float& upperBound) {
    if (s.originalPartitioning == TESS_FRACTIONAL_EVEN) {
        lowerBound = 2; upperBound = 64;
    } else if (s.originalPartitioning == TESS_FRACTIONAL_ODD) {
        lowerBound = 1; upperBound = 63;
    } else {  // integer, pow2 (hardware treats pow2 as integer)
        lowerBound = 1; upperBound = 64;
    }
}
constant_float TESS_EPSILON = 0.0000152587890625f;  // 2^(-16)
constant_float TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON = 1 + 0.0000152587890625f / 2;

// ============================== quad
struct TessQuadFactors {
    uint outsideTessFactor[4];
    uint insideTessFactor[2];
    int outsideTessFactorParity[4];
    int insideTessFactorParity[2];
    TessFactorContext outsideTessFactorCtx[4];
    TessFactorContext insideTessFactorCtx[2];
    int justDoMinimumTessFactor;
    int patchCulled;
    int numPointsForOutsideEdge[4];
    int numPointsForInsideTessFactor[2];
    int insideEdgePointBaseOffset;
};

inline void tess_quad_process(TESS_THREAD TessState& s, float tf0, float tf1, float tf2, float tf3,
                              float insideU, float insideV, TESS_THREAD TessQuadFactors& p) {
    if (!(tf0 > 0) || !(tf1 > 0) || !(tf2 > 0) || !(tf3 > 0)) {  // NaN will pass
        p.patchCulled = 1;
        return;
    }
    p.patchCulled = 0;
    float lowerBound, upperBound;
    tess_edge_bounds(s, lowerBound, upperBound);
    tf0 = tess_fmin(upperBound, tess_fmax(lowerBound, tf0));
    tf1 = tess_fmin(upperBound, tess_fmax(lowerBound, tf1));
    tf2 = tess_fmin(upperBound, tess_fmax(lowerBound, tf2));
    tf3 = tess_fmin(upperBound, tess_fmax(lowerBound, tf3));
    if (tess_hw_integer(s)) {
        tf0 = ceil(tf0); tf1 = ceil(tf1); tf2 = ceil(tf2); tf3 = ceil(tf3);
    }
    if (s.originalPartitioning == TESS_FRACTIONAL_ODD) {
        if (tf0 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            tf1 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            tf2 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            tf3 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            insideU > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            insideV > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON) {
            lowerBound = 1 + TESS_EPSILON;  // force picture frame
        }
    }
    insideU = tess_fmin(upperBound, tess_fmax(lowerBound, insideU));
    insideV = tess_fmin(upperBound, tess_fmax(lowerBound, insideV));
    if (tess_hw_integer(s)) {
        insideU = ceil(insideU); insideV = ceil(insideV);
    }
    s.numPoints = 0;
    s.numIndices = 0;
    const float outside[4] = {tf0, tf1, tf2, tf3};
    const float inside[2] = {insideU, insideV};
    for (int e = 0; e < 4; e++) {
        p.outsideTessFactorParity[e] = tess_hw_integer(s)
            ? (tess_is_even(outside[e]) ? TESS_PARITY_EVEN : TESS_PARITY_ODD)
            : s.originalParity;
        p.outsideTessFactor[e] = tess_float_to_fixed(outside[e]);
    }
    for (int a = 0; a < 2; a++) {
        p.insideTessFactorParity[a] = tess_hw_integer(s)
            ? ((tess_is_even(inside[a]) || inside[a] == 1.0f) ? TESS_PARITY_EVEN : TESS_PARITY_ODD)
            : s.originalParity;
        p.insideTessFactor[a] = tess_float_to_fixed(inside[a]);
    }
    if (tess_hw_integer(s) || tess_odd(s)) {
        if (p.insideTessFactor[0] == FXP_ONE && p.insideTessFactor[1] == FXP_ONE &&
            p.outsideTessFactor[0] == FXP_ONE && p.outsideTessFactor[1] == FXP_ONE &&
            p.outsideTessFactor[2] == FXP_ONE && p.outsideTessFactor[3] == FXP_ONE) {
            p.justDoMinimumTessFactor = 1;
            return;
        }
    }
    p.justDoMinimumTessFactor = 0;
    for (int e = 0; e < 4; e++) {
        s.parity = p.outsideTessFactorParity[e];
        tess_compute_tess_factor_context(s, p.outsideTessFactor[e], p.outsideTessFactorCtx[e]);
    }
    for (int a = 0; a < 2; a++) {
        s.parity = p.insideTessFactorParity[a];
        tess_compute_tess_factor_context(s, p.insideTessFactor[a], p.insideTessFactorCtx[a]);
    }
    for (int e = 0; e < 4; e++) {
        s.parity = p.outsideTessFactorParity[e];
        p.numPointsForOutsideEdge[e] = tess_num_points_for_tess_factor(s, p.outsideTessFactor[e]);
        s.numPoints += p.numPointsForOutsideEdge[e];
    }
    s.numPoints -= 4;
    for (int a = 0; a < 2; a++) {
        s.parity = p.insideTessFactorParity[a];
        p.numPointsForInsideTessFactor[a] = tess_num_points_for_tess_factor(s, p.insideTessFactor[a]);
        const int pointCountMin = (p.insideTessFactorParity[a] == TESS_PARITY_ODD) ? 4 : 3;
        p.numPointsForInsideTessFactor[a] = tess_max(pointCountMin, p.numPointsForInsideTessFactor[a]);
    }
    p.insideEdgePointBaseOffset = s.numPoints;
    s.numPoints += (p.numPointsForInsideTessFactor[0] - 2) * (p.numPointsForInsideTessFactor[1] - 2);
}

inline void tess_quad_generate_points(TESS_THREAD TessState& s, TESS_THREAD const TessQuadFactors& p) {
    int pointOffset = 0;
    for (int edge = 0; edge < 4; edge++) {
        const int parity = edge & 0x1;
        const int endPoint = p.numPointsForOutsideEdge[edge] - 1;
        for (int q0 = 0; q0 < endPoint; q0++, pointOffset++) {
            const int q = (edge == 1 || edge == 2) ? q0 : endPoint - q0;
            s.parity = p.outsideTessFactorParity[edge];
            const uint fxpParam = tess_place_point_in_1d(s, p.outsideTessFactorCtx[edge], q);
            if (parity != 0) {
                tess_define_point(s, fxpParam, (edge == 3) ? FXP_ONE : 0, pointOffset);
            } else {
                tess_define_point(s, (edge == 2) ? FXP_ONE : 0, fxpParam, pointOffset);
            }
        }
    }
    const int minNumPointsForTessFactor =
        tess_min(p.numPointsForInsideTessFactor[0], p.numPointsForInsideTessFactor[1]);
    const int numRings = minNumPointsForTessFactor >> 1;
    for (int ring = 1; ring < numRings; ring++) {
        const int startPoint = ring;
        const int endPoint[2] = {p.numPointsForInsideTessFactor[0] - 1 - startPoint,
                                 p.numPointsForInsideTessFactor[1] - 1 - startPoint};
        for (int edge = 0; edge < 4; edge++) {
            const int parity0 = edge & 0x1;
            const int parity1 = (edge + 1) & 0x1;
            const int perpendicularAxisPoint = (edge < 2) ? startPoint : endPoint[parity0];
            s.parity = p.insideTessFactorParity[parity0];
            const uint fxpPerpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx[parity0], perpendicularAxisPoint);
            s.parity = p.insideTessFactorParity[parity1];
            for (int q0 = startPoint; q0 < endPoint[parity1]; q0++, pointOffset++) {
                const int q = (edge == 1 || edge == 2) ? q0 : endPoint[parity1] - (q0 - startPoint);
                const uint fxpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx[parity1], q);
                if (parity1 != 0) {
                    tess_define_point(s, fxpPerpParam, fxpParam, pointOffset);
                } else {
                    tess_define_point(s, fxpParam, fxpPerpParam, pointOffset);
                }
            }
        }
    }
    // For even tessellation, the inner "ring" is degenerate - a row of points
    if (p.numPointsForInsideTessFactor[0] > p.numPointsForInsideTessFactor[1] &&
        p.insideTessFactorParity[1] == TESS_PARITY_EVEN) {
        const int startPoint = numRings;
        const int endPoint = p.numPointsForInsideTessFactor[0] - 1 - startPoint;
        s.parity = p.insideTessFactorParity[0];
        for (int q = startPoint; q <= endPoint; q++, pointOffset++) {
            const uint fxpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx[0], q);
            tess_define_point(s, fxpParam, FXP_ONE_HALF, pointOffset);
        }
    } else if (p.numPointsForInsideTessFactor[1] >= p.numPointsForInsideTessFactor[0] &&
               p.insideTessFactorParity[0] == TESS_PARITY_EVEN) {
        const int startPoint = numRings;
        const int endPoint = p.numPointsForInsideTessFactor[1] - 1 - startPoint;
        s.parity = p.insideTessFactorParity[1];
        for (int q = endPoint; q >= startPoint; q--, pointOffset++) {
            const uint fxpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx[1], q);
            tess_define_point(s, FXP_ONE_HALF, fxpParam, pointOffset);
        }
    }
}

inline void tess_quad_generate_connectivity(TESS_THREAD TessState& s, TESS_THREAD const TessQuadFactors& p) {
    const int numPointRowsToCenter[2] = {(p.numPointsForInsideTessFactor[0] + 1) >> 1,
                                         (p.numPointsForInsideTessFactor[1] + 1) >> 1};
    const int numRings = tess_min(numPointRowsToCenter[0], numPointRowsToCenter[1]);
    const int degeneratePointRing[2] = {
        (p.insideTessFactorParity[1] == TESS_PARITY_EVEN) ? numPointRowsToCenter[1] - 1 : -1,
        (p.insideTessFactorParity[0] == TESS_PARITY_EVEN) ? numPointRowsToCenter[0] - 1 : -1};
    // Which context each edge stitches against: the outside edge's own on
    // the first ring, the inside axis' after (the reference's pointer array).
    int ctxIsInside = 0;
    int outsideTessFactorParity[4] = {p.outsideTessFactorParity[0], p.outsideTessFactorParity[1],
                                      p.outsideTessFactorParity[2], p.outsideTessFactorParity[3]};
    int numPointsForOutsideEdge[4] = {p.numPointsForOutsideEdge[0], p.numPointsForOutsideEdge[1],
                                      p.numPointsForOutsideEdge[2], p.numPointsForOutsideEdge[3]};
    int insideEdgePointBaseOffset = p.insideEdgePointBaseOffset;
    int outsideEdgePointBaseOffset = 0;
    for (int ring = 1; ring < numRings; ring++) {
        const int numPointsForInsideEdge[2] = {p.numPointsForInsideTessFactor[0] - 2 * ring,
                                               p.numPointsForInsideTessFactor[1] - 2 * ring};
        const int edge0InsidePointBaseOffset = insideEdgePointBaseOffset;
        const int edge0OutsidePointBaseOffset = outsideEdgePointBaseOffset;
        for (int edge = 0; edge < 4; edge++) {
            const int parity = (edge + 1) & 0x1;
            const int numTriangles = numPointsForInsideEdge[parity] + numPointsForOutsideEdge[edge] - 2;
            int insideBaseOffset;
            int outsideBaseOffset;
            if (edge == 3) {
                if (ring == degeneratePointRing[parity]) {
                    s.baseIndexToInvert = insideEdgePointBaseOffset + 1;
                    s.cornerCaseBadValue = outsideEdgePointBaseOffset + numPointsForOutsideEdge[edge] - 1;
                    s.cornerCaseReplacementValue = edge0OutsidePointBaseOffset;
                    s.indexInversionEndPoint = (s.baseIndexToInvert << 1) - 1;
                    insideBaseOffset = s.baseIndexToInvert;
                    outsideBaseOffset = outsideEdgePointBaseOffset;
                    s.usingPatchedIndices2 = 1;
                } else {
                    s.insidePointIndexDeltaToRealValue = insideEdgePointBaseOffset;
                    s.insidePointIndexBadValue = numPointsForInsideEdge[parity] - 1;
                    s.insidePointIndexReplacementValue = edge0InsidePointBaseOffset;
                    s.outsidePointIndexPatchBase = s.insidePointIndexBadValue + 1;
                    s.outsidePointIndexDeltaToRealValue = outsideEdgePointBaseOffset - s.outsidePointIndexPatchBase;
                    s.outsidePointIndexBadValue = s.outsidePointIndexPatchBase + numPointsForOutsideEdge[edge] - 1;
                    s.outsidePointIndexReplacementValue = edge0OutsidePointBaseOffset;
                    insideBaseOffset = 0;
                    outsideBaseOffset = s.outsidePointIndexPatchBase;
                    s.usingPatchedIndices = 1;
                }
            } else if (edge == 2 && ring == degeneratePointRing[parity]) {
                s.baseIndexToInvert = insideEdgePointBaseOffset;
                s.cornerCaseBadValue = -1;
                s.cornerCaseReplacementValue = -1;
                s.indexInversionEndPoint = s.baseIndexToInvert << 1;
                insideBaseOffset = s.baseIndexToInvert;
                outsideBaseOffset = outsideEdgePointBaseOffset;
                s.usingPatchedIndices2 = 1;
            } else {
                insideBaseOffset = insideEdgePointBaseOffset;
                outsideBaseOffset = outsideEdgePointBaseOffset;
            }
            if (ring == 1) {
                const int outsideHalf = ctxIsInside != 0
                    ? p.insideTessFactorCtx[edge & 1].numHalfTessFactorPoints
                    : p.outsideTessFactorCtx[edge].numHalfTessFactorPoints;
                tess_stitch_transition(s, s.numIndices,
                                       insideBaseOffset, p.insideTessFactorCtx[parity].numHalfTessFactorPoints,
                                       p.insideTessFactorParity[parity],
                                       outsideBaseOffset, outsideHalf, outsideTessFactorParity[edge]);
            } else {
                tess_stitch_regular(s, true, TESS_DIAGONALS_MIRRORED, s.numIndices,
                                    numPointsForInsideEdge[parity], insideBaseOffset, outsideBaseOffset);
            }
            s.usingPatchedIndices = 0;
            s.usingPatchedIndices2 = 0;
            s.numIndices += numTriangles * 3;
            outsideEdgePointBaseOffset += numPointsForOutsideEdge[edge] - 1;
            if (edge == 2 && ring == degeneratePointRing[parity]) {
                insideEdgePointBaseOffset -= numPointsForInsideEdge[parity] - 1;
            } else {
                insideEdgePointBaseOffset += numPointsForInsideEdge[parity] - 1;
            }
            numPointsForOutsideEdge[edge] = numPointsForInsideEdge[parity];
        }
        if (ring == 1) {
            ctxIsInside = 1;
            for (int edge = 0; edge < 4; edge++) {
                outsideTessFactorParity[edge] = p.insideTessFactorParity[edge & 1];
            }
        }
    }
    // Triangulate center - a row of quads if odd
    if (p.numPointsForInsideTessFactor[0] > p.numPointsForInsideTessFactor[1] &&
        p.insideTessFactorParity[1] == TESS_PARITY_ODD) {
        s.usingPatchedIndices2 = 1;
        const int stripNumQuads =
            (((p.numPointsForInsideTessFactor[0] >> 1) - (p.numPointsForInsideTessFactor[1] >> 1)) << 1) +
            ((p.insideTessFactorParity[0] == TESS_PARITY_EVEN) ? 2 : 1);
        s.baseIndexToInvert = outsideEdgePointBaseOffset + stripNumQuads + 2;
        s.cornerCaseBadValue = s.baseIndexToInvert;
        s.cornerCaseReplacementValue = outsideEdgePointBaseOffset;
        s.indexInversionEndPoint = s.baseIndexToInvert + s.baseIndexToInvert + stripNumQuads;
        tess_stitch_regular(s, false, TESS_DIAGONALS_INSIDE_TO_OUTSIDE, s.numIndices, stripNumQuads + 1,
                            s.baseIndexToInvert, outsideEdgePointBaseOffset + 1);
        s.usingPatchedIndices2 = 0;
        s.numIndices += stripNumQuads * 6;
    } else if (p.numPointsForInsideTessFactor[1] >= p.numPointsForInsideTessFactor[0] &&
               p.insideTessFactorParity[0] == TESS_PARITY_ODD) {
        s.usingPatchedIndices2 = 1;
        const int stripNumQuads =
            (((p.numPointsForInsideTessFactor[1] >> 1) - (p.numPointsForInsideTessFactor[0] >> 1)) << 1) +
            ((p.insideTessFactorParity[1] == TESS_PARITY_EVEN) ? 2 : 1);
        s.baseIndexToInvert = outsideEdgePointBaseOffset + stripNumQuads + 1;
        s.cornerCaseBadValue = -1;
        s.indexInversionEndPoint = s.baseIndexToInvert + s.baseIndexToInvert + stripNumQuads;
        const int diag = (p.insideTessFactorParity[1] == TESS_PARITY_EVEN)
            ? TESS_DIAGONALS_INSIDE_TO_OUTSIDE : TESS_DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE;
        tess_stitch_regular(s, false, diag, s.numIndices, stripNumQuads + 1,
                            s.baseIndexToInvert, outsideEdgePointBaseOffset);
        s.usingPatchedIndices2 = 0;
        s.numIndices += stripNumQuads * 6;
    }
}

inline void tess_quad(TESS_THREAD TessState& s, float tf0, float tf1, float tf2, float tf3,
                      float insideU, float insideV) {
    TessQuadFactors p;
    tess_quad_process(s, tf0, tf1, tf2, tf3, insideU, insideV, p);
    if (p.patchCulled != 0) {
        s.numPoints = 0;
        s.numIndices = 0;
        return;
    }
    if (p.justDoMinimumTessFactor != 0) {
        tess_define_point(s, 0, 0, 0);
        tess_define_point(s, FXP_ONE, 0, 1);
        tess_define_point(s, FXP_ONE, FXP_ONE, 2);
        tess_define_point(s, 0, FXP_ONE, 3);
        s.numPoints = 4;
        if (s.outputPrimitive == TESS_OUT_TRIANGLE_CW || s.outputPrimitive == TESS_OUT_TRIANGLE_CCW) {
            tess_define_clockwise_triangle(s, 0, 1, 3, 0);
            tess_define_clockwise_triangle(s, 1, 2, 3, 3);
            s.numIndices = 6;
        } else if (s.outputPrimitive == TESS_OUT_POINT) {
            tess_dump_all_points(s);
        } else {
            tess_dump_all_points_as_in_order_line_list(s);
        }
        return;
    }
    tess_quad_generate_points(s, p);
    if (s.outputPrimitive == TESS_OUT_POINT) { tess_dump_all_points(s); return; }
    if (s.outputPrimitive == TESS_OUT_LINE) { tess_dump_all_points_as_in_order_line_list(s); return; }
    tess_quad_generate_connectivity(s, p);
}

// ============================== tri
struct TessTriFactors {
    uint outsideTessFactor[3];
    uint insideTessFactor;
    int outsideTessFactorParity[3];
    int insideTessFactorParity;
    TessFactorContext outsideTessFactorCtx[3];
    TessFactorContext insideTessFactorCtx;
    int justDoMinimumTessFactor;
    int patchCulled;
    int numPointsForOutsideEdge[3];
    int numPointsForInsideTessFactor;
    int insideEdgePointBaseOffset;
};

inline void tess_tri_process(TESS_THREAD TessState& s, float tf0, float tf1, float tf2, float inside,
                             TESS_THREAD TessTriFactors& p) {
    if (!(tf0 > 0) || !(tf1 > 0) || !(tf2 > 0)) {
        p.patchCulled = 1;
        return;
    }
    p.patchCulled = 0;
    float lowerBound, upperBound;
    tess_edge_bounds(s, lowerBound, upperBound);
    tf0 = tess_fmin(upperBound, tess_fmax(lowerBound, tf0));
    tf1 = tess_fmin(upperBound, tess_fmax(lowerBound, tf1));
    tf2 = tess_fmin(upperBound, tess_fmax(lowerBound, tf2));
    if (tess_hw_integer(s)) {
        tf0 = ceil(tf0); tf1 = ceil(tf1); tf2 = ceil(tf2);
    }
    if (s.originalPartitioning == TESS_FRACTIONAL_ODD) {
        if (tf0 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            tf1 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON ||
            tf2 > TESS_MIN_ODD_TESSFACTOR_PLUS_HALF_EPSILON) {
            lowerBound = 1 + TESS_EPSILON;
        }
    }
    inside = tess_fmin(upperBound, tess_fmax(lowerBound, inside));
    if (tess_hw_integer(s)) inside = ceil(inside);
    s.numPoints = 0;
    s.numIndices = 0;
    const float outside[3] = {tf0, tf1, tf2};
    for (int e = 0; e < 3; e++) {
        p.outsideTessFactorParity[e] = tess_hw_integer(s)
            ? (tess_is_even(outside[e]) ? TESS_PARITY_EVEN : TESS_PARITY_ODD)
            : s.originalParity;
        p.outsideTessFactor[e] = tess_float_to_fixed(outside[e]);
    }
    p.insideTessFactorParity = tess_hw_integer(s)
        ? ((tess_is_even(inside) || inside == 1.0f) ? TESS_PARITY_EVEN : TESS_PARITY_ODD)
        : s.originalParity;
    p.insideTessFactor = tess_float_to_fixed(inside);
    if (tess_hw_integer(s) || tess_odd(s)) {
        if (p.insideTessFactor == FXP_ONE && p.outsideTessFactor[0] == FXP_ONE &&
            p.outsideTessFactor[1] == FXP_ONE && p.outsideTessFactor[2] == FXP_ONE) {
            p.justDoMinimumTessFactor = 1;
            return;
        }
    }
    p.justDoMinimumTessFactor = 0;
    for (int e = 0; e < 3; e++) {
        s.parity = p.outsideTessFactorParity[e];
        tess_compute_tess_factor_context(s, p.outsideTessFactor[e], p.outsideTessFactorCtx[e]);
    }
    s.parity = p.insideTessFactorParity;
    tess_compute_tess_factor_context(s, p.insideTessFactor, p.insideTessFactorCtx);
    for (int e = 0; e < 3; e++) {
        s.parity = p.outsideTessFactorParity[e];
        p.numPointsForOutsideEdge[e] = tess_num_points_for_tess_factor(s, p.outsideTessFactor[e]);
        s.numPoints += p.numPointsForOutsideEdge[e];
    }
    s.numPoints -= 3;
    s.parity = p.insideTessFactorParity;
    p.numPointsForInsideTessFactor = tess_num_points_for_tess_factor(s, p.insideTessFactor);
    {
        const int pointCountMin = tess_odd(s) ? 4 : 3;
        p.numPointsForInsideTessFactor = tess_max(pointCountMin, p.numPointsForInsideTessFactor);
    }
    p.insideEdgePointBaseOffset = s.numPoints;
    {
        const int numInteriorRings = (p.numPointsForInsideTessFactor >> 1) - 1;
        int numInteriorPoints;
        if (tess_odd(s)) {
            numInteriorPoints = 3 * (numInteriorRings * (numInteriorRings + 1) - numInteriorRings);
        } else {
            numInteriorPoints = 3 * (numInteriorRings * (numInteriorRings + 1)) + 1;
        }
        s.numPoints += numInteriorPoints;
    }
}

inline void tess_tri_generate_points(TESS_THREAD TessState& s, TESS_THREAD const TessTriFactors& p) {
    int pointOffset = 0;
    for (int edge = 0; edge < 3; edge++) {
        const int parity = edge & 0x1;
        const int endPoint = p.numPointsForOutsideEdge[edge] - 1;
        for (int q0 = 0; q0 < endPoint; q0++, pointOffset++) {
            const int q = (parity != 0) ? q0 : endPoint - q0;
            s.parity = p.outsideTessFactorParity[edge];
            const uint fxpParam = tess_place_point_in_1d(s, p.outsideTessFactorCtx[edge], q);
            if (edge == 0) {
                tess_define_point(s, 0, fxpParam, pointOffset);
            } else {
                tess_define_point(s, fxpParam, (edge == 2) ? FXP_ONE - fxpParam : 0, pointOffset);
            }
        }
    }
    s.parity = p.insideTessFactorParity;
    const int numRings = p.numPointsForInsideTessFactor >> 1;
    for (int ring = 1; ring < numRings; ring++) {
        const int startPoint = ring;
        const int endPoint = p.numPointsForInsideTessFactor - 1 - startPoint;
        for (int edge = 0; edge < 3; edge++) {
            const int parity = edge & 0x1;
            uint fxpPerpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx, startPoint);
            fxpPerpParam *= FXP_TWO_THIRDS;  // map location to the right size in barycentric space
            fxpPerpParam = (fxpPerpParam + FXP_ONE_HALF) >> FXP_FRACTION_BITS;
            for (int q0 = startPoint; q0 < endPoint; q0++, pointOffset++) {
                const int q = (parity != 0) ? q0 : endPoint - (q0 - startPoint);
                const uint fxpParam = tess_place_point_in_1d(s, p.insideTessFactorCtx, q);
                const uint deriv = 2;
                if (edge == 0) {
                    tess_define_point(s, fxpPerpParam, fxpParam - (fxpPerpParam + 1) / deriv, pointOffset);
                } else if (edge == 1) {
                    tess_define_point(s, fxpParam - (fxpPerpParam + 1) / deriv, fxpPerpParam, pointOffset);
                } else {
                    tess_define_point(s, fxpParam - (fxpPerpParam + 1) / deriv,
                                      FXP_ONE - (fxpParam - (fxpPerpParam + 1) / deriv) - fxpPerpParam,
                                      pointOffset);
                }
            }
        }
    }
    if (!tess_odd(s)) {
        tess_define_point(s, FXP_ONE_THIRD, FXP_ONE_THIRD, pointOffset);  // the center
    }
}

inline void tess_tri_generate_connectivity(TESS_THREAD TessState& s, TESS_THREAD const TessTriFactors& p) {
    const int numRings = (p.numPointsForInsideTessFactor + 1) >> 1;
    int ctxIsInside = 0;
    int outsideTessFactorParity[3] = {p.outsideTessFactorParity[0], p.outsideTessFactorParity[1],
                                      p.outsideTessFactorParity[2]};
    int numPointsForOutsideEdge[3] = {p.numPointsForOutsideEdge[0], p.numPointsForOutsideEdge[1],
                                      p.numPointsForOutsideEdge[2]};
    int insideEdgePointBaseOffset = p.insideEdgePointBaseOffset;
    int outsideEdgePointBaseOffset = 0;
    for (int ring = 1; ring < numRings; ring++) {
        const int numPointsForInsideEdge = p.numPointsForInsideTessFactor - 2 * ring;
        const int edge0InsidePointBaseOffset = insideEdgePointBaseOffset;
        const int edge0OutsidePointBaseOffset = outsideEdgePointBaseOffset;
        for (int edge = 0; edge < 3; edge++) {
            const int numTriangles = numPointsForInsideEdge + numPointsForOutsideEdge[edge] - 2;
            int insideBaseOffset;
            int outsideBaseOffset;
            if (edge == 2) {
                s.insidePointIndexDeltaToRealValue = insideEdgePointBaseOffset;
                s.insidePointIndexBadValue = numPointsForInsideEdge - 1;
                s.insidePointIndexReplacementValue = edge0InsidePointBaseOffset;
                s.outsidePointIndexPatchBase = s.insidePointIndexBadValue + 1;
                s.outsidePointIndexDeltaToRealValue = outsideEdgePointBaseOffset - s.outsidePointIndexPatchBase;
                s.outsidePointIndexBadValue = s.outsidePointIndexPatchBase + numPointsForOutsideEdge[edge] - 1;
                s.outsidePointIndexReplacementValue = edge0OutsidePointBaseOffset;
                s.usingPatchedIndices = 1;
                insideBaseOffset = 0;
                outsideBaseOffset = s.outsidePointIndexPatchBase;
            } else {
                insideBaseOffset = insideEdgePointBaseOffset;
                outsideBaseOffset = outsideEdgePointBaseOffset;
            }
            if (ring == 1) {
                const int outsideHalf = ctxIsInside != 0
                    ? p.insideTessFactorCtx.numHalfTessFactorPoints
                    : p.outsideTessFactorCtx[edge].numHalfTessFactorPoints;
                tess_stitch_transition(s, s.numIndices,
                                       insideBaseOffset, p.insideTessFactorCtx.numHalfTessFactorPoints,
                                       p.insideTessFactorParity,
                                       outsideBaseOffset, outsideHalf, outsideTessFactorParity[edge]);
            } else {
                tess_stitch_regular(s, true, TESS_DIAGONALS_MIRRORED, s.numIndices,
                                    numPointsForInsideEdge, insideBaseOffset, outsideBaseOffset);
            }
            if (edge == 2) s.usingPatchedIndices = 0;
            s.numIndices += numTriangles * 3;
            outsideEdgePointBaseOffset += numPointsForOutsideEdge[edge] - 1;
            insideEdgePointBaseOffset += numPointsForInsideEdge - 1;
            numPointsForOutsideEdge[edge] = numPointsForInsideEdge;
        }
        if (ring == 1) {
            ctxIsInside = 1;
            for (int edge = 0; edge < 3; edge++) {
                outsideTessFactorParity[edge] = p.insideTessFactorParity;
            }
        }
    }
    if (tess_odd(s)) {
        // Triangulate center (a single triangle)
        tess_define_clockwise_triangle(s, outsideEdgePointBaseOffset, outsideEdgePointBaseOffset + 1,
                                       outsideEdgePointBaseOffset + 2, s.numIndices);
        s.numIndices += 3;
    }
}

inline void tess_tri(TESS_THREAD TessState& s, float tf0, float tf1, float tf2, float inside) {
    TessTriFactors p;
    tess_tri_process(s, tf0, tf1, tf2, inside, p);
    if (p.patchCulled != 0) {
        s.numPoints = 0;
        s.numIndices = 0;
        return;
    }
    if (p.justDoMinimumTessFactor != 0) {
        tess_define_point(s, 0, FXP_ONE, 0);
        tess_define_point(s, 0, 0, 1);
        tess_define_point(s, FXP_ONE, 0, 2);
        s.numPoints = 3;
        if (s.outputPrimitive == TESS_OUT_TRIANGLE_CW || s.outputPrimitive == TESS_OUT_TRIANGLE_CCW) {
            tess_define_clockwise_triangle(s, 0, 1, 2, s.numIndices);
            s.numIndices = 3;
        } else if (s.outputPrimitive == TESS_OUT_POINT) {
            tess_dump_all_points(s);
        } else {
            tess_dump_all_points_as_in_order_line_list(s);
        }
        return;
    }
    tess_tri_generate_points(s, p);
    if (s.outputPrimitive == TESS_OUT_POINT) { tess_dump_all_points(s); return; }
    if (s.outputPrimitive == TESS_OUT_LINE) { tess_dump_all_points_as_in_order_line_list(s); return; }
    tess_tri_generate_connectivity(s, p);
}

// ============================== isoline
inline void tess_isoline(TESS_THREAD TessState& s, float density, float detail) {
    if (!(density > 0) || !(detail > 0)) {
        s.numPoints = 0;
        s.numIndices = 0;
        return;
    }
    float lowerBound, upperBound;
    tess_edge_bounds(s, lowerBound, upperBound);
    density = tess_fmin(64.0f, tess_fmax(1.0f, density));
    detail = tess_fmin(upperBound, tess_fmax(lowerBound, detail));
    s.numPoints = 0;
    s.numIndices = 0;
    int lineDetailParity;
    if (tess_hw_integer(s)) {
        detail = ceil(detail);
        lineDetailParity = tess_is_even(detail) ? TESS_PARITY_EVEN : TESS_PARITY_ODD;
    } else {
        lineDetailParity = s.originalParity;
    }
    const uint fxpDetail = tess_float_to_fixed(detail);
    s.parity = lineDetailParity;
    TessFactorContext detailCtx;
    tess_compute_tess_factor_context(s, fxpDetail, detailCtx);
    const int numPointsPerLine = tess_num_points_for_tess_factor(s, fxpDetail);
    s.partitioning = TESS_INTEGER;  // OverridePartitioning: density is integer
    density = ceil(density);
    const int lineDensityParity = tess_is_even(density) ? TESS_PARITY_EVEN : TESS_PARITY_ODD;
    s.parity = lineDensityParity;
    const uint fxpDensity = tess_float_to_fixed(density);
    TessFactorContext densityCtx;
    tess_compute_tess_factor_context(s, fxpDensity, densityCtx);
    const int numLines = tess_num_points_for_tess_factor(s, fxpDensity) - 1;  // no line at V == 1
    s.partitioning = s.originalPartitioning;  // RestorePartitioning
    s.numPoints = numPointsPerLine * numLines;
    if (s.outputPrimitive == TESS_OUT_POINT) {
        s.numIndices = s.numPoints;
    } else {
        s.numIndices = numLines * (numPointsPerLine - 1) * 2;
    }
    // IsoLineGeneratePoints
    int pointOffset = 0;
    for (int line = 0; line < numLines; line++) {
        for (int point = 0; point < numPointsPerLine; point++) {
            s.parity = lineDensityParity;
            const uint fxpV = tess_place_point_in_1d(s, densityCtx, line);
            s.parity = lineDetailParity;
            const uint fxpU = tess_place_point_in_1d(s, detailCtx, point);
            tess_define_point(s, fxpU, fxpV, pointOffset++);
        }
    }
    // IsoLineGenerateConnectivity
    pointOffset = 0;
    int indexOffset = 0;
    for (int line = 0; line < numLines; line++) {
        for (int point = 0; point < numPointsPerLine; point++) {
            if (s.outputPrimitive == TESS_OUT_POINT) {
                tess_define_index(s, pointOffset++, indexOffset++);
            } else {
                if (point > 0) {
                    tess_define_index(s, pointOffset - 1, indexOffset++);
                    tess_define_index(s, pointOffset, indexOffset++);
                }
                pointOffset++;
            }
        }
    }
}

/// One patch: @p domain 1 isoline / 2 tri / 3 quad and @p partitioning,
/// @p output_primitive as DXBC declares them (1-based); @p factors in the
/// reference's argument order.
inline void tess_patch(TESS_THREAD TessState& s, uint domain, uint partitioning,
                       uint output_primitive, TESS_THREAD const float* factors) {
    s.originalPartitioning = (int)partitioning - 1;
    s.partitioning = s.originalPartitioning;
    s.parity = s.originalPartitioning == TESS_FRACTIONAL_ODD ? TESS_PARITY_ODD : TESS_PARITY_EVEN;
    s.originalParity = s.parity;
    s.outputPrimitive = (int)output_primitive - 1;
    s.numPoints = 0;
    s.numIndices = 0;
    s.usingPatchedIndices = 0;
    s.usingPatchedIndices2 = 0;
    s.sink.vertices_per_primitive = output_primitive == 1 ? 1u : output_primitive == 2 ? 2u : 3u;
    if (domain == 1) {
        tess_isoline(s, factors[0], factors[1]);
    } else if (domain == 2) {
        tess_tri(s, factors[0], factors[1], factors[2], factors[3]);
    } else {
        tess_quad(s, factors[0], factors[1], factors[2], factors[3], factors[4], factors[5]);
    }
}

)  // VITREO_TESS_BODY

#undef TESS_THREAD
#undef TESS_DEVICE
#undef constant_int
#undef constant_uint
#undef constant_float
#undef TESS_AS_INT
#undef TESS_AS_FLOAT

// ---------------------------------------------------------------------------
// The GPU passes. Per batch of patches:
//   vitreo_tess_patch (mode 0)  one thread per patch: its point and index
//                               counts, from the hull kernel's raw factors;
//   vitreo_tess_scan            one threadgroup: each patch's first point and
//                               index (exclusive prefix sums, in place), and
//                               the block the later passes read;
//   vitreo_tess_patch (mode 1)  the points and indices, at those offsets.
// Patches keep their order, so primitives are in patch order (D3D11.3
// 11.7.9: "all geometry for a given patch flows down the pipeline before any
// geometry for subsequent patches").
// ---------------------------------------------------------------------------

/// Constants of vitreo_tess_patch.
struct TessPatchParams {
    u32 mode{0};                 ///< 0 count, 1 write
    u32 domain{0};               ///< DXBC: 1 isoline, 2 tri, 3 quad
    u32 partitioning{0};         ///< 1 integer, 2 pow2, 3 odd, 4 even
    u32 output_primitive{0};     ///< 1 point, 2 line, 3 cw, 4 ccw
    u32 patches{0};              ///< in this batch
    u32 count_mode{0};           ///< 1: patches past the GPU word are empty
    u32 first_patch{0};          ///< the batch's first, in the hull output
    u32 patches_per_instance{1}; ///< SV_PrimitiveID = patch % this
    u32 write_primitive_ids{0};
    u32 factor_words{8};
    u32 point_capacity{0};
    u32 index_capacity{0};
};
static_assert(sizeof(TessPatchParams) == 48);
inline constexpr unsigned TESS_FACTORS_INDEX = 1;
inline constexpr unsigned TESS_COUNTS_INDEX = 2;   ///< 2 words per patch
inline constexpr unsigned TESS_POINTS_INDEX = 3;
inline constexpr unsigned TESS_INDICES_INDEX = 4;
inline constexpr unsigned TESS_PRIMITIVE_IDS_INDEX = 5;
inline constexpr unsigned TESS_PATCH_COUNT_INDEX = 6;
inline constexpr const char* kTessPatchEntryPoint = "vitreo_tess_patch";

/// Constants of vitreo_tess_scan.
struct TessScanParams {
    u32 patches{0};
    u32 vertices_per_primitive{1};
    u32 point_capacity{0};
    u32 index_capacity{0};
};
inline constexpr unsigned TESS_SCAN_COUNTS_INDEX = 1;
inline constexpr unsigned TESS_SCAN_BLOCK_INDEX = 2;
inline constexpr unsigned TESS_SCAN_ARGS_INDEX = 3;
inline constexpr unsigned TESS_SCAN_THREADS = 256;
inline constexpr const char* kTessScanEntryPoint = "vitreo_tess_scan";
/// The batch block (u32 words). [5] and [6] are the domain kernel's
/// control-point / patch-constant strides (host-written), [7] its point
/// count -- the compute-lowered domain shader reads the block as its
/// control (kDomainControl*).
inline constexpr u32 TESS_BLOCK_POINTS = 7;
inline constexpr u32 TESS_BLOCK_INDICES = 8;
inline constexpr u32 TESS_BLOCK_PRIMITIVES = 9;
inline constexpr u32 TESS_BLOCK_OVERFLOW = 10;
inline constexpr u32 TESS_BLOCK_WORDS = 16;
/// The batch's argument blocks, in a buffer of their own: a Metal indirect
/// dispatch must not read its arguments from a buffer the same dispatch
/// binds (measured: the domain kernel never ran), so they are kept apart
/// from the block the domain kernel reads.
/// {groups, 1, 1} for the domain kernel (64 threads a group).
inline constexpr u32 TESS_ARGS_DOMAIN_DISPATCH = 0;
/// {indices, 1, 0, 0, 0}: the indexed draw of the domain records.
inline constexpr u32 TESS_ARGS_DRAW = 4;
inline constexpr u32 TESS_ARGS_WORDS = 12;

/// Points and indices one patch makes at most, for these declarations and a
/// maxtessfactor of @p max_factor (the hull shader clamps every factor to it,
/// D3D11.3 10.13): the reference run with every factor at the limit.
struct TessPatchBound {
    u32 points{0};
    u32 indices{0};
};
[[nodiscard]] inline TessPatchBound tess_patch_bound(u32 domain,
                                                     u32 partitioning,
                                                     u32 output_primitive,
                                                     float max_factor) {
    const float m = max_factor < 1.0f ? 1.0f : max_factor > 64.0f ? 64.0f : max_factor;
    // Each count grows with every factor (more segments on an edge, more
    // rings inside), so all at the limit is the most a patch can make.
    float factors[8] = {m, m, m, m, m, m, 0, 0};
    TessState s{};
    s.sink.write = 0;
    tess_patch(s, domain, partitioning, output_primitive, factors);
    TessPatchBound bound{};
    bound.points = static_cast<u32>(s.numPoints);
    bound.indices = static_cast<u32>(s.numIndices);
    return bound;
}

[[nodiscard]] inline std::string tessellator_msl() {
    std::string msl = R"(#include <metal_stdlib>
using namespace metal;
#define TESS_THREAD thread
#define TESS_DEVICE device
#define TESS_AS_INT(f) as_type<int>(f)
#define TESS_AS_FLOAT(i) as_type<float>(i)
#define constant_int constant int
#define constant_uint constant uint
#define constant_float constant float
)";
    msl += body_source();
    char kernels[6144];
    std::snprintf(kernels, sizeof(kernels), R"(

struct TessPatchParams {
    uint mode;
    uint domain;
    uint partitioning;
    uint output_primitive;
    uint patches;
    uint count_mode;
    uint first_patch;
    uint patches_per_instance;
    uint write_primitive_ids;
    uint factor_words;
    uint point_capacity;
    uint index_capacity;
};

kernel void %s(
    constant TessPatchParams& p [[buffer(0)]],
    device const float* factors [[buffer(%u)]],
    device uint* counts [[buffer(%u)]],
    device uint* points [[buffer(%u)]],
    device uint* indices [[buffer(%u)]],
    device uint* primitive_ids [[buffer(%u)]],
    device const uint* patch_count [[buffer(%u)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid >= p.patches) return;
    uint n = p.patches;
    uint per_instance = max(p.patches_per_instance, 1u);
    if (p.count_mode != 0u) {
        // An indirect draw's resolved block: {patches in all, per instance}.
        const uint total = patch_count[0];
        n = min(n, total > p.first_patch ? total - p.first_patch : 0u);
        per_instance = max(patch_count[1], 1u);
    }
    if (tid >= n) {
        if (p.mode == 0u) { counts[2u * tid] = 0u; counts[2u * tid + 1u] = 0u; }
        return;
    }
    const uint patch_index = p.first_patch + tid;
    float f[8];
    for (uint i = 0u; i < 8u; ++i) {
        f[i] = i < p.factor_words ? factors[patch_index * p.factor_words + i]
                                  : 0.0f;
    }
    TessState s;
    s.sink.points = points;
    s.sink.indices = indices;
    s.sink.primitive_ids = primitive_ids;
    s.sink.point_base = 0u;
    s.sink.index_base = 0u;
    s.sink.patch_index = patch_index;
    s.sink.primitive_id = patch_index %% per_instance;
    s.sink.write = 0u;
    s.sink.write_primitive_ids = p.write_primitive_ids;
    if (p.mode == 1u) {
        s.sink.point_base = counts[2u * tid];
        s.sink.index_base = counts[2u * tid + 1u];
        // A batch the scan found over capacity is dropped (it never is for
        // the sizes the host computes; see tess_patch_bound).
        s.sink.write = counts[2u * tid] != 0xFFFFFFFFu ? 1u : 0u;
    }
    tess_patch(s, p.domain, p.partitioning, p.output_primitive, f);
    if (p.mode == 0u) {
        counts[2u * tid] = uint(s.numPoints);
        counts[2u * tid + 1u] = uint(s.numIndices);
    }
}

struct TessScanParams {
    uint patches;
    uint vertices_per_primitive;
    uint point_capacity;
    uint index_capacity;
};

kernel void %s(
    constant TessScanParams& p [[buffer(0)]],
    device uint* counts [[buffer(%u)]],
    device uint* block [[buffer(%u)]],
    device uint* args [[buffer(%u)]],
    uint t [[thread_index_in_threadgroup]])
{
    threadgroup uint sums[%u][2];
    threadgroup uint overflow;
    const uint chunk = (p.patches + %uu - 1u) / %uu;
    const uint begin = min(t * chunk, p.patches);
    const uint end = min(begin + chunk, p.patches);
    uint points = 0u;
    uint indices = 0u;
    for (uint i = begin; i < end; ++i) {
        points += counts[2u * i];
        indices += counts[2u * i + 1u];
    }
    sums[t][0] = points;
    sums[t][1] = indices;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t == 0u) {
        uint running_points = 0u;
        uint running_indices = 0u;
        for (uint i = 0u; i < %uu; ++i) {
            const uint sp = sums[i][0];
            const uint si = sums[i][1];
            sums[i][0] = running_points;
            sums[i][1] = running_indices;
            running_points += sp;
            running_indices += si;
        }
        const bool over = running_points > p.point_capacity ||
                          running_indices > p.index_capacity;
        overflow = over ? 1u : 0u;
        const uint vpp = max(p.vertices_per_primitive, 1u);
        block[%u] = over ? 0u : running_points;
        block[%u] = over ? 0u : running_indices;
        block[%u] = over ? 0u : running_indices / vpp;
        block[%u] = over ? 1u : 0u;
        args[%u] = over ? 0u : (running_points + 63u) / 64u;
        args[%u + 1u] = 1u;
        args[%u + 2u] = 1u;
        args[%u] = over ? 0u : running_indices;
        args[%u + 1u] = 1u;
        args[%u + 2u] = 0u;
        args[%u + 3u] = 0u;
        args[%u + 4u] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint at_points = sums[t][0];
    uint at_indices = sums[t][1];
    for (uint i = begin; i < end; ++i) {
        const uint np = counts[2u * i];
        const uint ni = counts[2u * i + 1u];
        counts[2u * i] = overflow != 0u ? 0xFFFFFFFFu : at_points;
        counts[2u * i + 1u] = at_indices;
        at_points += np;
        at_indices += ni;
    }
}
)",
                  kTessPatchEntryPoint, TESS_FACTORS_INDEX, TESS_COUNTS_INDEX,
                  TESS_POINTS_INDEX, TESS_INDICES_INDEX,
                  TESS_PRIMITIVE_IDS_INDEX, TESS_PATCH_COUNT_INDEX,
                  kTessScanEntryPoint, TESS_SCAN_COUNTS_INDEX,
                  TESS_SCAN_BLOCK_INDEX, TESS_SCAN_ARGS_INDEX,
                  TESS_SCAN_THREADS, TESS_SCAN_THREADS,
                  TESS_SCAN_THREADS, TESS_SCAN_THREADS, TESS_BLOCK_POINTS,
                  TESS_BLOCK_INDICES, TESS_BLOCK_PRIMITIVES,
                  TESS_BLOCK_OVERFLOW, TESS_ARGS_DOMAIN_DISPATCH,
                  TESS_ARGS_DOMAIN_DISPATCH, TESS_ARGS_DOMAIN_DISPATCH,
                  TESS_ARGS_DRAW, TESS_ARGS_DRAW, TESS_ARGS_DRAW,
                  TESS_ARGS_DRAW, TESS_ARGS_DRAW);
    msl += kernels;
    return msl;
}

}  // namespace vitreo::graphics::tess
