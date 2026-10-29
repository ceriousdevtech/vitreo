// SPDX-License-Identifier: LGPL-2.1-only
//
// Typed UAV formats, interface shaders, input layouts and the bounds-guard view sizes.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"
#include "d3d11_refused_shaders.h"
#include "d3d11_oracle_blobs.hpp"
namespace classlink_blobs {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_classlink_shaders.h"
#pragma clang diagnostic pop
}  // namespace classlink_blobs

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, CheckFormatSupportReportsTypedUavGroup) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr OUT = DATA_BASE + 0x300;
    constexpr u32 kTypedUav = 0x2000000u; // TYPED_UNORDERED_ACCESS_VIEW
    constexpr u32 kLoadStore = 0xc0u;     // UAV_TYPED_LOAD | UAV_TYPED_STORE

    // The group this backend supports, by DXGI_FORMAT value.
    for (const u32 format : {41u,  // R32_FLOAT (always required)
                             42u,  // R32_UINT  (always required)
                             43u,  // R32_SINT  (always required)
                             2u,   // R32G32B32A32_FLOAT
                             3u,   // R32G32B32A32_UINT
                             4u,   // R32G32B32A32_SINT
                             10u,  // R16G16B16A16_FLOAT
                             12u,  // R16G16B16A16_UINT
                             14u,  // R16G16B16A16_SINT
                             28u,  // R8G8B8A8_UNORM
                             30u,  // R8G8B8A8_UINT
                             32u,  // R8G8B8A8_SINT
                             54u,  // R16_FLOAT
                             57u,  // R16_UINT
                             61u,  // R8_UNORM
                             62u,  // R8_UINT
                             64u}) { // R8_SINT
        ASSERT_TRUE(memory.write_u32(OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, format);
        set_arg(2, OUT);
        d3d11.impl_DeviceMethod(marshaler, 29);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << "format " << format;
        EXPECT_EQ(memory.read_u32(OUT).value() & kTypedUav, kTypedUav)
            << "CheckFormatSupport format " << format;

        ASSERT_TRUE(memory.write_u32(OUT, format).is_ok());
        ASSERT_TRUE(memory.write_u32(OUT + 4, 0).is_ok());
        set_arg(0, device);
        set_arg(1, 3); // D3D11_FEATURE_FORMAT_SUPPORT2
        set_arg(2, OUT);
        set_arg(3, 8);
        d3d11.impl_DeviceMethod(marshaler, 33);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << "format " << format;
        EXPECT_EQ(memory.read_u32(OUT + 4).value() & kLoadStore, kLoadStore)
            << "FORMAT_SUPPORT2 format " << format;
        // InFormat is an input the device must leave alone.
        EXPECT_EQ(memory.read_u32(OUT).value(), format);
    }

    // Microsoft Learn distinguishes UAV_TYPED_STORE from UAV_TYPED_LOAD.
    // Metal's Write capability covers these formats; read_write does not.
    for (const u32 format : {49u,   // R8G8_UNORM
                             26u,   // R11G11B10_FLOAT
                             24u}) { // R10G10B10A2_UNORM
        ASSERT_TRUE(memory.write_u32(OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, format);
        set_arg(2, OUT);
        d3d11.impl_DeviceMethod(marshaler, 29);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << "format " << format;
        EXPECT_EQ(memory.read_u32(OUT).value() & kTypedUav, kTypedUav)
            << "CheckFormatSupport format " << format;
        for (u32 feature : {2u, 3u}) {
            ASSERT_TRUE(memory.write_u32(OUT, format).is_ok());
            ASSERT_TRUE(memory.write_u32(OUT + 4, 0).is_ok());
            set_arg(0, device);
            set_arg(1, feature);
            set_arg(2, OUT);
            set_arg(3, 8);
            d3d11.impl_DeviceMethod(marshaler, 33);
            ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
            const u32 mask = feature == 2 ? kTypedUav : kLoadStore;
            const u32 expected = feature == 2 ? kTypedUav :
                D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
            EXPECT_EQ(memory.read_u32(OUT + 4).value() & mask, expected);
            EXPECT_EQ(memory.read_u32(OUT).value(), format);
        }
    }

    // Learn D3D11_FEATURE_DATA_D3D11_OPTIONS2: eight 4-byte members, so the
    // struct is 32 bytes and a caller passing sizeof() gets S_OK, not
    // E_INVALIDARG. TypedUAVLoadAdditionalFormats is the second, and has to
    // agree with the per-format answers above.
    constexpr GuestAddr OPTIONS2 = DATA_BASE + 0x340;
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u32(OPTIONS2 + i * 4, 0xFFFFFFFFu).is_ok());
    }
    set_arg(0, device);
    set_arg(1, 14); // D3D11_OPTIONS2
    set_arg(2, OPTIONS2);
    set_arg(3, 32);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OPTIONS2).value(), 0u);     // PSSpecifiedStencilRef
    EXPECT_EQ(memory.read_u32(OPTIONS2 + 4).value(), 1u); // TypedUAVLoadAdditionalFormats
    EXPECT_EQ(memory.read_u32(OPTIONS2 + 8).value(), 0u); // ROVsSupported

    // A wrong size is still E_INVALIDARG (Windows contract).
    set_arg(0, device);
    set_arg(1, 14);
    set_arg(2, OPTIONS2);
    set_arg(3, 20);
    d3d11.impl_DeviceMethod(marshaler, 33);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

// CreateClassLinkage makes a real linkage (measured, a12 [cl]: S_OK, one
// device reference). What does not exist is the translator's interface
// lowering (fcall), so a shader that declares interfaces is refused: with a
// NULL linkage by WARP's own answer, E_INVALIDARG (Windows,
// d3d11_a09a_alu_probe.expected `iface_xor: CreateComputeShader(NULL
// linkage) hr=0x80070057`); with a linkage -- where Windows succeeds -- by a
// loud E_NOTIMPL, never a shader that silently computes nothing.
// A shader that declares interfaces (dcl_interface / fcall, D3D11.3 7.19)
// is E_INVALIDARG without a class linkage and created with one, in every
// stage; with a NULL out-parameter the call validates (S_FALSE). Expected:
// tools/oracle_probe/d3d11_classlink_probe.expected [create] (and the a09a
// probe's iface_xor lines for the compute shader below).
TEST_F(D3D11ManagerTest, InterfaceShadersNeedALinkageInEveryStage) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr LINKAGE_OUT = DATA_BASE + 0x2C0;
    constexpr GuestAddr SHADER_OUT = DATA_BASE + 0x2C8;
    // The shader blobs are larger than the fixture's data page.
    constexpr GuestAddr BYTECODE = 0x100000;
    ASSERT_TRUE(memory.map_region(BYTECODE, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());

    set_arg(0, device);
    set_arg(1, LINKAGE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 19); // CreateClassLinkage
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr linkage = memory.read_u64(LINKAGE_OUT).value();
    ASSERT_NE(linkage, 0u);

    using namespace classlink_blobs;
    struct Stage {
        usize slot;
        const unsigned char* code;
        usize size;
        void (D3D11Manager::*release)(Marshaler&, usize);
    };
    const Stage stages[] = {
        {12, k_vs_iface, sizeof(k_vs_iface), &D3D11Manager::impl_VertexShaderMethod},
        {13, k_gs_iface, sizeof(k_gs_iface), &D3D11Manager::impl_GeometryShaderMethod},
        {15, k_ps_iface, sizeof(k_ps_iface), &D3D11Manager::impl_PixelShaderMethod},
        {16, k_hs_iface, sizeof(k_hs_iface), &D3D11Manager::impl_HullShaderMethod},
        {17, k_ds_iface, sizeof(k_ds_iface), &D3D11Manager::impl_DomainShaderMethod},
        {18, k_cs_multi, sizeof(k_cs_multi), &D3D11Manager::impl_ComputeShaderMethod},
        {18, a09a_cs_iface, sizeof(a09a_cs_iface),
         &D3D11Manager::impl_ComputeShaderMethod},
    };
    for (const Stage& stage : stages) {
        ASSERT_TRUE(memory.write_bytes(BYTECODE, stage.code, stage.size).is_ok());
        for (int form = 0; form < 3; ++form) {
            ASSERT_TRUE(memory.write_u64(SHADER_OUT, 0xA5A5A5A5ull).is_ok());
            set_arg(0, device);
            set_arg(1, BYTECODE);
            set_arg(2, stage.size);
            set_arg(3, form == 0 ? 0 : linkage);
            set_arg(4, form == 2 ? 0 : SHADER_OUT);
            d3d11.impl_DeviceMethod(marshaler, stage.slot);
            const u64 hr = cpu.gpr.rax;
            if (form == 0) {
                EXPECT_EQ(hr, d3d11::E_INVALIDARG) << stage.slot;
                EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);
            } else if (form == 1) {
                EXPECT_EQ(hr, d3d11::S_OK) << stage.slot;
                const GuestAddr shader = memory.read_u64(SHADER_OUT).value();
                EXPECT_NE(shader, 0u);
                set_arg(0, shader);
                (d3d11.*stage.release)(marshaler, 2);
                EXPECT_EQ(cpu.gpr.rax, 0u);
            } else {
                EXPECT_EQ(hr, 1u) << stage.slot;  // S_FALSE
            }
        }
    }
    // A pClassLinkage that is not a linkage is refused.
    ASSERT_TRUE(memory.write_bytes(BYTECODE, a09a_cs_iface,
                                   sizeof(a09a_cs_iface)).is_ok());
    set_arg(0, device);
    set_arg(1, BYTECODE);
    set_arg(2, sizeof(a09a_cs_iface));
    set_arg(3, device);
    set_arg(4, SHADER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    set_arg(0, linkage);
    d3d11.impl_ClassLinkageMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// Learn D3D11_FEATURE_DATA_D3D11_OPTIONS2: a pixel shader that writes
// SV_StencilRef needs PSSpecifiedStencilRefSupported. This device reports
// it FALSE, so the shader is refused as WARP refuses it on an exact 11_0
// device, where the cap is 0 too (Windows d3d11_a09b [ps_stencil_ref]:
// CreatePixelShader hr=0x80070057) -- accepting it promised a stencil write
// the device says it cannot do.
TEST_F(D3D11ManagerTest, StencilRefPixelShaderIsRefusedWithoutTheCap) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr OPTIONS2 = DATA_BASE + 0x2C0;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x2F0;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;

    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u32(OPTIONS2 + i * 4, 0xFFFFFFFFu).is_ok());
    }
    set_arg(0, device);
    set_arg(1, 14); // D3D11_FEATURE_D3D11_OPTIONS2
    set_arg(2, OPTIONS2);
    set_arg(3, 32);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_EQ(memory.read_u32(OPTIONS2).value(), 0u); // PSSpecifiedStencilRef

    ASSERT_TRUE(memory.write_bytes(PS_BYTECODE, a09b_ps_stencil_ref,
                                   sizeof(a09b_ps_stencil_ref)).is_ok());
    ASSERT_TRUE(memory.write_u64(PS_OUT, 0xA5A5A5A5ull).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(a09b_ps_stencil_ref));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15); // CreatePixelShader
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(PS_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, CheckFeatureSupportReportsSharingAndThreading) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr THREADING = DATA_BASE + 0x280;
    ASSERT_TRUE(memory.write_u32(THREADING, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(THREADING + 4, 0xFFFFFFFFu).is_ok());
    set_arg(0, device);
    set_arg(1, 0); // THREADING
    set_arg(2, THREADING);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(THREADING).value(), 1u);     // ConcurrentCreates
    EXPECT_EQ(memory.read_u32(THREADING + 4).value(), 0u); // CommandLists

    constexpr GuestAddr OPTIONS5 = DATA_BASE + 0x290;
    ASSERT_TRUE(memory.write_u32(OPTIONS5, 0).is_ok());
    set_arg(0, device);
    set_arg(1, 19); // D3D11_OPTIONS5
    set_arg(2, OPTIONS5);
    set_arg(3, 4);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // Learn D3D11_SHARED_RESOURCE_TIER: TIER_1 needs ExtendedResourceSharing
    // and OPTIONS4 ExtendedNV12SharedTextureSupported, both FALSE with this
    // test backend (it shares single images only), so TIER_0.
    EXPECT_EQ(memory.read_u32(OPTIONS5).value(), 0u);

    constexpr GuestAddr OPTIONS3 = DATA_BASE + 0x2A0;
    ASSERT_TRUE(memory.write_u32(OPTIONS3, 0).is_ok());
    set_arg(0, device);
    set_arg(1, 15); // D3D11_OPTIONS3
    set_arg(2, OPTIONS3);
    set_arg(3, 4);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer (11.26).
    EXPECT_EQ(memory.read_u32(OPTIONS3).value(), 1u);

    constexpr GuestAddr FORMAT2 = DATA_BASE + 0x298;
    ASSERT_TRUE(memory.write_u32(FORMAT2, 87).is_ok()); // B8G8R8A8_UNORM
    ASSERT_TRUE(memory.write_u32(FORMAT2 + 4, 0).is_ok());
    set_arg(0, device);
    set_arg(1, 3); // FORMAT_SUPPORT2
    set_arg(2, FORMAT2);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(FORMAT2).value(), 87u); // InFormat preserved
    // SHAREABLE, and UAV_TYPED_STORE: a typed-UAV store of B8G8R8A8_UNORM
    // matches Windows (d3d11_a08_formats_probe "F087 uavst"); WARP's
    // optional typed load has no Metal read_write form for BGRA8.
    EXPECT_EQ(memory.read_u32(FORMAT2 + 4).value(), 0x480u);
}

TEST_F(D3D11ManagerTest, InputLayoutsRetainBoundObjectsAndRecycleReleasedStorage) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr ELEMENTS = DATA_BASE + 0x280;
    constexpr GuestAddr SEMANTIC = DATA_BASE + 0x2C0;
    // A real fxc blob: CreateInputLayout checks the container hash and the
    // signature. SV_VertexID-only, so the POSITION element is simply extra
    // (a05 "il vs_id: unused POSITION -> 0x00000000").
    constexpr GuestAddr BYTECODE = DATA_BASE + 0x2000;
    constexpr usize BYTECODE_SIZE = sizeof(test_blobs::kVsVertexIdOnly);
    constexpr GuestAddr LAYOUT_OUT = DATA_BASE + 0x2F0;
    ASSERT_TRUE(memory.write_bytes(
        SEMANTIC, reinterpret_cast<const Byte*>("POSITION"), 9).is_ok());
    // One x64 D3D11_INPUT_ELEMENT_DESC (32 bytes): SemanticName ptr + fields.
    ASSERT_TRUE(memory.write_u64(ELEMENTS, SEMANTIC).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 8, 0).is_ok());  // SemanticIndex
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 12, 2).is_ok()); // R32G32B32A32_FLOAT
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 28, 0).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        BYTECODE, reinterpret_cast<const Byte*>(test_blobs::kVsVertexIdOnly),
        BYTECODE_SIZE).is_ok());

    set_arg(0, device);
    set_arg(1, ELEMENTS);
    set_arg(2, 1);
    set_arg(3, BYTECODE);
    set_arg(4, BYTECODE_SIZE);
    set_arg(5, LAYOUT_OUT);
    d3d11.impl_DeviceMethod(marshaler, 11);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(LAYOUT_OUT).value(), 0u);

    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    set_arg(0, context);
    const GuestAddr layout = memory.read_u64(LAYOUT_OUT).value();
    set_arg(1, layout);
    d3d11.impl_ContextMethod(marshaler, 17); // IASetInputLayout
    set_arg(0, layout);
    d3d11.impl_InputLayoutMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(
        kept_alive_by_runtime(layout, &D3D11Manager::impl_InputLayoutMethod))
        << "context must retain the input layout";
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110); // ClearState releases it.
    for (unsigned iteration = 0; iteration < 8192; ++iteration) {
        set_arg(0, device);
        set_arg(1, ELEMENTS);
        set_arg(2, 1);
        set_arg(3, BYTECODE);
        set_arg(4, BYTECODE_SIZE);
        set_arg(5, LAYOUT_OUT);
        d3d11.impl_DeviceMethod(marshaler, 11);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << iteration;
        const GuestAddr recycled = memory.read_u64(LAYOUT_OUT).value();
        EXPECT_EQ(recycled, layout);
        set_arg(0, recycled);
        d3d11.impl_InputLayoutMethod(marshaler, 2);
        ASSERT_EQ(cpu.gpr.rax, 0u);
    }
}

TEST_F(D3D11ManagerTest, CreateInputLayoutRejectsNullSemanticName) {
    // Learn D3D11_INPUT_ELEMENT_DESC.SemanticName is the HLSL semantic
    // matched to the shader input signature; a null name is invalid.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr ELEMENTS = DATA_BASE + 0x280;
    constexpr GuestAddr BYTECODE = DATA_BASE + 0x2000;
    constexpr GuestAddr LAYOUT_OUT = DATA_BASE + 0x2F0;
    ASSERT_TRUE(memory.write_u64(ELEMENTS, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 12, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ELEMENTS + 28, 0).is_ok());
    // A valid blob, so the NULL name is the only thing wrong.
    ASSERT_TRUE(memory.write_bytes(
        BYTECODE, reinterpret_cast<const Byte*>(test_blobs::kVsVertexIdOnly),
        sizeof(test_blobs::kVsVertexIdOnly)).is_ok());

    set_arg(0, device);
    set_arg(1, ELEMENTS);
    set_arg(2, 1);
    set_arg(3, BYTECODE);
    set_arg(4, sizeof(test_blobs::kVsVertexIdOnly));
    set_arg(5, LAYOUT_OUT);
    d3d11.impl_DeviceMethod(marshaler, 11);
    ASSERT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(LAYOUT_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, CreateInputLayoutAllowsEmptyElementList) {
    // A vertex shader that reads only SV_VertexID has an empty input
    // signature, and CreateInputLayout with zero elements succeeds -- but only
    // with a non-NULL element array. Measured, d3d11_a05_context_state_probe:
    // "il vs_id: 0 elements -> 0x80070057" (NULL array) and "il vs_id: 0
    // elements, non-NULL array -> 0x00000000". (This test used to expect S_OK
    // for the NULL array; the oracle proved that wrong.)
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr ELEMENTS = DATA_BASE + 0x280;
    constexpr GuestAddr BYTECODE = DATA_BASE + 0x2000;
    constexpr GuestAddr LAYOUT_OUT = DATA_BASE + 0x2F0;
    ASSERT_TRUE(memory.write_bytes(
        BYTECODE, reinterpret_cast<const Byte*>(test_blobs::kVsVertexIdOnly),
        sizeof(test_blobs::kVsVertexIdOnly)).is_ok());
    for (const GuestAddr elements : {GuestAddr{0}, ELEMENTS}) {
        ASSERT_TRUE(memory.write_u64(LAYOUT_OUT, 0xDEADBEEFull).is_ok());
        set_arg(0, device);
        set_arg(1, elements);
        set_arg(2, 0);
        set_arg(3, BYTECODE);
        set_arg(4, sizeof(test_blobs::kVsVertexIdOnly));
        set_arg(5, LAYOUT_OUT);
        d3d11.impl_DeviceMethod(marshaler, 11);
        if (elements == 0) {
            EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
            EXPECT_EQ(memory.read_u64(LAYOUT_OUT).value(), 0u);
        } else {
            EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
            EXPECT_NE(memory.read_u64(LAYOUT_OUT).value(), 0u);
        }
    }
}

TEST_F(D3D11ManagerTest, ContextRetainsConstantBuffersAndPipelineStates) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BUFFER_DESC = DATA_BASE + 0x280;
    constexpr GuestAddr BUFFER_OUT = DATA_BASE + 0x2A0;
    constexpr GuestAddr BINDING = DATA_BASE + 0x2B0;
    constexpr GuestAddr RASTER_DESC = DATA_BASE + 0x300;
    constexpr GuestAddr RASTER_OUT = DATA_BASE + 0x340;
    constexpr GuestAddr DEPTH_DESC = DATA_BASE + 0x380;
    constexpr GuestAddr DEPTH_OUT = DATA_BASE + 0x3C0;

    const std::array<u32, 6> buffer_desc{
        16, 0, 0x4, 0, 0, 0, // DEFAULT, BIND_CONSTANT_BUFFER
    };
    ASSERT_TRUE(memory.write_bytes(
        BUFFER_DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(BUFFER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDING, buffer).is_ok());
    for (const usize slot : {7u, 16u}) {
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 1);
        set_arg(3, BINDING);
        d3d11.impl_ContextMethod(marshaler, slot);
    }
    // Release answers the application's count (measured, a05 "bound:
    // cbA+0"); the bindings keep the buffer alive -- each of them: with the
    // VS slot cleared the PS slot still holds it.
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u64(BINDING + 8, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING + 8);
    d3d11.impl_ContextMethod(marshaler, 7);  // VSSetConstantBuffers(NULL)
    EXPECT_TRUE(kept_alive_by_runtime(buffer, &D3D11Manager::impl_BufferMethod))
        << "VS and PS slots must each retain the constant buffer";

    const std::array<u32, 10> raster_desc{
        3, 1, 0, 0, 0, 0, 1, 0, 0, 0,
    }; // SOLID, CULL_NONE, DepthClipEnable
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rasterizer = memory.read_u64(RASTER_OUT).value();
    set_arg(0, context);
    set_arg(1, rasterizer);
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, rasterizer);
    d3d11.impl_RasterizerStateMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        rasterizer, &D3D11Manager::impl_RasterizerStateMethod));

    const std::array<u32, 13> depth_desc{
        1, 1, 2, 0, 0x0000FFFFu,
        1, 1, 1, 8,
        1, 1, 1, 8,
    };
    ASSERT_TRUE(memory.write_bytes(
        DEPTH_DESC, reinterpret_cast<const Byte*>(depth_desc.data()),
        sizeof(depth_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, DEPTH_DESC);
    set_arg(2, DEPTH_OUT);
    d3d11.impl_DeviceMethod(marshaler, 21);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr depth = memory.read_u64(DEPTH_OUT).value();
    set_arg(0, context);
    set_arg(1, depth);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 36);
    set_arg(0, depth);
    d3d11.impl_DepthStencilStateMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        depth, &D3D11Manager::impl_DepthStencilStateMethod));

    // ClearState drops the context's references, and with them the objects
    // only the context kept alive.
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110);
    EXPECT_FALSE(
        kept_alive_by_runtime(buffer, &D3D11Manager::impl_BufferMethod));
    EXPECT_FALSE(kept_alive_by_runtime(
        rasterizer, &D3D11Manager::impl_RasterizerStateMethod));
    // This depth-stencil description is the family's default, whose object
    // the runtime itself keeps (d3d11_a04_state_objects_probe, StatePoolEntry).
    EXPECT_TRUE(kept_alive_by_runtime(
        depth, &D3D11Manager::impl_DepthStencilStateMethod));
}

TEST_F(D3D11ManagerTest, Device3CreatesDefaultExtendedRasterizerState) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr desc = DATA_BASE + 0x300;
    constexpr GuestAddr output = DATA_BASE + 0x340;
    const std::array<u32, 12> raster_desc{
        3, 1, 0, 0, 0, 0, 1, 0, 0, 0,
        0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        desc, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, desc);
    set_arg(2, output);
    d3d11.impl_DeviceMethod(marshaler, 56);

    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rasterizer = memory.read_u64(output).value();
    ASSERT_NE(rasterizer, 0u);

    constexpr GuestAddr iid = DATA_BASE + 0x350;
    constexpr GuestAddr queried = DATA_BASE + 0x360;
    constexpr GuestAddr returned_desc = DATA_BASE + 0x380;
    ASSERT_TRUE(memory.write_bytes(
        iid, reinterpret_cast<const Byte*>(&IID_ID3D11RASTERIZERSTATE2),
        sizeof(IID_ID3D11RASTERIZERSTATE2)).is_ok());
    set_arg(0, rasterizer);
    set_arg(1, iid);
    set_arg(2, queried);
    d3d11.impl_RasterizerStateMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(queried).value(), rasterizer);

    set_arg(0, rasterizer);
    set_arg(1, returned_desc);
    d3d11.impl_RasterizerStateMethod(marshaler, 9);
    EXPECT_EQ(memory.read_u32(returned_desc).value(), 3u);
    EXPECT_EQ(memory.read_u32(returned_desc + 40).value(), 0u);
    EXPECT_EQ(memory.read_u32(returned_desc + 44).value(), 0u);

    set_arg(0, rasterizer);
    d3d11.impl_RasterizerStateMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    set_arg(0, rasterizer);
    d3d11.impl_RasterizerStateMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// Learn (StructuredBuffer / ByteAddressBuffer): a load past the bound view
// returns 0. The translator enforces it from the view's word count in the
// stage push block (member 2, one entry per t# then u# slot), so every stage
// that runs a guarded shader must receive that block on every draw -- a vertex
// shader reading GPU-scene transforms from t0..t2 included -- and each slot
// must carry its own view's size, t9 and t100 as much as t0.
TEST_F(D3D11ManagerTest, DrawBindsEveryStagesBufferViewSizesForTheBoundsGuard) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BUFFER_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr VIEW_WIDE_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr VIEW_WINDOW_OUT = DATA_BASE + 0x910;
    constexpr GuestAddr RT_TEXTURE_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr RTV_OUT = DATA_BASE + 0x928;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x930;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x938;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x1C00;

    // D3D11_BUFFER_DESC: 1024 bytes, DEFAULT, SHADER_RESOURCE, structured
    // with a 16-byte stride.
    const std::array<u32, 6> buffer_desc{1024, 0, 0x8, 0, 0x40, 16};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(BUFFER_OUT).value();

    const auto create_buffer_srv = [&](u32 first, u32 count, GuestAddr out) {
        // D3D11_SHADER_RESOURCE_VIEW_DESC: DXGI_FORMAT_UNKNOWN,
        // D3D11_SRV_DIMENSION_BUFFER, FirstElement, NumElements.
        const std::array<u32, 4> desc{0, 1, first, count};
        EXPECT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, buffer);
        set_arg(2, SRV_DESC);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    const GuestAddr wide = create_buffer_srv(0, 64, VIEW_WIDE_OUT);
    const GuestAddr window = create_buffer_srv(4, 16, VIEW_WINDOW_OUT);
    ASSERT_NE(wide, 0u);
    ASSERT_NE(window, 0u);

    const std::array<u32, 11> rt_desc{16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(rt_desc.data()),
        sizeof(rt_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, RT_TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, device);
    set_arg(1, memory.read_u64(RT_TEXTURE_OUT).value());
    set_arg(2, 0);
    set_arg(3, RTV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(RTV_OUT).value();

    ASSERT_TRUE(memory.write_bytes(
        VS_BYTECODE, vs_main, sizeof(vs_main)).is_ok());
    set_arg(0, device);
    set_arg(1, VS_BYTECODE);
    set_arg(2, sizeof(vs_main));
    set_arg(3, 0);
    set_arg(4, VS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 12);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_bytes(
        PS_BYTECODE, ps_color, sizeof(ps_color)).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(ps_color));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(VS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);  // VSSetShader
    set_arg(0, context);
    set_arg(1, memory.read_u64(PS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);   // PSSetShader

    const auto bind = [&](u32 method, u32 slot, GuestAddr view) {
        ASSERT_TRUE(memory.write_u64(BINDINGS, view).is_ok());
        set_arg(0, context);
        set_arg(1, slot);
        set_arg(2, 1);
        set_arg(3, BINDINGS);
        d3d11.impl_ContextMethod(marshaler, method);
    };
    bind(25, 0, wide);    // VSSetShaderResources t0
    bind(25, 9, window);  // VSSetShaderResources t9
    bind(8, 2, window);   // PSSetShaderResources t2
    bind(8, 100, wide);   // PSSetShaderResources t100

    ASSERT_TRUE(memory.write_u64(BINDINGS, rtv).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, BINDINGS);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);  // OMSetRenderTargets
    set_arg(0, context);
    set_arg(1, 4);  // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    d3d11.impl_ContextMethod(marshaler, 24);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);  // Draw
    ASSERT_EQ(backend.state.draws, 1u);

    constexpr usize kSizeTable = 192;  // member 2's byte offset
    // ... then the capture SV_InstanceID bias (16 bytes), the fixed-function
    // word quad (depth offset, SampleMask, viewport depth range) and the
    // s8..s15 border colours, the b# read limits (one per slot, 16) and the
    // min/max sampler states (16 x uvec4).
    constexpr usize kBlockBytes = kSizeTable + (128 + 8) * sizeof(u32) + 16 +
                                  16 + 8 * 16 + 16 * 4 + 16 * 16;
    const auto size_at = [&](const std::vector<u8>& block, usize slot) {
        u32 words = 0;
        std::memcpy(&words, block.data() + kSizeTable + slot * 4, 4);
        return words;
    };
    const auto& vertex = backend.state.last_draw_vertex_constants;
    const auto& fragment = backend.state.last_draw_fragment_constants;
    ASSERT_EQ(vertex.size(), kBlockBytes)
        << "the vertex stage must receive the push block on every draw";
    ASSERT_EQ(fragment.size(), kBlockBytes);
    // NumElements * StructureByteStride / 4: 64*16/4 and 16*16/4.
    EXPECT_EQ(size_at(vertex, 0), 256u);
    EXPECT_EQ(size_at(vertex, 9), 64u);
    EXPECT_EQ(size_at(vertex, 1), 0u) << "no view bound: no check";
    EXPECT_EQ(size_at(fragment, 2), 64u);
    EXPECT_EQ(size_at(fragment, 100), 256u);
    EXPECT_EQ(size_at(fragment, 0), 0u);
    for (usize uav = 0; uav < 8; ++uav) {
        EXPECT_EQ(size_at(fragment, 128 + uav), 0u);
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
