/// @file test_d3d8.cpp
/// @brief Direct3D 8 frontend contract tests.
///
/// Expected values come from the SDK headers (d3d8.h / d3d8types.h /
/// d3d8caps.h, through the generated d3d8_contract.inc, which CTest
/// audit_d3d8_contract re-derives), Microsoft Learn's "Converting to
/// Direct3D 9", and the Windows d3d8.dll export table and
/// tools/oracle_probe/d3d8_contract_probe.c measurements.

#include "../d3d9/d3d9_test_fixture.hpp"

#include <winapi/graphics/d3d8/d3d8.hpp>
#include <winapi/graphics/d3d8/d3d8_contract.hpp>
#include <winapi/graphics/d3d8/d3d8_internal.hpp>

#include <cstring>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

namespace c8 = vitreo::winapi::d3d8c;
using D8 = D3D8InterfaceId;

class D3D8Test : public d3d9_test::D3D9ManagerTest {
protected:
    void SetUp() override {
        D3D9ManagerTest::SetUp();
        d3d8_.set_memory(&memory_, false);
        d3d8_.set_d3d9(&mgr_);
        GuestAddr thunk = 0x70800000;
        for (usize index = 0; index < D3D8_INTERFACE_COUNT; ++index) {
            const auto iface = static_cast<D8>(index);
            D3D8Manager::InterfaceThunks thunks(d3d8_interface_method_count(iface));
            for (GuestAddr& slot : thunks) {
                slot = thunk;
                thunk += 0x10;
            }
            d3d8_.set_interface_thunks(iface, std::move(thunks));
        }
        exports8_ = create_d3d8_thunks(d3d8_);
    }

    ExportEntry* export8(std::string_view name) {
        for (ExportEntry& entry : exports8_.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    u32 call8(D8 iface, usize slot, std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            EXPECT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4 + index * 4, argument).is_ok());
            ++index;
        }
        ExportEntry* entry = export8(d3d8_method_export_name(iface, slot));
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0xFFFFFFFFu;
        entry->handler(marshaler_);
        mgr_.d3d9_cs_flush();
        return static_cast<u32>(cpu_.gpr.rax);
    }

    template <typename Slot>
    u32 dev(Slot slot, std::initializer_list<u32> args) {
        return call8(D8::Device, static_cast<usize>(slot), args);
    }

    static usize slot9(const char* name) {
        return d3d8::d3d9_slot(D3D9InterfaceId::Device, name);
    }

    u32 rd(GuestAddr addr) { return memory_.read_u32(addr).value(); }
    void wr(GuestAddr addr, u32 value) { ASSERT_TRUE(memory_.write_u32(addr, value).is_ok()); }
    void write_iid(GuestAddr addr, const std::array<u8, 16>& iid) {
        ASSERT_TRUE(memory_.write_bytes(addr, reinterpret_cast<const Byte*>(iid.data()), 16)
                        .is_ok());
    }

    /// A windowed D3D8 D3DPRESENT_PARAMETERS (52 bytes, d3d8types.h).
    void write_pp8(u32 width, u32 height, u32 swap = c8::D3DSWAPEFFECT_DISCARD,
                   u32 interval = 0, bool depth = false) {
        namespace L = c8::layout_PRESENT_PARAMETERS;
        static_assert(L::SIZE8 == 52);
        std::vector<u8> zero(L::SIZE8, 0);
        ASSERT_TRUE(memory_.write_bytes(PP8, zero.data(), zero.size()).is_ok());
        wr(PP8 + L::BackBufferWidth.d3d8, width);
        wr(PP8 + L::BackBufferHeight.d3d8, height);
        wr(PP8 + L::BackBufferFormat.d3d8, 22);  // D3DFMT_X8R8G8B8
        wr(PP8 + L::BackBufferCount.d3d8, 1);
        wr(PP8 + L::SwapEffect.d3d8, swap);
        wr(PP8 + L::hDeviceWindow.d3d8, TEST_HWND);
        wr(PP8 + L::Windowed.d3d8, 1);
        wr(PP8 + L::EnableAutoDepthStencil.d3d8, depth ? 1 : 0);
        wr(PP8 + L::AutoDepthStencilFormat.d3d8, depth ? 75 : 0);  // D3DFMT_D24S8
        wr(PP8 + L::FullScreen_PresentationInterval.d3d8, interval);
    }

    u32 create_factory() {
        const u32 d3d = d3d8_.direct3d_create8(c8::D3D_SDK_VERSION);
        EXPECT_NE(d3d, 0u);
        return d3d;
    }

    std::pair<u32, u32> create_device8(bool depth = false) {
        const u32 d3d = create_factory();
        write_pp8(640, 480, c8::D3DSWAPEFFECT_DISCARD, 0, depth);
        const u32 hr = call8(D8::Direct3D8,
                             static_cast<usize>(c8::Direct3DSlot8::CreateDevice),
                             {d3d, 0, 1, TEST_HWND, 0x40, static_cast<u32>(PP8),
                              static_cast<u32>(OUT8)});
        EXPECT_EQ(hr, D3D_OK);
        return {d3d, rd(OUT8)};
    }

    static constexpr GuestAddr PP8 = SCRATCH_ADDR + 0x2000;
    static constexpr GuestAddr OUT8 = SCRATCH_ADDR + 0x2100;
    static constexpr GuestAddr BUF = SCRATCH_ADDR + 0x3000;
    static constexpr GuestAddr DECL = SCRATCH_ADDR + 0x5000;
    static constexpr GuestAddr FUNC = SCRATCH_ADDR + 0x5400;
    static constexpr GuestAddr IID = SCRATCH_ADDR + 0x5800;

    D3D8Manager d3d8_;
    DllExports exports8_;
};

using DS = c8::DeviceSlot8;

// -----------------------------------------------------------------------------
// Export table and vtable shapes
// -----------------------------------------------------------------------------

TEST_F(D3D8Test, ExportsMatchTheWindowsD3d8ExportTable) {
    // Windows 11 SysWOW64 d3d8.dll: ordinal base 1.
    const std::pair<const char*, u16> expected[] = {
        {"Direct3D8EnableMaximizedWindowedModeShim", 1}, {"ValidatePixelShader", 2},
        {"ValidateVertexShader", 3}, {"DebugSetMute", 4}, {"Direct3DCreate8", 5}};
    for (const auto& [name, ordinal] : expected) {
        ExportEntry* entry = export8(name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->ordinal, ordinal) << name;
    }
    EXPECT_EQ(export8("Direct3DCreate8")->stdcall_bytes, 4u);
}

TEST_F(D3D8Test, VtablesHaveTheSdkMethodCountsAndStdcallSizes) {
    // d3d8.h: IDirect3D8 16 methods, IDirect3DDevice8 97, IDirect3DSurface8 11.
    EXPECT_EQ(d3d8_interface_method_count(D8::Direct3D8), 16u);
    EXPECT_EQ(d3d8_interface_method_count(D8::Device), 97u);
    EXPECT_EQ(d3d8_interface_method_count(D8::Surface), 11u);
    EXPECT_EQ(d3d8_interface_method_count(D8::SwapChain), 5u);
    EXPECT_EQ(d3d8_interface_method_count(D8::Texture), 19u);
    usize count = 0;
    const D3D8MethodInfo* methods = d3d8_interface_methods(D8::Device, count);
    // DrawIndexedPrimitiveUP has 8 arguments, CopyRects 5, Clear 6.
    EXPECT_STREQ(methods[static_cast<usize>(DS::DrawIndexedPrimitiveUP)].name,
                 "DrawIndexedPrimitiveUP");
    EXPECT_EQ(methods[static_cast<usize>(DS::DrawIndexedPrimitiveUP)].stdcall_bytes, 36u);
    EXPECT_EQ(methods[static_cast<usize>(DS::CopyRects)].stdcall_bytes, 24u);
    EXPECT_EQ(methods[static_cast<usize>(DS::Clear)].stdcall_bytes, 28u);

    const u32 d3d = create_factory();
    const u32 vtable = rd(d3d);
    usize factory_count = 0;
    (void)d3d8_interface_methods(D8::Direct3D8, factory_count);
    for (usize slot = 0; slot < factory_count; ++slot) {
        EXPECT_EQ(rd(vtable + 4 * slot), 0x70800000u + 0x10u * slot);
    }
}

// -----------------------------------------------------------------------------
// Direct3DCreate8 / IDirect3D8
// -----------------------------------------------------------------------------

TEST_F(D3D8Test, Direct3DCreate8AcceptsOnlyTheD3d8SdkVersions) {
    // Measured: 120 (DirectX 8.0) and 220 (D3D_SDK_VERSION) succeed.
    EXPECT_EQ(c8::D3D_SDK_VERSION, 220u);
    EXPECT_NE(d3d8_.direct3d_create8(120), 0u);
    EXPECT_NE(d3d8_.direct3d_create8(220), 0u);
    EXPECT_EQ(d3d8_.direct3d_create8(0), 0u);
    EXPECT_EQ(d3d8_.direct3d_create8(32), 0u);
    EXPECT_EQ(d3d8_.direct3d_create8(0x80000000u | 120u), 0u);
}

TEST_F(D3D8Test, QueryInterfaceAnswersOnlyD3d8Identities) {
    const u32 d3d = create_factory();
    write_iid(IID, c8::kIID_IDirect3D8);
    EXPECT_EQ(call8(D8::Direct3D8, 0, {d3d, static_cast<u32>(IID), static_cast<u32>(OUT8)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8), d3d);
    write_iid(IID, c8::d3d9h::kIID_IDirect3D9);
    wr(OUT8, 0xDEADBEEF);
    EXPECT_EQ(call8(D8::Direct3D8, 0, {d3d, static_cast<u32>(IID), static_cast<u32>(OUT8)}),
              0x80004002u);
    EXPECT_EQ(rd(OUT8), 0u);
    // QueryInterface took a reference: AddRef now returns 3, Release 2.
    EXPECT_EQ(call8(D8::Direct3D8, 1, {d3d}), 3u);
    EXPECT_EQ(call8(D8::Direct3D8, 2, {d3d}), 2u);
}

TEST_F(D3D8Test, DeviceCapsAreD3dcaps8WithOnlyD3d8Bits) {
    const u32 d3d = create_factory();
    namespace L = c8::layout_CAPS8;
    std::vector<u8> fill(L::SIZE9 + 16, 0xCC);
    ASSERT_TRUE(memory_.write_bytes(BUF, fill.data(), fill.size()).is_ok());
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::GetDeviceCaps),
                    {d3d, 0, 1, static_cast<u32>(BUF)}),
              D3D_OK);
    // D3DCAPS8 is 212 bytes: the byte after it is untouched.
    EXPECT_EQ(memory_.read_u8(BUF + L::SIZE8).value(), 0xCCu);
    EXPECT_EQ(rd(BUF + L::DeviceType.d3d8), 1u);
    EXPECT_LE(rd(BUF + L::VertexShaderVersion.d3d8), c8::D3DVS_VERSION_1_1);
    EXPECT_LE(rd(BUF + L::PixelShaderVersion.d3d8), c8::D3DPS_VERSION_1_4);
    EXPECT_EQ(rd(BUF + L::Caps2.d3d8) & ~c8::kCaps8Mask_Caps2, 0u);
    EXPECT_EQ(rd(BUF + L::RasterCaps.d3d8) & ~(c8::kCaps8Mask_RasterCaps), 0u);
    EXPECT_EQ(rd(BUF + L::DevCaps.d3d8) & ~c8::kCaps8Mask_DevCaps, 0u);
}

TEST_F(D3D8Test, GetAdapterIdentifierWritesTheD3d8Layout) {
    const u32 d3d = create_factory();
    namespace L = c8::layout_ADAPTER_IDENTIFIER8;
    std::vector<u8> fill(L::SIZE9, 0xCC);
    ASSERT_TRUE(memory_.write_bytes(BUF, fill.data(), fill.size()).is_ok());
    EXPECT_EQ(call8(D8::Direct3D8,
                    static_cast<usize>(c8::Direct3DSlot8::GetAdapterIdentifier),
                    {d3d, 0, c8::D3DENUM_NO_WHQL_LEVEL, static_cast<u32>(BUF)}),
              D3D_OK);
    // 1068 bytes; nothing past it.
    EXPECT_EQ(memory_.read_u8(BUF + L::SIZE8).value(), 0xCCu);
    EXPECT_EQ(L::SIZE8, 1068u);
    EXPECT_NE(memory_.read_u8(BUF + L::Description.d3d8).value(), 0u);
}

TEST_F(D3D8Test, AdapterModesEnumerateAcrossFormats) {
    const u32 d3d = create_factory();
    const u32 count = call8(D8::Direct3D8,
                            static_cast<usize>(c8::Direct3DSlot8::GetAdapterModeCount),
                            {d3d, 0});
    ASSERT_GT(count, 0u);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::EnumAdapterModes),
                    {d3d, 0, count - 1, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::EnumAdapterModes),
                    {d3d, 0, count, static_cast<u32>(BUF)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(call8(D8::Direct3D8,
                    static_cast<usize>(c8::Direct3DSlot8::GetAdapterModeCount),
                    {d3d, 7}),
              0u);
}

// -----------------------------------------------------------------------------
// Device
// -----------------------------------------------------------------------------

TEST_F(D3D8Test, CreateDeviceTranslatesPresentParametersAndPresents) {
    const auto [d3d, device] = create_device8();
    ASSERT_NE(device, 0u);
    // Learn: D3D9 renamed FullScreen_PresentationInterval; a windowed D3D8
    // chain must leave it (and the refresh rate) 0.
    write_pp8(640, 480, c8::D3DSWAPEFFECT_DISCARD, c8::D3DPRESENT_INTERVAL_ONE);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::CreateDevice),
                    {d3d, 0, 1, TEST_HWND, 0x40, static_cast<u32>(PP8),
                     static_cast<u32>(OUT8 + 4)}),
              D3DERR_INVALIDCALL);

    EXPECT_EQ(dev(DS::Clear, {device, 0, 0, 1, 0xFF00FF00u, 0x3F800000u, 0}), D3D_OK);
    EXPECT_EQ(dev(DS::Present, {device, 0, 0, 0, 0}), D3D_OK);
    mgr_.d3d9_cs_flush();
    EXPECT_GE(backend_.record.presents, 1u);

    // GetDirect3D hands back the IDirect3D8 that created the device, with
    // one more reference.
    const u32 before = call8(D8::Direct3D8, 1, {d3d}) - 1;
    (void)call8(D8::Direct3D8, 2, {d3d});
    EXPECT_EQ(dev(DS::GetDirect3D, {device, static_cast<u32>(OUT8 + 8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8 + 8), d3d);
    EXPECT_EQ(call8(D8::Direct3D8, 2, {d3d}), before);
}

TEST_F(D3D8Test, CopyVsyncIsAcceptedAsAWindowedSwapEffect) {
    const u32 d3d = create_factory();
    write_pp8(320, 240, c8::D3DSWAPEFFECT_COPY_VSYNC);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::CreateDevice),
                    {d3d, 0, 1, TEST_HWND, 0x40, static_cast<u32>(PP8),
                     static_cast<u32>(OUT8)}),
              D3D_OK);
    EXPECT_NE(rd(OUT8), 0u);
}

TEST_F(D3D8Test, RemovedRenderStatesKeepTheirValues) {
    const auto [d3d, device] = create_device8();
    for (const u32 state : {10u /*LINEPATTERN*/, 47u /*ZBIAS*/, 40u /*EDGEANTIALIAS*/}) {
        EXPECT_EQ(dev(DS::SetRenderState, {device, state, 5}), D3D_OK) << state;
        EXPECT_EQ(dev(DS::GetRenderState, {device, state, static_cast<u32>(OUT8)}), D3D_OK);
        EXPECT_EQ(rd(OUT8), 5u) << state;
    }
    // PATCHSEGMENTS starts at 1.0f.
    EXPECT_EQ(dev(DS::GetRenderState, {device, 164, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 0x3F800000u);
    // States carried over pass straight through: D3DRS_ZENABLE (7).
    EXPECT_EQ(dev(DS::SetRenderState, {device, 7, 0}), D3D_OK);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 7, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 0u);
    // Measured: Set never fails; Get refuses the retired DX7 states (1 is
    // TEXTUREHANDLE) and anything from 256 on, and returns an undefined
    // state below 256 only once Set gave it a value.
    EXPECT_EQ(dev(DS::SetRenderState, {device, 1, 1}), D3D_OK);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 1, static_cast<u32>(OUT8)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(dev(DS::SetRenderState, {device, 256, 1}), D3D_OK);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 256, static_cast<u32>(OUT8)}),
              D3DERR_INVALIDCALL);
    // Never set: the runtime's fill value 0xBAADCAFE (rs[200]).
    wr(OUT8, 0);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 200, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 0xBAADCAFEu);
    EXPECT_EQ(dev(DS::SetRenderState, {device, 200, 1}), D3D_OK);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 200, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 1u);
    // Point sizes start at 0.0 and D3DCAPS8.MaxPointSize (rs[155], rs[166]).
    namespace L = c8::layout_CAPS8;
    ASSERT_EQ(dev(DS::GetDeviceCaps, {device, static_cast<u32>(BUF)}), D3D_OK);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 155, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 0u);
    EXPECT_EQ(dev(DS::GetRenderState, {device, 166, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), rd(BUF + L::MaxPointSize.d3d8));
}

TEST_F(D3D8Test, SamplerStageStatesMoveToD3d9SamplerState) {
    const auto [d3d, device] = create_device8();
    // D3DTSS_ADDRESSU (13) -> D3DSAMP_ADDRESSU (1); D3DTADDRESS_CLAMP = 3.
    EXPECT_EQ(dev(DS::SetTextureStageState, {device, 2, 13, 3}), D3D_OK);
    const u32 device9 = d3d8_.backing_object(device);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetSamplerState"),
                     {device9, 2, 1, static_cast<u32>(OUT8)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8), 3u);
    EXPECT_EQ(dev(DS::GetTextureStageState, {device, 2, 13, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 4), 3u);
    // D3DTSS_COLOROP (1) stays a stage state. Measured: Set at stage 8 and
    // of undefined types (12, 29) succeeds.
    EXPECT_EQ(dev(DS::SetTextureStageState, {device, 0, 1, 2}), D3D_OK);
    EXPECT_EQ(dev(DS::SetTextureStageState, {device, 8, 1, 2}), D3D_OK);
    EXPECT_EQ(dev(DS::SetTextureStageState, {device, 0, 29, 7}), D3D_OK);
    EXPECT_EQ(dev(DS::GetTextureStageState, {device, 0, 29, static_cast<u32>(OUT8)}), D3D_OK);
    EXPECT_EQ(rd(OUT8), 7u);
}

TEST_F(D3D8Test, SetIndicesKeepsBaseVertexIndexAndStateBlocksRestoreIt) {
    const auto [d3d, device] = create_device8();
    EXPECT_EQ(dev(DS::CreateIndexBuffer,
                  {device, 64, 0, 101 /*INDEX16*/, 1 /*MANAGED*/, static_cast<u32>(OUT8)}),
              D3D_OK);
    const u32 ib = rd(OUT8);
    ASSERT_NE(ib, 0u);
    EXPECT_EQ(dev(DS::SetIndices, {device, ib, 7}), D3D_OK);
    EXPECT_EQ(dev(DS::CreateStateBlock, {device, c8::D3DSBT_ALL, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    const u32 token = rd(OUT8 + 4);
    ASSERT_NE(token, 0u);
    EXPECT_EQ(dev(DS::SetIndices, {device, ib, 3}), D3D_OK);
    EXPECT_EQ(dev(DS::ApplyStateBlock, {device, token}), D3D_OK);
    EXPECT_EQ(dev(DS::GetIndices, {device, static_cast<u32>(OUT8 + 8), static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 8), ib);
    EXPECT_EQ(rd(OUT8 + 12), 7u);
    EXPECT_EQ(dev(DS::DeleteStateBlock, {device, token}), D3D_OK);
    EXPECT_EQ(dev(DS::DeleteStateBlock, {device, token}), D3DERR_INVALIDCALL);
}

// -----------------------------------------------------------------------------
// Vertex shaders
// -----------------------------------------------------------------------------

TEST_F(D3D8Test, DeclarationOnlyShaderBecomesAD3d9DeclarationByRegisterMeaning) {
    const auto [d3d, device] = create_device8();
    // D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, FLOAT3),
    // D3DVSD_REG(D3DVSDE_DIFFUSE, D3DCOLOR), D3DVSD_END().
    const u32 decl[] = {(c8::D3DVSD_TOKEN_STREAM << 29) | 0,
                        (c8::D3DVSD_TOKEN_STREAMDATA << 29) | (c8::D3DVSDT_FLOAT3 << 16) |
                            c8::D3DVSDE_POSITION,
                        (c8::D3DVSD_TOKEN_STREAMDATA << 29) | (c8::D3DVSDT_D3DCOLOR << 16) |
                            c8::D3DVSDE_DIFFUSE,
                        0xFFFFFFFFu};
    for (usize i = 0; i < 4; ++i) wr(DECL + 4 * i, decl[i]);
    EXPECT_EQ(dev(DS::CreateVertexShader, {device, static_cast<u32>(DECL), 0,
                                           static_cast<u32>(OUT8), 0}),
              D3D_OK);
    const u32 handle = rd(OUT8);
    EXPECT_EQ(handle & c8::D3DFVF_RESERVED0, 1u);
    EXPECT_EQ(dev(DS::SetVertexShader, {device, handle}), D3D_OK);
    EXPECT_EQ(dev(DS::GetVertexShader, {device, static_cast<u32>(OUT8 + 4)}), D3D_OK);
    EXPECT_EQ(rd(OUT8 + 4), handle);

    // The D3D9 declaration: {0,0,FLOAT3,DEFAULT,POSITION,0},
    // {0,12,D3DCOLOR,DEFAULT,COLOR,0}, D3DDECL_END.
    const u32 device9 = d3d8_.backing_object(device);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetVertexDeclaration"),
                     {device9, static_cast<u32>(OUT8 + 8)}),
              D3D_OK);
    const u32 decl9 = rd(OUT8 + 8);
    ASSERT_NE(decl9, 0u);
    wr(OUT8 + 12, 3);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexDeclaration, 4,
                     {decl9, static_cast<u32>(BUF), static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 12), 3u);
    EXPECT_EQ(rd(BUF + 0), 0u);
    EXPECT_EQ(rd(BUF + 4), c8::d3d9h::D3DDECLTYPE_FLOAT3 |
                               (c8::d3d9h::D3DDECLUSAGE_POSITION << 16));
    EXPECT_EQ(rd(BUF + 8), 12u << 16);
    EXPECT_EQ(rd(BUF + 12), c8::d3d9h::D3DDECLTYPE_D3DCOLOR |
                                (c8::d3d9h::D3DDECLUSAGE_COLOR << 16));
    EXPECT_EQ(rd(BUF + 16) & 0xFFFFu, 0xFFu);

    // GetVertexShaderDeclaration returns the tokens as given.
    EXPECT_EQ(dev(DS::GetVertexShaderDeclaration,
                  {device, handle, 0, static_cast<u32>(OUT8 + 16)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 16), 16u);

    // An FVF code selects fixed function and reads back as itself.
    EXPECT_EQ(dev(DS::SetVertexShader, {device, 0x42}), D3D_OK);
    EXPECT_EQ(dev(DS::GetVertexShader, {device, static_cast<u32>(OUT8 + 4)}), D3D_OK);
    EXPECT_EQ(rd(OUT8 + 4), 0x42u);
    EXPECT_EQ(dev(DS::SetVertexShader, {device, handle + 2}), D3DERR_INVALIDCALL);
    EXPECT_EQ(dev(DS::DeleteVertexShader, {device, handle}), D3D_OK);
    EXPECT_EQ(dev(DS::DeleteVertexShader, {device, handle}), D3DERR_INVALIDCALL);
    // Measured: deleting an FVF code succeeds; its declaration size is 0.
    EXPECT_EQ(dev(DS::DeleteVertexShader, {device, 0x42}), D3D_OK);
    wr(OUT8 + 16, 0xDEADBEEF);
    EXPECT_EQ(dev(DS::GetVertexShaderDeclaration, {device, 0x42, 0, static_cast<u32>(OUT8 + 16)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(rd(OUT8 + 16), 0u);
    // PATCHSEGMENTS does not tessellate ordinary buffers: the device still
    // validates in one pass.
    EXPECT_EQ(dev(DS::SetRenderState, {device, 164, 0x40000000u}), D3D_OK);
    EXPECT_EQ(dev(DS::ValidateDevice, {device, static_cast<u32>(OUT8 + 20)}), D3D_OK);
    EXPECT_EQ(rd(OUT8 + 20), 1u);
    // Measured: handles count up 3, 5, 7, ...
    EXPECT_EQ(handle, 3u);
}

TEST_F(D3D8Test, Vs11FunctionGainsDclForEachDeclaredRegister) {
    const auto [d3d, device] = create_device8();
    const u32 decl[] = {(c8::D3DVSD_TOKEN_STREAM << 29) | 0,
                        (c8::D3DVSD_TOKEN_STREAMDATA << 29) | (c8::D3DVSDT_FLOAT3 << 16) | 0,
                        (c8::D3DVSD_TOKEN_STREAMDATA << 29) | (c8::D3DVSDT_D3DCOLOR << 16) | 5,
                        // D3DVSD_CONST(4, 1) and its four DWORDs.
                        (c8::D3DVSD_TOKEN_CONSTMEM << 29) | (1u << 25) | 4u,
                        0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u, 0xFFFFFFFFu};
    for (usize i = 0; i < std::size(decl); ++i) wr(DECL + 4 * i, decl[i]);
    auto reg = [](u32 type) { return (type & 7u) << 28; };
    // vs.1.1: m4x4 oPos, v0, c0; mov oD0, v5; end (d3d8types.h tokens).
    const u32 func[] = {c8::D3DVS_VERSION_1_1,
                        20u, 0x80000000u | reg(4) | (0xFu << 16) | 0,
                        0x80000000u | reg(1) | (0xE4u << 16) | 0,
                        0x80000000u | reg(2) | (0xE4u << 16) | 0,
                        1u, 0x80000000u | reg(5) | (0xFu << 16) | 0,
                        0x80000000u | reg(1) | (0xE4u << 16) | 5, 0x0000FFFFu};
    for (usize i = 0; i < std::size(func); ++i) wr(FUNC + 4 * i, func[i]);
    EXPECT_EQ(dev(DS::CreateVertexShader, {device, static_cast<u32>(DECL),
                                           static_cast<u32>(FUNC), static_cast<u32>(OUT8), 0}),
              D3D_OK);
    const u32 handle = rd(OUT8);
    // GetVertexShaderFunction returns the D3D8 tokens unchanged.
    wr(OUT8 + 4, 0x1000);
    EXPECT_EQ(dev(DS::GetVertexShaderFunction,
                  {device, handle, static_cast<u32>(BUF), static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 4), sizeof(func));
    for (usize i = 0; i < std::size(func); ++i) EXPECT_EQ(rd(BUF + 4 * i), func[i]);
    // Measured: too small a buffer is D3DERR_INVALIDCALL and the size is
    // left as the caller passed it.
    wr(OUT8 + 4, 4);
    EXPECT_EQ(dev(DS::GetVertexShaderFunction,
                  {device, handle, static_cast<u32>(BUF), static_cast<u32>(OUT8 + 4)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(rd(OUT8 + 4), 4u);

    // The D3D9 shader carries dcl_position v0 and dcl_color v5 after the
    // version token.
    EXPECT_EQ(dev(DS::SetVertexShader, {device, handle}), D3D_OK);
    const u32 device9 = d3d8_.backing_object(device);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetVertexShader"),
                     {device9, static_cast<u32>(OUT8 + 8)}),
              D3D_OK);
    const u32 vs9 = rd(OUT8 + 8);
    ASSERT_NE(vs9, 0u);
    wr(OUT8 + 12, 0x1000);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexShader, 4,
                     {vs9, static_cast<u32>(BUF), static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF), c8::D3DVS_VERSION_1_1);
    EXPECT_EQ(rd(BUF + 4), c8::d3d9h::D3DSIO_DCL);
    EXPECT_EQ(rd(BUF + 8), 0x80000000u | c8::d3d9h::D3DDECLUSAGE_POSITION);
    EXPECT_EQ(rd(BUF + 12), 0x80000000u | 0x10000000u | 0x000F0000u | 0u);
    EXPECT_EQ(rd(BUF + 20), 0x80000000u | c8::d3d9h::D3DDECLUSAGE_COLOR);
    EXPECT_EQ(rd(BUF + 24), 0x80000000u | 0x10000000u | 0x000F0000u | 5u);
    EXPECT_EQ(rd(BUF + 28), 20u);  // the m4x4 follows

    // D3DVSD_CONST loaded c4 when the shader was set.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetVertexShaderConstantF"),
                     {device9, 4, static_cast<u32>(BUF + 0x200), 1}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + 0x200), 0x3F800000u);
    EXPECT_EQ(rd(BUF + 0x20C), 0x40800000u);
}

// -----------------------------------------------------------------------------
// Resources
// -----------------------------------------------------------------------------

TEST_F(D3D8Test, SurfaceDescriptionsCarryTheD3d8SizeField) {
    const auto [d3d, device] = create_device8();
    EXPECT_EQ(dev(DS::CreateTexture,
                  {device, 64, 32, 0, 0, 21 /*A8R8G8B8*/, 1 /*MANAGED*/,
                   static_cast<u32>(OUT8)}),
              D3D_OK);
    const u32 texture = rd(OUT8);
    ASSERT_NE(texture, 0u);
    // 64x32 with a full chain has 7 levels.
    EXPECT_EQ(call8(D8::Texture, 13, {texture}), 7u);
    namespace L = c8::layout_SURFACE_DESC;
    EXPECT_EQ(call8(D8::Texture, static_cast<usize>(c8::TextureSlot8::GetLevelDesc),
                    {texture, 1, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Format.d3d8), 21u);
    EXPECT_EQ(rd(BUF + L::Type.d3d8), c8::D3DRTYPE_SURFACE);
    EXPECT_EQ(rd(BUF + L::Width.d3d8), 32u);
    EXPECT_EQ(rd(BUF + L::Height.d3d8), 16u);
    EXPECT_EQ(rd(BUF + L::Size.d3d8), 32u * 16u * 4u);
    // Measured: rows are DWORD-aligned, a 1x1 R5G6B5 level is 4 bytes.
    ASSERT_EQ(dev(DS::CreateTexture, {device, 1, 1, 1, 0, c8::fmt8::D3DFMT_R5G6B5, 1,
                                      static_cast<u32>(OUT8 + 16)}),
              D3D_OK);
    EXPECT_EQ(call8(D8::Texture, static_cast<usize>(c8::TextureSlot8::GetLevelDesc),
                    {rd(OUT8 + 16), 0, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Size.d3d8), 4u);

    EXPECT_EQ(call8(D8::Texture, static_cast<usize>(c8::TextureSlot8::GetSurfaceLevel),
                    {texture, 0, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    const u32 surface = rd(OUT8 + 4);
    ASSERT_NE(surface, 0u);
    // GetContainer(IID_IDirect3DTexture8) is the texture.
    write_iid(IID, c8::kIID_IDirect3DTexture8);
    EXPECT_EQ(call8(D8::Surface, static_cast<usize>(c8::SurfaceSlot8::GetContainer),
                    {surface, static_cast<u32>(IID), static_cast<u32>(OUT8 + 8)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 8), texture);
    // Surfaces are not IDirect3DResource8 in D3D8.
    write_iid(IID, c8::kIID_IDirect3DResource8);
    EXPECT_EQ(call8(D8::Surface, 0, {surface, static_cast<u32>(IID), static_cast<u32>(OUT8 + 12)}),
              0x80004002u);
    write_iid(IID, c8::kIID_IDirect3DBaseTexture8);
    EXPECT_EQ(call8(D8::Texture, 0, {texture, static_cast<u32>(IID), static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 12), texture);
}

TEST_F(D3D8Test, CopyRectsMovesPixelsBetweenImageSurfaces) {
    const auto [d3d, device] = create_device8();
    ASSERT_EQ(dev(DS::CreateImageSurface, {device, 8, 8, 21, static_cast<u32>(OUT8)}), D3D_OK);
    ASSERT_EQ(dev(DS::CreateImageSurface, {device, 8, 8, 21, static_cast<u32>(OUT8 + 4)}), D3D_OK);
    const u32 src = rd(OUT8);
    const u32 dst = rd(OUT8 + 4);
    using SS = c8::SurfaceSlot8;
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::LockRect),
                    {src, static_cast<u32>(OUT8 + 8), 0, 0}),
              D3D_OK);
    const u32 pitch = rd(OUT8 + 8);
    const u32 bits = rd(OUT8 + 12);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) wr(bits + y * pitch + 4 * x, (y << 8) | x);
    }
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::UnlockRect), {src}), D3D_OK);
    // Copy src rect (1,2)-(3,4) to point (5,6).
    wr(BUF, 1);
    wr(BUF + 4, 2);
    wr(BUF + 8, 3);
    wr(BUF + 12, 4);
    wr(BUF + 16, 5);
    wr(BUF + 20, 6);
    EXPECT_EQ(dev(DS::CopyRects, {device, src, static_cast<u32>(BUF), 1, dst,
                                  static_cast<u32>(BUF + 16)}),
              D3D_OK);
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::LockRect),
                    {dst, static_cast<u32>(OUT8 + 8), 0, c8::D3DLOCK_READONLY}),
              D3D_OK);
    const u32 dpitch = rd(OUT8 + 8);
    const u32 dbits = rd(OUT8 + 12);
    EXPECT_EQ(rd(dbits + 6 * dpitch + 4 * 5), (2u << 8) | 1u);
    EXPECT_EQ(rd(dbits + 7 * dpitch + 4 * 6), (3u << 8) | 2u);
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::UnlockRect), {dst}), D3D_OK);
    // Different formats are refused.
    ASSERT_EQ(dev(DS::CreateImageSurface, {device, 8, 8, 23 /*R5G6B5*/, static_cast<u32>(OUT8 + 16)}),
              D3D_OK);
    EXPECT_EQ(dev(DS::CopyRects, {device, src, 0, 0, rd(OUT8 + 16), 0}), D3DERR_INVALIDCALL);
}

TEST_F(D3D8Test, ReleasingTheLastReferenceRetiresTheWrapper) {
    const auto [d3d, device] = create_device8();
    ASSERT_EQ(dev(DS::CreateVertexBuffer, {device, 48, 0, 0x42, 1, static_cast<u32>(OUT8)}),
              D3D_OK);
    const u32 vb = rd(OUT8);
    ASSERT_NE(vb, 0u);
    EXPECT_NE(d3d8_.backing_object(vb), 0u);
    EXPECT_EQ(call8(D8::VertexBuffer, 2, {vb}), 0u);
    EXPECT_EQ(d3d8_.backing_object(vb), 0u);
    // A stale pointer is an invalid call, and AddRef/Release return 0.
    EXPECT_EQ(call8(D8::VertexBuffer, 13, {vb, static_cast<u32>(BUF)}), D3DERR_INVALIDCALL);
    EXPECT_EQ(call8(D8::VertexBuffer, 1, {vb}), 0u);
    EXPECT_EQ(dev(DS::Release, {device}), 0u);
    EXPECT_EQ(d3d8_.backing_object(device), 0u);
}

TEST_F(D3D8Test, DrawIndexedPrimitivePassesTheBaseVertexIndex) {
    const auto [d3d, device] = create_device8();
    ASSERT_EQ(dev(DS::CreateVertexBuffer, {device, 16 * 8, 0, 0x42, 1, static_cast<u32>(OUT8)}),
              D3D_OK);
    ASSERT_EQ(dev(DS::CreateIndexBuffer, {device, 6, 0, 101, 1, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    const u32 vb = rd(OUT8);
    const u32 ib = rd(OUT8 + 4);
    ASSERT_EQ(dev(DS::SetVertexShader, {device, 0x42}), D3D_OK);
    ASSERT_EQ(dev(DS::SetStreamSource, {device, 0, vb, 16}), D3D_OK);
    EXPECT_EQ(dev(DS::GetStreamSource, {device, 0, static_cast<u32>(OUT8 + 8),
                                        static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    EXPECT_EQ(rd(OUT8 + 8), vb);
    EXPECT_EQ(rd(OUT8 + 12), 16u);
    ASSERT_EQ(dev(DS::SetIndices, {device, ib, 2}), D3D_OK);
    ASSERT_EQ(dev(DS::BeginScene, {device}), D3D_OK);
    const unsigned before = backend_.record.draws;
    EXPECT_EQ(dev(DS::DrawIndexedPrimitive, {device, 4 /*TRIANGLELIST*/, 0, 3, 0, 1}), D3D_OK);
    ASSERT_EQ(dev(DS::EndScene, {device}), D3D_OK);
    mgr_.d3d9_cs_flush();
    EXPECT_EQ(backend_.record.draws, before + 1);
}

TEST_F(D3D8Test, ZBiasIsUnitsOfTheBoundDepthBuffer) {
    // Measured ("zbias2"): ZBIAS k moves depth k * 2^-24 nearer on D24S8
    // and k * 2^-16 on D16.
    const auto [d3d, device] = create_device8(true);  // D24S8
    const u32 device9 = d3d8_.backing_object(device);
    EXPECT_EQ(dev(DS::SetRenderState, {device, 47, 4}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetRenderState"),
                     {device9, c8::d3d9h::D3DRS_DEPTHBIAS, static_cast<u32>(OUT8)}),
              D3D_OK);
    float bias = 0;
    u32 bits = rd(OUT8);
    std::memcpy(&bias, &bits, sizeof bias);
    EXPECT_FLOAT_EQ(bias, -4.0f / 16777216.0f);
    ASSERT_EQ(dev(DS::CreateDepthStencilSurface,
                  {device, 640, 480, c8::fmt8::D3DFMT_D16, 0, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    EXPECT_EQ(dev(DS::SetRenderTarget, {device, 0, rd(OUT8 + 4)}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, slot9("GetRenderState"),
                     {device9, c8::d3d9h::D3DRS_DEPTHBIAS, static_cast<u32>(OUT8)}),
              D3D_OK);
    bits = rd(OUT8);
    std::memcpy(&bias, &bits, sizeof bias);
    EXPECT_FLOAT_EQ(bias, -4.0f / 65536.0f);
}

TEST_F(D3D8Test, PresentParametersFollowTheD3d8Rules) {
    const u32 d3d = create_factory();
    namespace L = c8::layout_PRESENT_PARAMETERS;
    const auto create = [&]() {
        return call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::CreateDevice),
                     {d3d, 0, 1, TEST_HWND, 0x40, static_cast<u32>(PP8),
                      static_cast<u32>(OUT8)});
    };
    // Measured (create.*): swap effect 5 is invalid.
    write_pp8(64, 64, 5);
    EXPECT_EQ(create(), D3DERR_INVALIDCALL);
    // Four back buffers: refused, and the count is clamped to 3.
    write_pp8(64, 64);
    wr(PP8 + L::BackBufferCount.d3d8, 4);
    EXPECT_EQ(create(), D3DERR_INVALIDCALL);
    EXPECT_EQ(rd(PP8 + L::BackBufferCount.d3d8), 3u);
    // No D3DFMT_UNKNOWN back buffer, even windowed.
    write_pp8(64, 64);
    wr(PP8 + L::BackBufferFormat.d3d8, 0);
    EXPECT_EQ(create(), D3DERR_INVALIDCALL);
    // A zero count and zero size: the count reads back 1, the size 0x0.
    write_pp8(0, 0);
    wr(PP8 + L::BackBufferCount.d3d8, 0);
    EXPECT_EQ(create(), D3D_OK);
    EXPECT_EQ(rd(PP8 + L::BackBufferCount.d3d8), 1u);
    EXPECT_EQ(rd(PP8 + L::BackBufferWidth.d3d8), 0u);
    EXPECT_EQ(rd(PP8 + L::BackBufferHeight.d3d8), 0u);
}

TEST_F(D3D8Test, DeviceReleasesItsFactoryReference) {
    // Measured: IDirect3D8 counts 1 -> 2 while a device lives, back to 1.
    const auto [d3d, device] = create_device8();
    EXPECT_EQ(call8(D8::Direct3D8, 1, {d3d}), 3u);
    EXPECT_EQ(call8(D8::Direct3D8, 2, {d3d}), 2u);
    EXPECT_EQ(dev(DS::Release, {device}), 0u);
    EXPECT_EQ(call8(D8::Direct3D8, 1, {d3d}), 2u);
    EXPECT_EQ(call8(D8::Direct3D8, 2, {d3d}), 1u);
    // A CreateDevice that fails takes no reference (create.* then
    // d3d.refcount.before_create = 2 1).
    write_pp8(640, 480);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::CreateDevice),
                    {d3d, 0, 1, TEST_HWND, 0 /* no vertex processing */,
                     static_cast<u32>(PP8), static_cast<u32>(OUT8)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(call8(D8::Direct3D8, 1, {d3d}), 2u);
    EXPECT_EQ(call8(D8::Direct3D8, 2, {d3d}), 1u);
}

TEST_F(D3D8Test, TextureLevelsShareTheirContainersReferenceCount) {
    // Measured ("surface refcount"): GetSurfaceLevel adds a reference to
    // the texture; a level's AddRef/Release are the texture's; a level held
    // past the texture's last Release keeps it alive.
    const auto [d3d, device] = create_device8();
    ASSERT_EQ(dev(DS::CreateTexture, {device, 64, 64, 0, 0, 21, 1, static_cast<u32>(OUT8)}),
              D3D_OK);
    const u32 t = rd(OUT8);
    using TS = c8::TextureSlot8;
    ASSERT_EQ(call8(D8::Texture, static_cast<usize>(TS::GetSurfaceLevel),
                    {t, 0, static_cast<u32>(OUT8 + 4)}),
              D3D_OK);
    EXPECT_EQ(call8(D8::Texture, 1, {t}), 3u);
    EXPECT_EQ(call8(D8::Texture, 2, {t}), 2u);
    ASSERT_EQ(call8(D8::Texture, static_cast<usize>(TS::GetSurfaceLevel),
                    {t, 0, static_cast<u32>(OUT8 + 8)}),
              D3D_OK);
    const u32 s1 = rd(OUT8 + 4);
    EXPECT_EQ(rd(OUT8 + 8), s1);
    EXPECT_EQ(call8(D8::Surface, 1, {s1}), 4u);
    EXPECT_EQ(call8(D8::Surface, 2, {s1}), 3u);
    EXPECT_EQ(call8(D8::Surface, 2, {s1}), 2u);
    EXPECT_EQ(call8(D8::Surface, 2, {s1}), 1u);
    ASSERT_EQ(call8(D8::Texture, static_cast<usize>(TS::GetSurfaceLevel),
                    {t, 1, static_cast<u32>(OUT8 + 12)}),
              D3D_OK);
    const u32 s3 = rd(OUT8 + 12);
    EXPECT_EQ(call8(D8::Texture, 2, {t}), 1u);
    namespace L = c8::layout_SURFACE_DESC;
    EXPECT_EQ(call8(D8::Surface, static_cast<usize>(c8::SurfaceSlot8::GetDesc),
                    {s3, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Width.d3d8), 32u);
    EXPECT_EQ(call8(D8::Surface, 2, {s3}), 0u);
    EXPECT_EQ(d3d8_.backing_object(t), 0u);
}

TEST_F(D3D8Test, RenderTargetSurfacesReportNoSize) {
    // Measured: bb.Size and ds.Size are 0; an image surface's is its bytes.
    const auto [d3d, device] = create_device8(true);
    ASSERT_EQ(dev(DS::GetBackBuffer, {device, 0, 0, static_cast<u32>(OUT8)}), D3D_OK);
    namespace L = c8::layout_SURFACE_DESC;
    using SS = c8::SurfaceSlot8;
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::GetDesc), {rd(OUT8), static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Usage.d3d8), c8::D3DUSAGE_RENDERTARGET);
    EXPECT_EQ(rd(BUF + L::Pool.d3d8), c8::D3DPOOL_DEFAULT);
    EXPECT_EQ(rd(BUF + L::Size.d3d8), 0u);
    ASSERT_EQ(dev(DS::GetDepthStencilSurface, {device, static_cast<u32>(OUT8 + 4)}), D3D_OK);
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::GetDesc),
                    {rd(OUT8 + 4), static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Usage.d3d8), c8::D3DUSAGE_DEPTHSTENCIL);
    EXPECT_EQ(rd(BUF + L::Size.d3d8), 0u);
    ASSERT_EQ(dev(DS::CreateImageSurface, {device, 32, 32, 21, static_cast<u32>(OUT8 + 8)}), D3D_OK);
    ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::GetDesc),
                    {rd(OUT8 + 8), static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::Pool.d3d8), c8::D3DPOOL_SYSTEMMEM);
    EXPECT_EQ(rd(BUF + L::Size.d3d8), 0x1000u);
}

TEST_F(D3D8Test, MiscellaneousMeasuredResults) {
    const auto [d3d, device] = create_device8();
    // GetInfo: ids 4..7 S_FALSE, id 1 E_FAIL.
    for (const u32 id : {4u, 5u, 6u, 7u}) {
        EXPECT_EQ(dev(DS::GetInfo, {device, id, static_cast<u32>(BUF), 4}), 1u) << id;
    }
    EXPECT_EQ(dev(DS::GetInfo, {device, 1, static_cast<u32>(BUF), 4}), 0x80004005u);
    EXPECT_EQ(dev(DS::ApplyStateBlock, {device, 0}), D3D_OK);
    using F = c8::Direct3DSlot8;
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::RegisterSoftwareDevice), {d3d, 0}),
              D3DERR_INVALIDCALL);
    // CheckDeviceType: UNKNOWN display format INVALIDCALL, A8R8G8B8
    // display NOTAVAILABLE, X8R8G8B8 fine.
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::CheckDeviceType), {d3d, 0, 1, 0, 22, 1}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::CheckDeviceType), {d3d, 0, 1, 21, 21, 1}),
              0x8876086Au);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::CheckDeviceType), {d3d, 0, 1, 22, 22, 1}),
              D3D_OK);
}

TEST_F(D3D8Test, LowColorModesFollowTheManifest) {
    // Learn (EnumDisplaySettings): modes below 32 bpp exist, emulated, only
    // for an image that does not target Windows 8 or later; Learn
    // (EnumAdapterModes): 565/555 enumerate as one set in the adapter's
    // format, alpha variants as their non-alpha one with the supplied Format.
    const u32 d9 = mgr_.direct3d_create9(D3D_SDK_VERSION);
    const auto count9 = [&](u32 format) { return invoke_method(6, {d9, 0, format}); };
    const u32 modes32 = count9(c8::fmt8::D3DFMT_X8R8G8B8);
    ASSERT_GT(modes32, 0u);
    EXPECT_EQ(count9(c8::fmt8::D3DFMT_A8R8G8B8), modes32);
    EXPECT_EQ(invoke_method(7, {d9, 0, c8::fmt8::D3DFMT_A8R8G8B8, 0, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + 12), c8::fmt8::D3DFMT_A8R8G8B8);
    EXPECT_EQ(count9(c8::fmt8::D3DFMT_R5G6B5), 0u);  // manifested-for-8 default

    const u32 d3d = create_factory();
    const u32 d8_32 = call8(D8::Direct3D8,
                            static_cast<usize>(c8::Direct3DSlot8::GetAdapterModeCount), {d3d, 0});
    EXPECT_EQ(d8_32, modes32);

    mgr_.set_low_color_modes_emulated(true);
    EXPECT_EQ(count9(c8::fmt8::D3DFMT_R5G6B5), modes32);
    EXPECT_EQ(count9(c8::fmt8::D3DFMT_X1R5G5B5), modes32);
    EXPECT_EQ(invoke_method(7, {d9, 0, c8::fmt8::D3DFMT_X1R5G5B5, 1, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + 12), c8::fmt8::D3DFMT_R5G6B5);
    // D3D8 lists both bit depths; the 16-bit 800x600 a 2001 title falls
    // back to is there.
    const u32 d8_all = call8(D8::Direct3D8,
                             static_cast<usize>(c8::Direct3DSlot8::GetAdapterModeCount), {d3d, 0});
    EXPECT_EQ(d8_all, 2 * modes32);
    bool found = false;
    for (u32 i = 0; i < d8_all; ++i) {
        ASSERT_EQ(call8(D8::Direct3D8, static_cast<usize>(c8::Direct3DSlot8::EnumAdapterModes),
                        {d3d, 0, i, static_cast<u32>(BUF)}),
                  D3D_OK);
        found |= rd(BUF) == 800 && rd(BUF + 4) == 600 && rd(BUF + 12) == c8::fmt8::D3DFMT_R5G6B5;
    }
    EXPECT_TRUE(found);
}

TEST_F(D3D8Test, LockableRenderTargetShowsAndKeepsItsImage) {
    // Learn (CreateRenderTarget Lockable, D3DPRESENTFLAG_LOCKABLE_BACKBUFFER):
    // a lockable render target can be locked. What a Lock exposes is the
    // rendered image, and what is written through it becomes the image.
    // (The test backend's swap chain has no colour texture, so the render
    // target stands in for the back buffer; both take the same path.)
    for (const u32 format : {c8::fmt8::D3DFMT_X8R8G8B8, c8::fmt8::D3DFMT_R5G6B5}) {
        const auto [d3d, device] = create_device8();
        ASSERT_EQ(dev(DS::CreateRenderTarget,
                      {device, 64, 32, format, 0, 1 /* Lockable */, static_cast<u32>(OUT8 + 4)}),
                  D3D_OK)
            << format;
        const u32 bb = rd(OUT8 + 4);
        ASSERT_EQ(dev(DS::SetRenderTarget, {device, bb, 0}), D3D_OK);
        // Opaque pure red, representable exactly in both formats.
        ASSERT_EQ(dev(DS::Clear, {device, 0, 0, 1, 0xFFFF0000u, 0, 0}), D3D_OK);
        using SS = c8::SurfaceSlot8;
        ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::LockRect),
                        {bb, static_cast<u32>(OUT8 + 8), 0, 0}),
                  D3D_OK);
        const u32 pitch = rd(OUT8 + 8);
        const u32 bits = rd(OUT8 + 12);
        const bool wide = format == c8::fmt8::D3DFMT_X8R8G8B8;
        const auto pixel = [&](u32 x, u32 y) {
            const GuestAddr a = bits + y * pitch + x * (wide ? 4u : 2u);
            return wide ? (rd(a) & 0x00FFFFFFu) : (rd(a) & 0xFFFFu);
        };
        const u32 red = wide ? 0x00FF0000u : 0xF800u;
        const u32 blue = wide ? 0x000000FFu : 0x001Fu;
        EXPECT_EQ(pixel(5, 7), red) << format;
        // Write a blue pixel; it must survive the GPU round trip.
        const GuestAddr a = bits + 3 * pitch + 2 * (wide ? 4u : 2u);
        if (wide) {
            wr(a, 0xFF000000u | blue);
        } else {
            ASSERT_TRUE(memory_.write_u16(a, static_cast<u16>(blue)).is_ok());
        }
        ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::UnlockRect), {bb}), D3D_OK);
        // Scrub the guest copy: the relock must re-read the GPU image.
        std::vector<u8> scrub(static_cast<usize>(pitch) * 32, 0);
        ASSERT_TRUE(memory_.write_bytes(bits, scrub.data(), scrub.size()).is_ok());
        ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::LockRect),
                        {bb, static_cast<u32>(OUT8 + 8), 0, c8::D3DLOCK_READONLY}),
                  D3D_OK);
        EXPECT_EQ(pixel(2, 3), blue) << format;
        EXPECT_EQ(pixel(5, 7), red) << format;
        ASSERT_EQ(call8(D8::Surface, static_cast<usize>(SS::UnlockRect), {bb}), D3D_OK);
    }
}

TEST_F(D3D8Test, ReferenceDeviceIsAvailable) {
    // Measured (d3d8_ref_device_probe): Windows 11's d3d8.dll answers
    // GetDeviceCaps, CheckDeviceType and CreateDevice for D3DDEVTYPE_REF.
    const u32 d3d = create_factory();
    using F = c8::Direct3DSlot8;
    namespace L = c8::layout_CAPS8;
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::GetDeviceCaps),
                    {d3d, 0, c8::D3DDEVTYPE_REF, static_cast<u32>(BUF)}),
              D3D_OK);
    EXPECT_EQ(rd(BUF + L::DeviceType.d3d8), c8::D3DDEVTYPE_REF);
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::CheckDeviceType),
                    {d3d, 0, c8::D3DDEVTYPE_REF, 22, 22, 1}),
              D3D_OK);
    write_pp8(64, 64);
    ASSERT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::CreateDevice),
                    {d3d, 0, c8::D3DDEVTYPE_REF, TEST_HWND, 0x20, static_cast<u32>(PP8),
                     static_cast<u32>(OUT8)}),
              D3D_OK);
    const u32 device = rd(OUT8);
    ASSERT_EQ(dev(DS::GetCreationParameters, {device, static_cast<u32>(BUF)}), D3D_OK);
    EXPECT_EQ(rd(BUF + c8::layout_DEVICE_CREATION_PARAMETERS::DeviceType.d3d8),
              c8::D3DDEVTYPE_REF);
    ASSERT_EQ(dev(DS::GetDeviceCaps, {device, static_cast<u32>(BUF)}), D3D_OK);
    EXPECT_EQ(rd(BUF + L::DeviceType.d3d8), c8::D3DDEVTYPE_REF);
    // D3DDEVTYPE_SW stays unavailable (d3d8_contract_probe: caps.sw).
    EXPECT_EQ(call8(D8::Direct3D8, static_cast<usize>(F::GetDeviceCaps),
                    {d3d, 0, c8::D3DDEVTYPE_SW, static_cast<u32>(BUF)}),
              D3DERR_INVALIDCALL);
}

}  // namespace

namespace {
// d3d8_device_cursor_probe32.winoracle-gpu.expected: without a device cursor,
// ShowCursor leaves the Win32 cursor alone and answers FALSE (even right
// after TRUE); after SetCursorProperties it drives the cursor and answers the
// previous visibility.
TEST_F(D3D8Test, ShowCursorDrivesOnlyACursorSetBySetCursorProperties) {
    auto [d3d, device] = create_device8();
    std::vector<bool> host;
    mgr_.set_cursor_visible_callback([&](bool visible) { host.push_back(visible); });
    EXPECT_EQ(dev(DS::ShowCursor, {device, 0}), 0u);  // noprops.show_false prev=0
    EXPECT_EQ(dev(DS::ShowCursor, {device, 1}), 0u);  // noprops.show_true prev=0
    EXPECT_EQ(dev(DS::ShowCursor, {device, 0}), 0u);  // noprops.show_false2 prev=0
    EXPECT_TRUE(host.empty());                        // showing=1 handle=initial throughout

    ASSERT_EQ(dev(DS::CreateImageSurface, {device, 32, 32, 21 /*A8R8G8B8*/,
                                           static_cast<u32>(OUT8 + 8)}), D3D_OK);
    const u32 surface = rd(OUT8 + 8);
    ASSERT_EQ(dev(DS::SetCursorProperties, {device, 1, 1, surface}), D3D_OK);
    EXPECT_TRUE(host.empty());                        // props.after_set: unchanged
    EXPECT_EQ(dev(DS::ShowCursor, {device, 1}), 0u);  // props.show_true prev=0
    EXPECT_EQ(dev(DS::ShowCursor, {device, 0}), 1u);  // props.show_false prev=1
    EXPECT_EQ(host, (std::vector<bool>{true, false}));
    mgr_.set_cursor_visible_callback({});
    (void)call8(D8::Surface, 2, {surface});
    (void)dev(DS::Release, {device});
    (void)call8(D8::Direct3D8, 2, {d3d});
}
}  // namespace
