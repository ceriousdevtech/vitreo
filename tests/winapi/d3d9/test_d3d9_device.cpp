/// @file test_d3d9_device.cpp
/// @brief Device creation, vertex declarations, clear, scene bracketing and reset.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;
using vitreo::winapi::d3d9_test::test_f32_bits;

// =============================================================================
// IDirect3DDevice9 — creation and presentation
// =============================================================================

TEST_F(D3D9ManagerTest, CreateDeviceMapsADeviceWithACompleteVtable) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    const u32 vtable = memory_.read_u32(device).value();
    ASSERT_NE(vtable, 0u);
    const auto& expected =
        interface_thunks_[static_cast<usize>(D3D9InterfaceId::Device)];
    ASSERT_EQ(expected.size(), 119u);
    for (usize slot = 0; slot < expected.size(); ++slot) {
        EXPECT_EQ(memory_.read_u32(vtable + slot * sizeof(u32)).value(),
                  expected[slot])
            << "device vtable slot " << slot;
    }
    EXPECT_EQ(backend_.record.swap_chains_created, 1u);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.width, 640u);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.height, 480u);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.pixel_format,
              graphics::PixelFormat::BGRA8Unorm);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.native_view,
              &native_view_token_);
}

TEST_F(D3D9ManagerTest, CreateVertexDeclarationBindsAndClearsFvf) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    // POSITION FLOAT3 @0, COLOR D3DCOLOR @12, END.
    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},   // FLOAT3 POSITION
        {0, 12, 4, 0, 10, 0}, // D3DCOLOR COLOR
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, 0x42}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 decl = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(decl, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, decl}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 90,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 88,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x108)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x108).value(), decl);

    const u32 vtable = memory_.read_u32(decl).value();
    ASSERT_NE(vtable, 0u);
    EXPECT_EQ(memory_.read_u32(vtable).value(),
              interface_thunks_[static_cast<usize>(
                  D3D9InterfaceId::VertexDeclaration)][0]);
}

TEST_F(D3D9ManagerTest, CreateVertexDeclarationAcceptsUByte4AndFloat16) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    // POSITION FLOAT3 @0, BLENDINDICES UBYTE4 @12, TEXCOORD FLOAT16_2 @16.
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},     // FLOAT3 POSITION
        {0, 12, 5, 0, 2, 0},    // UBYTE4 BLENDINDICES
        {0, 16, 15, 0, 5, 0},   // FLOAT16_2 TEXCOORD
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration =
        memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
}

TEST_F(D3D9ManagerTest, CreateVertexDeclarationAcceptsDec3NAndUnused) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    // POSITION FLOAT3 @0, UNUSED padding @12, NORMAL DEC3N @16.
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},     // FLOAT3 POSITION
        {0, 12, 17, 0, 0, 1},   // UNUSED (stride padding, no attribute)
        {0, 16, 14, 0, 3, 0},   // DEC3N NORMAL
        {0xFF, 0, 17, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration =
        memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87,
                     {device, declaration}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 20 * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 0}),
              D3D_OK);
    const u32 vertex_buffer =
        memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, vertex_buffer, 0, 20}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4, 0, 1}),
              D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::Float3);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Dec3N);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 16u);
}

TEST_F(D3D9ManagerTest, UByte4NVertexElementsPreserveRgbaByteOrder) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},    // FLOAT3 POSITION
        {0, 12, 8, 0, 10, 0},  // UBYTE4N COLOR, not D3DCOLOR
        {0xFF, 0, 17, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration =
        memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87,
                     {device, declaration}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 16 * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 0}),
              D3D_OK);
    const u32 vertex_buffer =
        memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, vertex_buffer, 0, 16}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4, 0, 1}),
              D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::Float3);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::UByte4Normalized);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
}

TEST_F(D3D9ManagerTest, DeviceQueryInterfaceAcceptsOnlyItsOwnIids) {
    const auto [factory, device] = create_device();
    // IDirect3DDevice9 {d0223b96-bf7a-43fd-92bd-a43b0d82b9eb} (d3d9.h). The
    // previous bytes matched the frontend's equally wrong constant.
    constexpr std::array<u8, 16> iid_device{
        0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43,
        0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB};
    constexpr std::array<u8, 16> iid_d3d9{
        0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
        0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};

    ASSERT_TRUE(memory_.write_bytes(IID_ADDR, iid_device.data(), 16).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 0,
                     {device, static_cast<u32>(IID_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), device);

    // IDirect3D9 is a different interface; the out parameter must be cleared.
    ASSERT_TRUE(memory_.write_bytes(IID_ADDR, iid_d3d9.data(), 16).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 0,
                     {device, static_cast<u32>(IID_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              E_NOINTERFACE_D3D);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, ClearReachesTheBackendWithTheUnpackedColor) {
    const auto [factory, device] = create_device();
    // Opaque mid-grey: A=0xFF R=0x40 G=0x80 B=0xC0.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET, 0xFF4080C0u, 0, 0}),
              D3D_OK);
    ASSERT_EQ(backend_.record.back_buffer_clears, 1u);
    EXPECT_NEAR(backend_.record.last_clear[0], 0x40 / 255.0f, 1e-6);
    EXPECT_NEAR(backend_.record.last_clear[1], 0x80 / 255.0f, 1e-6);
    EXPECT_NEAR(backend_.record.last_clear[2], 0xC0 / 255.0f, 1e-6);
    EXPECT_NEAR(backend_.record.last_clear[3], 1.0f, 1e-6);
}

TEST_F(D3D9ManagerTest, ClearWithRectangleExpandsToFullTarget) {
    // RCT3 issues Clear with a D3DRECT even for whole-target clears. Returning
    // E_NOTIMPL left R5G6B5 lightmap RTs as undefined Metal memory (neon green).
    const auto [factory, device] = create_device();
    // D3DRECT {0,0,640,480} covering the default test back buffer.
    const i32 rect[4] = {0, 0, 640, 480};
    ASSERT_TRUE(memory_.write_bytes(SCRATCH_ADDR,
                                    reinterpret_cast<const Byte*>(rect),
                                    sizeof(rect)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR),
                      D3DCLEAR_TARGET, 0xFF00FF00u, 0, 0}),
              D3D_OK);
    ASSERT_EQ(backend_.record.back_buffer_clears, 1u);
    EXPECT_NEAR(backend_.record.last_clear[1], 1.0f, 1e-6);
}

TEST_F(D3D9ManagerTest, ClearRejectsInvalidFlagAndRectangleCombinations) {
    const auto [factory, device] = create_device();

    // No flags at all.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43, {device, 0, 0, 0, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    // A count without rectangles.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 1, 0, D3DCLEAR_TARGET, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    // Clearing a depth buffer the device was not created with.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_ZBUFFER, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.back_buffer_clears, 0u);
}

TEST_F(D3D9ManagerTest, ClearAcceptsDepthWhenTheDeviceHasAutoDepthStencil) {
    const auto [factory, device] = create_device(true);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                      0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.back_buffer_clears, 1u);
}

TEST_F(D3D9ManagerTest, ValidateDeviceReportsOnePassOnFreshDevice) {
    // Learn: ValidateDevice — null out → INVALIDCALL; success writes the
    // number of passes needed. Vitreo's draw path has no multipass fallbacks,
    // so accepted default state reports 1.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 70, {device, 0}),
              D3DERR_INVALIDCALL);

    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 70,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 1u);

    // N-patches have no tessellated draw path — refuse rather than claim
    // the current state is renderable in one pass.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 79,
                     {device, test_f32_bits(2.0f)}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 70,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              E_NOTIMPL_D3D);
}

TEST_F(D3D9ManagerTest, SceneBracketingFollowsTheWindowsStateMachine) {
    const auto [factory, device] = create_device();

    // EndScene without BeginScene.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    // Nested BeginScene.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}),
              D3DERR_INVALIDCALL);
    // Present inside a scene.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 17, {device, 0, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.presents, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 17, {device, 0, 0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.presents, 1u);
}

TEST_F(D3D9ManagerTest,
       ResetWindowedZeroBackBufferUsesClientArea) {
    constexpr u32 kClientW = 800;
    constexpr u32 kClientH = 600;
    mgr_.set_client_size_resolver(
        [](u32 hwnd, u32& width, u32& height) {
            if (hwnd != TEST_HWND) return false;
            width = kClientW;
            height = kClientH;
            return true;
        });

    const auto [factory, device] = create_device();
    // Learn: windowed Reset with BackBufferWidth/Height == 0 uses the
    // ClientSizeResolver path (same as CreateDevice).
    write_present_parameters(0, 0);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 16,
                     {device, static_cast<u32>(PRESENT_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR).value(), kClientW);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR + 4).value(), kClientH);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.width, kClientW);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.height, kClientH);
    EXPECT_NE(backend_.record.last_swap_chain_desc.width, 1920u);
    EXPECT_NE(backend_.record.last_swap_chain_desc.height, 1080u);
}

TEST_F(D3D9ManagerTest, ResetRebuildsPresentationAndRestoresDefaultState) {
    const auto [factory, device] = create_device();

    // Move the viewport away from its default so Reset has something to undo.
    const std::array<u32, 6> viewport{10, 20, 100, 50, test_f32_bits(0.25f),
                                      test_f32_bits(0.75f)};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 47,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);

    write_present_parameters(800, 600);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 16,
                     {device, static_cast<u32>(PRESENT_ADDR)}),
              D3D_OK);
    EXPECT_EQ(backend_.record.swap_chains_created, 2u);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.width, 800u);

    std::array<u32, 6> after{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 48,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR, reinterpret_cast<Byte*>(after.data()),
        sizeof(after)).is_ok());
    EXPECT_EQ(after[0], 0u);
    EXPECT_EQ(after[1], 0u);
    EXPECT_EQ(after[2], 800u);
    EXPECT_EQ(after[3], 600u);
}

TEST_F(D3D9ManagerTest, ResetIsRejectedInsideAScene) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    write_present_parameters(800, 600);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 16,
                     {device, static_cast<u32>(PRESENT_ADDR)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.swap_chains_created, 1u);
}

TEST_F(D3D9ManagerTest, SetViewportRejectsARegionOutsideTheRenderTarget) {
    const auto [factory, device] = create_device();
    const std::array<u32, 6> viewport{0, 0, 641, 480, 0, test_f32_bits(1.0f)};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 47,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_INVALIDCALL);
}
