// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv_spv_defs.hpp
/// @brief SPIR-V constants and enums the DXBC translator emits.

#include <vitreo/common.hpp>

namespace vitreo::graphics::dxbc_spirv_detail {

// =============================================================================
// SPIR-V Constants
// =============================================================================

constexpr u32 SPIRV_MAGIC = 0x07230203;
constexpr u32 SPIRV_VERSION = 0x00010600; // 1.6
constexpr u32 SPIRV_GENERATOR = 0x00000000; // Vitreo
constexpr u32 SPIRV_BOUND_BASE = 1000; // ID allocation starts here

// SPIR-V Opcodes (subset needed for basic shaders)
enum class SpvOp : u16 {
    OpNop = 0,
    OpSource = 3,
    OpName = 5,
    OpMemberName = 6,
    OpString = 7,
    OpExtension = 10,
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpFunctionCall = 57,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpDecorateString = 5632,
    OpMemberDecorate = 72,
    OpVectorShuffle = 79,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpCompositeInsert = 82,
    OpSampledImage = 86,
    OpImageSampleImplicitLod = 87,
    OpImageSampleExplicitLod = 88,
    OpImageSampleDrefImplicitLod = 89,
    OpImageSampleDrefExplicitLod = 90,
    OpImageFetch = 95,
    OpImageGather = 96,
    OpImageDrefGather = 97,
    OpImageRead = 98,
    OpImageWrite = 99,
    OpImageQuerySize = 104,
    OpConvertFToU = 109,
    OpConvertFToS = 110,
    OpConvertSToF = 111,
    OpConvertUToF = 112,
    OpBitcast = 124,
    OpSNegate = 126,
    OpFNegate = 127,
    OpIAdd = 128,
    OpFAdd = 129,
    OpISub = 130,
    OpFSub = 131,
    OpIMul = 132,
    OpFMul = 133,
    OpUDiv = 134,
    OpFDiv = 136,
    OpUMod = 137,
    OpFMod = 141,
    OpVectorTimesScalar = 142,
    OpMatrixTimesScalar = 143,
    OpMatrixTimesVector = 145,
    OpMatrixTimesMatrix = 146,
    OpDot = 148,
    OpUMulExtended = 151,
    OpSMulExtended = 152,
    OpIsNan = 156,
    OpAll = 155,
    OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195,
    OpShiftLeftLogical = 196,
    OpBitwiseOr = 197,
    OpBitwiseXor = 198,
    OpBitwiseAnd = 199,
    OpNot = 200,
    OpBitFieldSExtract = 202,
    OpBitFieldUExtract = 203,
    OpBitReverse = 204,
    OpBitCount = 205,
    OpDPdx = 207,
    OpDPdy = 208,
    OpDPdxFine = 210,
    OpDPdyFine = 211,
    OpDPdxCoarse = 213,
    OpDPdyCoarse = 214,
    OpImageQuerySizeLod = 103,
    OpImageQueryLod = 105,
    OpImageQueryLevels = 106,
    OpImageQuerySamples = 107,
    OpAtomicIAdd = 234,
    // OpAtomicISub = 235 (imm_atomic_consume) was once missing from this
    // table, which made every value below it low by one -- see
    // tools/audit_spirv_constants.py, which now checks this table against the
    // vendored SPIR-V header on every test run.
    OpAtomicISub = 235,
    OpAtomicSMin = 236,
    OpAtomicUMin = 237,
    OpAtomicSMax = 238,
    OpAtomicUMax = 239,
    OpAtomicAnd = 240,
    OpAtomicOr = 241,
    OpAtomicXor = 242,
    OpAtomicExchange = 229,
    OpImageTexelPointer = 60,
    OpAtomicCompareExchange = 230,
    OpLogicalOr = 166,
    OpLogicalAnd = 167,
    OpLogicalNot = 168,
    OpSelect = 169,
    OpIEqual = 170,
    OpINotEqual = 171,
    OpUGreaterThan = 172,
    OpUGreaterThanEqual = 174,
    OpSGreaterThanEqual = 175,
    OpULessThan = 176,
    OpSLessThan = 177,
    OpULessThanEqual = 178,
    OpFOrdEqual = 180,
    OpFUnordNotEqual = 183,
    OpFOrdLessThan = 184,
    OpFOrdGreaterThanEqual = 190,
    OpPhi = 245,
    OpLoopMerge = 246,
    OpSelectionMerge = 247,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpSwitch = 251,
    OpKill = 252,
    OpDemoteToHelperInvocationEXT = 5380,
    OpReturn = 253,
    OpReturnValue = 254,
    OpEmitVertex = 218,
    OpEndPrimitive = 219,
    OpGroupNonUniformBallot = 339,
    OpGroupNonUniformQuadBroadcast = 365,
    OpControlBarrier = 224,
    OpMemoryBarrier = 225,
};

// GLSL.std.450 extended instructions used by this translator.
constexpr u32 kGlslStd450Floor = 8;
constexpr u32 kGlslStd450FAbs = 4;
constexpr u32 kGlslStd450RoundEven = 2;
constexpr u32 kGlslStd450Trunc = 3;
constexpr u32 kGlslStd450Ceil = 9;
constexpr u32 kGlslStd450Sin = 13;
constexpr u32 kGlslStd450Cos = 14;
constexpr u32 kGlslStd450Exp2 = 29;
constexpr u32 kGlslStd450Log2 = 30;
constexpr u32 kGlslStd450Sqrt = 31;
constexpr u32 kGlslStd450InverseSqrt = 32;
constexpr u32 kGlslStd450PackHalf2x16 = 58;
constexpr u32 kGlslStd450UnpackHalf2x16 = 62;
constexpr u32 kGlslStd450FindILsb = 73;
constexpr u32 kGlslStd450FindSMsb = 74;
constexpr u32 kGlslStd450FindUMsb = 75;
// GLSL.std.450.h (vendored with SPIRV-Cross): FMin 37, FMax 40, Fma 50,
// InterpolateAtCentroid 76 / AtSample 77 / AtOffset 78.
constexpr u32 kGlslStd450FMin = 37;
constexpr u32 kGlslStd450FMax = 40;
// NaN-aware minNum / maxNum (GLSL.std.450.h NMin 79, NMax 80; MSL
// precise::min / precise::max).
constexpr u32 kGlslStd450NMin = 79;
constexpr u32 kGlslStd450NMax = 80;
constexpr u32 kGlslStd450Fma = 50;
constexpr u32 kGlslStd450InterpolateAtCentroid = 76;
constexpr u32 kGlslStd450InterpolateAtSample = 77;
constexpr u32 kGlslStd450InterpolateAtOffset = 78;

enum class SpvCapability : u32 {
    Shader = 1,
    Geometry = 2,
    Tessellation = 3,
    // gather4_po: an Offset image operand that is not a constant.
    ImageGatherExtended = 25,
    ClipDistance = 32,
    SampleRateShading = 35,
    MultiViewport = 57,
    Sampled1D = 43,
    SampledBuffer = 46,
    ImageBuffer = 47,
    ImageMSArray = 48,
    ImageQuery = 50,
    DerivativeControl = 51,
    InterpolationFunction = 52,
    GroupNonUniform = 61,
    // PSInvocations counting: a subgroup ballot of the counted lanes.
    GroupNonUniformBallot = 64,
    GroupNonUniformQuad = 68,
    StorageImageReadWithoutFormat = 55,
    StorageImageWriteWithoutFormat = 56,
    // Framebuffer-fetch reads of render targets (the logic-op variant).
    InputAttachment = 40,
    ShaderLayer = 69,
    ShaderViewportIndex = 70,
    // SPV_KHR_shader_draw_parameters: BaseVertex / BaseInstance / DrawIndex.
    DrawParameters = 4427,
    StencilExportEXT = 5013,
    DemoteToHelperInvocationEXT = 5379,
    // Shader model 5.1 descriptor arrays (SPIR-V 1.5 core, from
    // SPV_EXT_descriptor_indexing): indexing an array of descriptors with a
    // dynamically uniform value, an unbounded (runtime) descriptor array,
    // and NonUniformResourceIndex.
    UniformBufferArrayDynamicIndexing = 28,
    SampledImageArrayDynamicIndexing = 29,
    StorageBufferArrayDynamicIndexing = 30,
    StorageImageArrayDynamicIndexing = 31,
    ShaderNonUniform = 5301,
    RuntimeDescriptorArray = 5302,
    UniformTexelBufferArrayDynamicIndexing = 5304,
    StorageTexelBufferArrayDynamicIndexing = 5305,
    UniformBufferArrayNonUniformIndexing = 5306,
    SampledImageArrayNonUniformIndexing = 5307,
    StorageBufferArrayNonUniformIndexing = 5308,
    StorageImageArrayNonUniformIndexing = 5309,
    UniformTexelBufferArrayNonUniformIndexing = 5311,
    StorageTexelBufferArrayNonUniformIndexing = 5312,
};

enum class SpvExecutionModel : u32 {
    Vertex = 0,
    TessellationEvaluation = 2,
    Geometry = 3,
    Fragment = 4,
    GLCompute = 5,
};

enum class SpvExecutionMode : u32 {
    SpacingEqual = 1,
    SpacingFractionalEven = 2,
    SpacingFractionalOdd = 3,
    VertexOrderCw = 4,
    VertexOrderCcw = 5,
    OriginUpperLeft = 7,
    /// SPIR-V 3.6 execution mode 9: fragment tests run before the shader.
    EarlyFragmentTests = 9,
    DepthReplacing = 12,
    DepthGreater = 14,
    DepthLess = 15,
    LocalSize = 17,
    InputPoints = 19,
    InputLines = 20,
    InputLinesAdjacency = 21,
    Triangles = 22,
    InputTrianglesAdjacency = 23,
    Quads = 24,
    OutputVertices = 26,
    OutputPoints = 27,
    OutputLineStrip = 28,
    OutputTriangleStrip = 29,
};

enum class SpvStorageClass : u32 {
    Input = 1,
    Output = 3,
    Uniform = 2,
    UniformConstant = 0,
    Function = 7,
    Workgroup = 4,
    // SPIR-V 1.6 Table 2 (Storage Class). Module-scope variables private to
    // the invocation: where a geometry shader lowered to compute stages the
    // o# and v[][] registers it would otherwise hold in the interface.
    Private = 6,
    // SPIR-V 1.6 Table 2: the class of a pointer produced by
    // OpImageTexelPointer, which is how an atomic reaches a texel.
    Image = 11,
    PushConstant = 9,
    StorageBuffer = 12,
};

enum class SpvDecoration : u32 {
    NoContraction = 42,
    Invariant = 18,
    // SPIR-V 1.6 Table 3 (Decoration): interpolation qualifiers.
    NoPerspective = 13,
    Centroid = 16,
    Sample = 17,
    Location = 30,
    Binding = 33,
    DescriptorSet = 34,
    BuiltIn = 11,
    Flat = 14,
    Block = 2,
    ArrayStride = 6,
    Offset = 35,
    NonWritable = 24,
    /// SPIR-V 3.20 decoration 23: accesses are coherent with other
    /// invocations' accesses (SPIRV-Cross spells it `volatile` in MSL).
    Coherent = 23,
    /// A SubpassData image's attachment: MSL [[color(n)]].
    InputAttachmentIndex = 43,
    /// OpDecorateString's user semantic (core since SPIR-V 1.4).
    UserSemantic = 5635,
    /// NonUniformResourceIndex: the index and the descriptor it selects.
    NonUniform = 5300,
};

enum class SpvBuiltIn : u32 {
    Position = 0,
    ClipDistance = 3,
    PrimitiveId = 7,
    Layer = 9,
    ViewportIndex = 10,
    TessCoord = 13,
    FragCoord = 15,
    FrontFacing = 17,
    SampleId = 18,
    SampleMask = 20,
    FragDepth = 22,
    HelperInvocation = 23,
    WorkgroupId = 26,
    LocalInvocationId = 27,
    GlobalInvocationId = 28,
    LocalInvocationIndex = 29,
    SubgroupLocalInvocationId = 41,
    VertexIndex = 42,
    InstanceIndex = 43,
    BaseVertex = 4424,
    BaseInstance = 4425,
    FragStencilRefEXT = 5014,
};

enum class SpvDim : u32 {
    Dim1D = 0,
    Dim2D = 1,
    Dim3D = 2,
    DimCube = 3,
    /// A D3D typed buffer (`Buffer<T>`). Sampled, unmipped, unfiltered, and
    /// addressed by a single element index.
    DimBuffer = 5,
    /// A render target's current value at the fragment (framebuffer fetch).
    DimSubpassData = 6,
};

} // namespace vitreo::graphics::dxbc_spirv_detail
