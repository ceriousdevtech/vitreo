// SPDX-License-Identifier: LGPL-2.1-only
//
// graphics::fence_minmax_reduction (msl_minmax_fence.cpp): the MIN/MAX
// reduction switch in translated MSL, on SPIRV-Cross's output shape.

#include <gtest/gtest.h>

#include "graphics/shaders/minmax_reduction.hpp"

#include <string>

using namespace vitreo::graphics;

namespace {

const char* const kShape =
    "constant bool vitreoMinMaxReduction_tmp [[function_constant(4242)]];\n"
    "constant bool vitreoMinMaxReduction = is_function_constant_defined("
    "vitreoMinMaxReduction_tmp) ? vitreoMinMaxReduction_tmp : false;\n"
    "\n"
    "static __attribute__((noinline))\n"
    "float4 vitreoMinMaxSample0(texture2d<float> t, sampler s, float2 c)\n"
    "{\n"
    "    return t.sample(s, c, level(0.0));\n"
    "}\n"
    "\n"
    "fragment float4 main0(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],"
    " constant uint& f [[buffer(0)]])\n"
    "{\n"
    "    float4 r;\n"
    "    if (vitreoMinMaxReduction && ((f & 8u) != 0u))\n"
    "    {\n"
    "        if (f > 9u)\n"
    "        {\n"
    "            r = vitreoMinMaxSample0(t, s, float2(0.5));\n"
    "        }\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        r = t.sample(s, float2(0.5));\n"
    "    }\n"
    "    return r;\n"
    "}\n";

}  // namespace

TEST(MslMinMaxFence, TheConstantBecomesTheSwitchAndTheBranchIsFenced) {
    std::string msl = kShape;
    fence_minmax_reduction(msl);
    EXPECT_EQ(msl.find("function_constant"), std::string::npos) << msl;
    EXPECT_NE(msl.find("#ifndef VITREO_MINMAX_REDUCTION\n#define VITREO_MINMAX_REDUCTION 0\n"
                       "#endif\n#define vitreoMinMaxReduction (VITREO_MINMAX_REDUCTION != 0)\n"),
              std::string::npos)
        << msl;
    // The gated branch through its `else`, and the helper only it calls.
    EXPECT_NE(msl.find("#if VITREO_MINMAX_REDUCTION\n    if (vitreoMinMaxReduction && "),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find("    }\n    else\n#endif\n    {\n        r = t.sample"),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find("#if VITREO_MINMAX_REDUCTION\nstatic __attribute__((noinline))\n"
                       "float4 vitreoMinMaxSample0("),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find("    return t.sample(s, c, level(0.0));\n}\n#endif\n"),
              std::string::npos)
        << msl;
    EXPECT_TRUE(msl_has_minmax_switch(msl));
}

TEST(MslMinMaxFence, AnotherShapeKeepsTheConstantFalseConditionAndTheHelper) {
    // A gate SPIRV-Cross wrote as a temporary: not fenced, so the helper it
    // may call stays; the condition is still the switch.
    std::string msl = kShape;
    const std::string gate = "    if (vitreoMinMaxReduction && ((f & 8u) != 0u))\n";
    msl.replace(msl.find(gate), gate.size(),
                "    bool g = vitreoMinMaxReduction && ((f & 8u) != 0u);\n    if (g)\n");
    fence_minmax_reduction(msl);
    EXPECT_EQ(msl.find("#if VITREO_MINMAX_REDUCTION"), std::string::npos) << msl;
    EXPECT_NE(msl.find("#define vitreoMinMaxReduction (VITREO_MINMAX_REDUCTION != 0)"),
              std::string::npos);
    EXPECT_NE(msl.find("float4 vitreoMinMaxSample0("), std::string::npos);
}

TEST(MslMinMaxFence, MslWithoutTheConstantIsUnchanged) {
    const std::string plain = "fragment float4 main0() { return float4(0.0); }\n";
    std::string msl = plain;
    fence_minmax_reduction(msl);
    EXPECT_EQ(msl, plain);
    EXPECT_FALSE(msl_has_minmax_switch(msl));
}
