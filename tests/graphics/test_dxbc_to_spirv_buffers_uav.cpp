// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_buffers_uav.cpp
/// @brief Raw, typed and structured UAVs, groupshared memory, atomics.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/contracts/backend_device.hpp"

#include "dxbc_to_spirv_test_shaders.h"
#include "dxbc_to_spirv_test_spirv.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;
using namespace vitreo::graphics::dxbc_spirv_test;

TEST(DxbcToSpirvTest, TranslatesRawBufferLoadAsAStorageBuffer) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 1});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010050u);                    // vs_5_0
    append_word(11u);                            // total length in DWORDs
    append_word(161u | (3u << 24));              // dcl_resource_raw t0
    append_word(0x00107000u); append_word(0u);
    append_word(165u | (7u << 24));              // ld_raw
    append_word(0x001020F2u); append_word(0u);   // dst o0.xyzw
    append_word(0x001010E4u); append_word(0u);   // byte offset from v0
    append_word(0x001070E4u); append_word(0u);   // t0.xyzw
    append_word(62u | (1u << 24));               // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // Same binding window as a StructuredBuffer SRV: a read-only Metal device
    // buffer at 2 + the four constant-buffer slots.
    EXPECT_NE(msl.find("buffer(6)"), std::string::npos) << msl;
    EXPECT_NE(msl.find("const device"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("texture"), std::string::npos) << msl;
}

// Microsoft Learn (RWStructuredBuffer): like its read-only counterpart, the
// element type is opaque to the shader ISA — store_structured addresses the
// buffer as elementIndex * stride + byteOffset — so a UAV declared with
// dcl_uav_structured is a writable storage buffer, not a storage image. UE5's
// compute shaders use this constantly, and refusing the declaration made
// CreateComputeShader fail, which UE treats as fatal.
// Microsoft Learn (RWBuffer): a typed buffer UAV is dimension 1 (BUFFER). It
// is the writable counterpart of a Buffer<T> SRV — a texture addressed by one
// element index whose format decode lives in the view, not the shader — so it
// is a Dim=Buffer storage image (Metal texture_buffer), never a storage
// buffer. Refusing it made CreateComputeShader fail, which UE treats as fatal.
TEST(DxbcToSpirvTest, TranslatesTypedBufferUavAsAStorageTextureBuffer) {
    // Resource return type 0x3333 is SINT on all four components: an
    // RWBuffer<int>. An integer view is a distinct family from the float one,
    // so the image's sampled type has to be an integer too.
    auto container = create_synthetic_typed_uav_store_compute_shader(
        /*resource_dimension=*/1u, /*packed_return_type=*/0x00003333u);
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // A writable texture buffer of ints, written through one element index.
    EXPECT_NE(msl.find("texture_buffer<int"), std::string::npos) << msl;
    EXPECT_NE(msl.find(".write("), std::string::npos) << msl;
    // Learn: an out-of-bounds UAV store is a no-op, so the guard survives.
    EXPECT_NE(msl.find("get_width()"), std::string::npos) << msl;

    // The float form is the same shape with a float sampled type.
    auto floats = create_synthetic_typed_uav_store_compute_shader(
        1u, 0x00005555u);
    const auto float_result = translate_dxbc_to_spirv(floats);
    ASSERT_TRUE(float_result.valid()) << float_result.failure;
    const auto float_msl = compile_spirv_to_msl(
        float_result.spirv.data(), float_result.spirv.size(),
        ShaderStage::Compute, msl_options);
    ASSERT_TRUE(float_msl.valid());
    EXPECT_NE(std::string(float_msl.msl_text()).find("texture_buffer<float"),
              std::string::npos);

    // A mixed return type is not a real DXGI view and stays diagnostic.
    auto mixed = create_synthetic_typed_uav_store_compute_shader(
        1u, 0x00005543u);
    EXPECT_FALSE(translate_dxbc_to_spirv(mixed).valid());
}

// Windows SDK D3D10_SB_RESOURCE_RETURN_TYPE: UNORM (1) and SNORM (2) views
// are the float family — normalisation is the view format's job, done by the
// texture unit, so the shader reads and writes floats exactly as for FLOAT.
// UE5's GPU skinning writes RWBuffer<snorm float4> tangents; refusing the
// declaration failed CreateComputeShader, which UE treats as fatal.
TEST(DxbcToSpirvTest, TranslatesSnormAndUnormTypedUavsAsTheFloatFamily) {
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    for (const u32 packed : {0x00002222u, 0x00001111u}) {
        auto buffer = create_synthetic_typed_uav_store_compute_shader(1u, packed);
        const auto buffer_result = translate_dxbc_to_spirv(buffer);
        ASSERT_TRUE(buffer_result.valid()) << buffer_result.failure;
        const auto buffer_msl = compile_spirv_to_msl(
            buffer_result.spirv.data(), buffer_result.spirv.size(),
            ShaderStage::Compute, msl_options);
        ASSERT_TRUE(buffer_msl.valid());
        EXPECT_NE(std::string(buffer_msl.msl_text()).find("texture_buffer<float"),
                  std::string::npos);

        auto texture = create_synthetic_typed_uav_store_compute_shader(3u, packed);
        const auto texture_result = translate_dxbc_to_spirv(texture);
        ASSERT_TRUE(texture_result.valid()) << texture_result.failure;
        const auto texture_msl = compile_spirv_to_msl(
            texture_result.spirv.data(), texture_result.spirv.size(),
            ShaderStage::Compute, msl_options);
        ASSERT_TRUE(texture_msl.valid());
        EXPECT_NE(std::string(texture_msl.msl_text()).find("texture2d<float"),
                  std::string::npos);
    }
}

// Microsoft Learn (RWTexture3D): a typed UAV of dimension TEXTURE3D (5) is
// addressed by an (x, y, z) texel position and bounded on all three axes.
// UE5 fills its R8 volumes (distance fields, lookup tables) through one.
TEST(DxbcToSpirvTest, TranslatesTypedVolumeUavAsAStorageTexture3D) {
    auto container = create_synthetic_typed_uav_store_compute_shader(
        /*resource_dimension=*/5u, /*packed_return_type=*/0x00005555u);
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("texture3d<float"), std::string::npos) << msl;
    EXPECT_NE(msl.find(".write("), std::string::npos) << msl;
    // The out-of-bounds guard now covers depth as well.
    EXPECT_NE(msl.find("get_depth()"), std::string::npos) << msl;
}

// Microsoft Learn (RWByteAddressBuffer / dcl_uav_raw): a raw UAV has no
// element type and no format — byte-addressed, read and written as 32-bit
// words — so it is a writable storage buffer, the twin of dcl_resource_raw.
// Microsoft Learn (InterlockedAdd / atomic_iadd, imm_atomic_iadd): the atomic_
// forms are a read-modify-write with no result; the imm_atomic_ forms also
// return the value held *before* the operation. Both address a UAV or
// groupshared memory as 32-bit words. UE5 compute uses these for counters and
// histograms, and an unimplemented opcode fails the whole shader.
TEST(DxbcToSpirvTest, TranslatesAtomicsOnARawUavAndGroupsharedMemory) {
    // The opcode numbers below are MEASURED, not recalled: see
    // tools/oracle_probe/dxbc_opcode_probe.hlsl and its .expected table,
    // produced by fxc plus a token-stream decode. An earlier revision of this
    // test hand-assembled imm_atomic_iadd as 179 -- the same off-by-one the
    // translator had -- so the test agreed with the bug instead of catching
    // it. The signed/unsigned assertions below exist to catch the
    // transposition that accompanied it.
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(53u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u);      // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u);
    append_word(1u); append_word(1u);               // dcl_thread_group 1,1,1
    append_word(0x0300009Du);                       // dcl_uav_raw u0
    append_word(0x0011E000u); append_word(0u);
    // atomic_iadd u0, l(0), vThreadID  (no returned value)
    append_word(173u | (6u << 24));
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00020E46u);
    // imm_atomic_iadd r0.x, u0, l(0), vThreadID  (returns the prior value)
    append_word(180u | (8u << 24));
    append_word(0x00100012u); append_word(0u);
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00020E46u);
    // imm_atomic_imax r0.x, u0, l(0), vThreadID  -- signed
    append_word(186u | (8u << 24));
    append_word(0x00100012u); append_word(0u);
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00020E46u);
    // imm_atomic_umax r0.x, u0, l(0), vThreadID  -- unsigned
    append_word(188u | (8u << 24));
    append_word(0x00100012u); append_word(0u);
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00020E46u);
    // imm_atomic_cmp_exch r0.x, u0, l(0), l(1), vThreadID
    // src0 is the comparand and src1 the value stored on a match.
    append_word(185u | (10u << 24));
    append_word(0x00100012u); append_word(0u);
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00004001u); append_word(1u);
    append_word(0x00020E46u);
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;

    // The signed and unsigned forms must reach DIFFERENT SPIR-V opcodes.
    // Transposing them is silent everywhere else: the MSL still compiles and
    // the result is only wrong for negative operands.
    // These numbers are the SPIR-V header's, verified by
    // tools/audit_spirv_constants.py. They were previously off by one here
    // because OpAtomicISub (235) was missing from the translator's table.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 234u));  // OpAtomicIAdd
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 238u));  // OpAtomicSMax
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 239u));  // OpAtomicUMax
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 230u));  // OpAtomicCompareExchange
    // Nothing here is an atomic subtract or an unsigned MIN; seeing either
    // would mean the table had slipped again.
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, 235u));  // OpAtomicISub
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, 237u));  // OpAtomicUMin

    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // Both forms lower to a real atomic, not a plain read-modify-write.
    EXPECT_NE(msl.find("atomic_fetch_add_explicit"), std::string::npos) << msl;
    EXPECT_NE(msl.find("atomic_compare_exchange_weak_explicit"),
              std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, TranslatesAtomicsOnTypedUavs) {
    // Byte-for-byte the SHEX chunk of a real fxc /Fo of
    //   RWBuffer<uint> buf; RWTexture2D<uint> tex;
    //   InterlockedAdd(buf[tid.x], 1u, prev);
    //   InterlockedMax(tex[tid.xy], prev);
    // which is the shape behind RV There Yet's "ATOMIC: undeclared u0/u3".
    // Using the real container means the resource-declaration tokens, the
    // operand encodings and the opcode numbers are all measured.
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(0x00000027u); append_word(0x0100886Au); append_word(0x0400089Cu);
    append_word(0x0011E000u); append_word(0x00000000u); append_word(0x00004444u); append_word(0x0400189Cu);
    append_word(0x0011E000u); append_word(0x00000001u); append_word(0x00004444u); append_word(0x0200005Fu);
    append_word(0x00020032u); append_word(0x02000068u); append_word(0x00000001u); append_word(0x0400009Bu);
    append_word(0x00000001u); append_word(0x00000001u); append_word(0x00000001u); append_word(0x080000B4u);
    append_word(0x00100012u); append_word(0x00000000u); append_word(0x0011E000u); append_word(0x00000000u);
    append_word(0x0002000Au); append_word(0x00004001u); append_word(0x00000001u); append_word(0x05000036u);
    append_word(0x00100012u); append_word(0x00000000u); append_word(0x0010000Au); append_word(0x00000000u);
    append_word(0x060000B0u); append_word(0x0011E000u); append_word(0x00000001u); append_word(0x00020046u);
    append_word(0x0010000Au); append_word(0x00000000u); append_word(0x0100003Eu);

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // The consumer has to raise its MSL version off this flag, so a shader
    // that needs it must actually set it.
    EXPECT_TRUE(result.uses_typed_uav_atomics);
    // OpImageTexelPointer is what makes a texel addressable by an atomic.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 60u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 234u));  // OpAtomicIAdd
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, 239u));  // OpAtomicUMax
    // The shader's max is UNSIGNED, so the signed opcode next door must not
    // appear. This is what caught the off-by-one SPIR-V table: OpAtomicUMax
    // was emitting 238, and SPIRV-Cross faithfully produced a signed
    // spvTextureCast<texture2d<int>> max for it.
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, 238u));  // OpAtomicSMax

    // SPIRV-Cross lowers an image atomic to a NATIVE Metal texture atomic
    // only from MSL 3.1; below that it emulates it with a side buffer that
    // nothing in the backend binds, so the MSL would compile and the atomic
    // would go to an unbound allocation.
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    msl_options.msl_version = 30100;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // Metal's texture atomics are members, with no _explicit suffix.
    EXPECT_NE(msl.find("atomic_fetch_add("), std::string::npos) << msl;
    EXPECT_NE(msl.find("atomic_fetch_max("), std::string::npos) << msl;
    // The emulation path names a "<texture>_atomic" helper buffer. Seeing it
    // here would mean the native path was not taken.
    EXPECT_EQ(msl.find("_atomic["), std::string::npos) << msl;
    // An UNSIGNED max must not be lowered through a signed texture view.
    // SPIRV-Cross casts the texture when the atomic's signedness disagrees
    // with the image's, which is exactly how the off-by-one SPIR-V opcode
    // table showed itself: OpAtomicUMax was emitting OpAtomicSMax, and the
    // MSL came out as spvTextureCast<texture2d<int>>(...).atomic_fetch_max.
    EXPECT_EQ(msl.find("spvTextureCast"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, RefusesAHiddenCounterOnARawUav) {
    // imm_atomic_alloc (178) and imm_atomic_consume (179) occupy the two slots
    // the imm_atomic_ arithmetic forms used to be misnumbered into. They are
    // translated now (UavCounterTest, on real fxc output) -- but Learn gives
    // the hidden counter to a STRUCTURED buffer UAV with the Count or Append
    // flag, and this shader aims them at a raw one. Refusing it, for that
    // reason, is the check that they are neither decoded as arithmetic nor
    // accepted on a UAV that cannot have a counter.
    for (const u32 opcode : {178u, 179u}) {
        DxbcContainer container{};
        container.shader_model_major = 5;
        container.stage = ShaderStage::Compute;
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(container.bytecode.end(), bytes,
                                      bytes + 4);
        };
        append_word(0x00050050u); append_word(20u);
        append_word(0x0100086Au);
        append_word(0x02000068u); append_word(1u);
        append_word(0x0400009Bu); append_word(1u);
        append_word(1u); append_word(1u);
        append_word(0x0300009Du);                   // dcl_uav_raw u0
        append_word(0x0011E000u); append_word(0u);
        append_word(opcode | (5u << 24));           // imm_atomic_alloc/consume
        append_word(0x00100012u); append_word(0u);
        append_word(0x0011E012u); append_word(0u);
        append_word(0x0100003Eu);                   // ret

        const auto result = translate_dxbc_to_spirv(container);
        EXPECT_FALSE(result.valid())
            << "opcode " << opcode << " translated instead of being refused";
        EXPECT_NE(result.failure.find("not a structured UAV"),
                  std::string::npos)
            << "opcode " << opcode << " failure was: " << result.failure;
    }
}

TEST(DxbcToSpirvTest, TranslatesRawUavStoreAsAWritableStorageBuffer) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(24u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u);      // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u);
    append_word(1u); append_word(1u);               // dcl_thread_group 1,1,1
    append_word(0x0300009Du);                       // dcl_uav_raw u0
    append_word(0x0011E000u); append_word(0u);
    append_word(0x070000A5u);                       // ld_raw r0.x, l(0), u0.x
    append_word(0x00100012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x0011E00Au); append_word(0u);
    append_word(0x070000A6u);                       // store_raw u0.x, l(0), r0
    append_word(0x0011E012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00100006u); append_word(0u);
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("u0_raw"), std::string::npos) << msl;
    // Writable, so not a read-only binding and not a texture.
    EXPECT_EQ(msl.find("const device"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("texture"), std::string::npos) << msl;
    // Metal has one buffer namespace. Keep u# above b# [2,5] and
    // structured-buffer t# [6,13], preserving D3D's independent registers.
    EXPECT_NE(msl.find("buffer(14)"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, TranslatesStructuredUavStoreAsAWritableStorageBuffer) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(30u);
    append_word(0x0100086Au);                       // dcl_globalFlags
    append_word(0x02000068u); append_word(1u);      // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u);
    append_word(1u); append_word(1u);               // dcl_thread_group 1,1,1
    append_word(0x0400009Eu);                       // dcl_uav_structured u0
    append_word(0x0011E000u); append_word(0u);
    append_word(16u);                               // structure stride 16
    append_word(0x080000A7u);                       // ld_structured
    append_word(0x001000F2u); append_word(0u);      // dst r0.xyzw
    append_word(0x00020E46u);                       // element = vThreadID
    append_word(0x00004001u); append_word(0u);      // byte offset l(0)
    append_word(0x0011EE46u); append_word(0u);      // u0.xyzw
    append_word(0x080000A8u);                       // store_structured
    append_word(0x0011E0F2u); append_word(0u);      // dst u0.xyzw
    append_word(0x00020E46u);                       // element = vThreadID
    append_word(0x00004001u); append_word(0u);      // byte offset l(0)
    append_word(0x00100E46u); append_word(0u);      // src r0
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // A writable device buffer, not a texture and not a read-only binding.
    EXPECT_NE(msl.find("u0_structured"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("const device"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("texture"), std::string::npos) << msl;
    EXPECT_NE(msl.find("buffer(14)"), std::string::npos) << msl;
    // Address arithmetic is elementIndex * stride + offset, read as words:
    // a stride of 16 and a >> 2 word index.
    EXPECT_NE(msl.find("* 16u"), std::string::npos) << msl;
    EXPECT_NE(msl.find(">> 2u"), std::string::npos) << msl;
}

// Microsoft Learn (dcl_tgsm_structured): groupshared memory has no element
// type either, so a structured declaration describes the same word array a
// raw one does — only the address arithmetic at the use site differs.
TEST(DxbcToSpirvTest, TranslatesStructuredGroupsharedMemoryAsAWordArray) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(43u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u);      // dcl_temps 1
    append_word(0x0400009Bu); append_word(4u);
    append_word(1u); append_word(1u);               // dcl_thread_group 4,1,1
    append_word(0x050000A0u);                       // dcl_tgsm_structured g0
    append_word(0x0011F000u); append_word(0u);
    append_word(4u);                                // structure stride 4
    append_word(8u);                                // structure count 8
    append_word(0x0400009Eu);                       // dcl_uav_structured u0
    append_word(0x0011E000u); append_word(0u);
    append_word(4u);
    append_word(0x070000A8u);                       // store_structured g0.x
    append_word(0x0011F012u); append_word(0u);
    append_word(0x00020E46u);                       // element = vThreadID
    append_word(0x00004001u); append_word(0u);      // byte offset l(0)
    append_word(0x00020E46u);                       // src = vThreadID
    append_word(0x010018BEu);                       // sync_g_t
    append_word(0x080000A7u);                       // ld_structured r0, g0
    append_word(0x001000F2u); append_word(0u);
    append_word(0x00020E46u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x0011FE46u); append_word(0u);      // g0.xyzw
    append_word(0x080000A8u);                       // store_structured u0
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00100E46u); append_word(0u);      // src r0
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // stride 4 * count 8 = 32 bytes = 8 words of threadgroup storage, and the
    // barrier between the write and the read survives.
    EXPECT_NE(msl.find("threadgroup uint g0[8]"), std::string::npos) << msl;
    EXPECT_NE(msl.find("threadgroup_barrier"), std::string::npos) << msl;
}
