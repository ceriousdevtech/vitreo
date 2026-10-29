/// @file test_d3d9_factory.cpp
/// @brief IDirect3D9 factory, adapter identity, caps and format-query tests.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"
#include <winapi/common/display_device_identity.hpp>

#include <array>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// IDirect3D9 Tests
// =============================================================================

TEST_F(D3D9ManagerTest, Direct3DCreate9ReturnsMappedGuestComObject) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    const u32 vtable = memory_.read_u32(object).value();
    ASSERT_NE(vtable, 0u);
    for (usize slot = 0; slot < D3D9_METHOD_COUNT; ++slot) {
        EXPECT_EQ(
            memory_.read_u32(vtable + slot * sizeof(u32)).value(),
            method_thunks_[slot]);
    }
}

TEST_F(D3D9ManagerTest, Direct3DCreate9RejectsSdkMismatch) {
    EXPECT_EQ(mgr_.direct3d_create9(D3D_SDK_VERSION + 1), 0u);
}

TEST_F(D3D9ManagerTest, Direct3DCreate9UsesX64VtableAndQueryInterfacePointers) {
    D3D9Manager x64_manager;
    x64_manager.set_memory(&memory_, true);
    x64_manager.set_backend(&backend_);
    auto thunks = method_thunks_;
    for (auto& thunk : thunks) thunk += 0x140000000;
    x64_manager.set_method_thunks(thunks);
    const auto object = x64_manager.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    const auto vtable = memory_.read_u64(object).value();
    for (usize slot = 0; slot < thunks.size(); ++slot) {
        EXPECT_EQ(memory_.read_u64(vtable + slot * 8).value(), thunks[slot]);
    }
    constexpr GuestAddr high = 0x140000000;
    ASSERT_TRUE(memory_.map_region(high, 0x1000, MemoryProtection::ReadWrite).is_ok());
    constexpr std::array<u8, 16> iid{
        0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
        0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};
    ASSERT_TRUE(memory_.write_bytes(high, iid.data(), iid.size()).is_ok());
    ASSERT_TRUE(memory_.write_u64(high + 0x100, UINT64_MAX).is_ok());
    cpu_.init_long_mode();
    cpu_.gpr.rcx = object;
    cpu_.gpr.rdx = high;
    cpu_.gpr.r8 = high + 0x100;
    x64_manager.impl_method(marshaler_, D3D9InterfaceId::Direct3D9, 0);
    EXPECT_EQ(cpu_.gpr.rax, D3D_OK);
    EXPECT_EQ(memory_.read_u64(high + 0x100).value(), object);
}

TEST_F(D3D9ManagerTest, QueryInterfacePreservesIdentityAndReferenceCount) {
    constexpr std::array<u8, 16> iid_idirect3d9{
        0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
        0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};
    ASSERT_TRUE(memory_.write_bytes(
        IID_ADDR, iid_idirect3d9.data(), iid_idirect3d9.size()).is_ok());
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    const u32 vtable = memory_.read_u32(object).value();

    EXPECT_EQ(
        invoke_method(0, {object, static_cast<u32>(IID_ADDR),
                          static_cast<u32>(DEVICE_OUT_ADDR)}),
        D3D_OK);
    EXPECT_EQ(memory_.read_u32(DEVICE_OUT_ADDR).value(), object);
    EXPECT_EQ(invoke_method(2, {object}), 1u);
    EXPECT_EQ(invoke_method(2, {object}), 0u);
    // Keep the thunk table reachable so a stale Release fails through the
    // object registry instead of faulting at guest address 8.
    EXPECT_EQ(memory_.read_u32(object).value(), vtable);
}

TEST_F(D3D9ManagerTest, GetAdapterCountReturnsOne) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(invoke_method(4, {object}), 1u);
}

TEST_F(D3D9ManagerTest, GetAdapterIdentifierFillsCompleteStructure) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(
        invoke_method(5, {object, 0, 0, static_cast<u32>(IDENT_ADDR)}),
        D3D_OK);

    char driver[13] = {};
    ASSERT_TRUE(memory_.read_bytes(
        IDENT_ADDR, reinterpret_cast<Byte*>(driver), 12).is_ok());
    EXPECT_STREQ(driver, "vitreo-metal");
    char device_name[13] = {};
    ASSERT_TRUE(memory_.read_bytes(
        IDENT_ADDR + 1024, reinterpret_cast<Byte*>(device_name), 12).is_ok());
    EXPECT_STREQ(device_name, "\\\\.\\DISPLAY1");
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1064).value(), 0u);
}

// -----------------------------------------------------------------------------
// Advertised adapter identity (per-capsule configuration)
// -----------------------------------------------------------------------------

namespace {

/// Installs an advertised identity for one test and restores the previous one,
/// so identity tests cannot leak into the rest of the suite.
class ScopedAdapterIdentity {
public:
    explicit ScopedAdapterIdentity(
        display_device_identity::AdapterIdentity identity)
        : saved_(display_device_identity::adapter_identity()) {
        ::unsetenv("VITREO_D3D9_ADAPTER_ID");
        ::unsetenv("VITREO_D3D9_ADAPTER_DESC");
        display_device_identity::set_adapter_identity(std::move(identity));
    }
    ScopedAdapterIdentity(const ScopedAdapterIdentity&) = delete;
    ScopedAdapterIdentity& operator=(const ScopedAdapterIdentity&) = delete;
    ~ScopedAdapterIdentity() {
        display_device_identity::set_adapter_identity(saved_);
    }

private:
    display_device_identity::AdapterIdentity saved_;
};

[[nodiscard]] std::string read_c_string(const GuestMemory& memory,
                                        GuestAddr address, usize capacity) {
    std::vector<char> bytes(capacity + 1, '\0');
    EXPECT_TRUE(memory.read_bytes(address,
                                  reinterpret_cast<Byte*>(bytes.data()),
                                  capacity).is_ok());
    return std::string(bytes.data());
}

} // namespace

// An unconfigured capsule must report exactly what it reported before the
// identity knob existed: no PCI identity, no driver version, and the fixed
// description that is deliberately not the host GPU name.
TEST_F(D3D9ManagerTest, AdapterIdentityDefaultIsUnchanged) {
    const ScopedAdapterIdentity scoped{{}};
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    ASSERT_EQ(invoke_method(5, {object, 0, 0, static_cast<u32>(IDENT_ADDR)}),
              D3D_OK);

    EXPECT_EQ(read_c_string(memory_, IDENT_ADDR + 512, 512),
              "Vitreo Display Adapter");
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1056).value(), 0u); // liDriverVersion
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1060).value(), 0u);
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1064).value(), 0u); // VendorId
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1068).value(), 0u); // DeviceId
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1072).value(), 0u); // SubSysId
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1076).value(), 0u); // Revision
}

// A configured capsule sees the whole tuple, not just the vendor: a guest that
// reads VendorId but not DeviceId must not be able to observe a half-applied
// identity.
TEST_F(D3D9ManagerTest, ConfiguredAdapterIdentityIsReportedInFull) {
    display_device_identity::AdapterIdentity identity;
    identity.vendor_id = 0x8086u;
    identity.device_id = 0x7D55u;
    identity.subsystem_id = 0x2212103Cu;
    identity.revision = 0x08u;
    identity.description = "Configured Test Adapter";
    identity.driver_version =
        display_device_identity::DEFAULT_DRIVER_VERSION;
    const ScopedAdapterIdentity scoped{identity};

    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    ASSERT_EQ(invoke_method(5, {object, 0, 0, static_cast<u32>(IDENT_ADDR)}),
              D3D_OK);

    EXPECT_EQ(read_c_string(memory_, IDENT_ADDR + 512, 512),
              "Configured Test Adapter");
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1064).value(), 0x8086u);
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1068).value(), 0x7D55u);
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1072).value(), 0x2212103Cu);
    EXPECT_EQ(memory_.read_u32(IDENT_ADDR + 1076).value(), 0x08u);
    const u64 version =
        (static_cast<u64>(memory_.read_u32(IDENT_ADDR + 1060).value()) << 32) |
        memory_.read_u32(IDENT_ADDR + 1056).value();
    EXPECT_EQ(version, display_device_identity::DEFAULT_DRIVER_VERSION);
}

TEST(D3D9AdapterIdentityFormat, DriverVersionRoundTrips) {
    namespace identity = display_device_identity;
    const auto packed = identity::parse_driver_version("32.0.101.5972");
    ASSERT_TRUE(packed.has_value());
    EXPECT_EQ(*packed, identity::DEFAULT_DRIVER_VERSION);
    EXPECT_EQ(identity::format_driver_version(*packed), "32.0.101.5972");
    EXPECT_FALSE(identity::parse_driver_version("32.0.101").has_value());
    EXPECT_FALSE(identity::parse_driver_version("").has_value());
}

TEST_F(D3D9ManagerTest, GetDeviceCapsReportsAdapterGroupIdentity) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);

    // Poison the buffer so every asserted field must be written by the call
    // rather than inherited from zeroed guest memory.
    for (usize offset = 0; offset < 304; offset += sizeof(u32)) {
        ASSERT_TRUE(memory_.write_u32(CAPS_ADDR + offset, 0xDEADBEEF).is_ok());
    }

    constexpr u32 D3DDEVTYPE_HAL = 1;
    EXPECT_EQ(
        invoke_method(14, {object, 0, D3DDEVTYPE_HAL,
                           static_cast<u32>(CAPS_ADDR)}),
        D3D_OK);

    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_DEVICE_TYPE).value(),
              D3DDEVTYPE_HAL);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_ADAPTER_ORDINAL).value(), 0u);

    // An adapter always belongs to a group containing at least itself, and it
    // is its own master while Vitreo reports a single adapter.
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_MASTER_ADAPTER_ORDINAL).value(),
              0u);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR +
                  D3DCAPS9_OFFSET_ADAPTER_ORDINAL_IN_GROUP).value(), 0u);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR +
                  D3DCAPS9_OFFSET_NUMBER_OF_ADAPTERS_IN_GROUP).value(), 1u);

    // Min-spec fields games actually probe (texture size, shader models).
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_MAX_TEXTURE_WIDTH).value(),
              8192u);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_MAX_TEXTURE_HEIGHT).value(),
              8192u);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_VERTEX_SHADER_VERSION).value(),
              0xFFFE0300u);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_PIXEL_SHADER_VERSION).value(),
              0xFFFF0300u);
    const u32 texture_ops = memory_.read_u32(
        CAPS_ADDR + D3DCAPS9_OFFSET_TEXTURE_OP_CAPS).value();
    EXPECT_NE(texture_ops & 0x00200000u, 0u);  // BUMPENVMAP
    EXPECT_NE(texture_ops & 0x00400000u, 0u);  // BUMPENVMAPLUMINANCE
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_NUM_SIMULTANEOUS_RTS).value(),
              4u);

    // Shader capability structures must be filled (not leftover poison / not
    // accidentally overwritten by a miscounted adapter-group write).
    EXPECT_NE(memory_.read_u32(CAPS_ADDR + D3DCAPS9_OFFSET_VS20_CAPS).value(),
              0xDEADBEEFu);
    EXPECT_NE(memory_.read_u32(CAPS_ADDR + D3DCAPS9_OFFSET_PS20_CAPS).value(),
              0xDEADBEEFu);
    EXPECT_EQ(memory_.read_u32(
                  CAPS_ADDR + D3DCAPS9_OFFSET_PS20_CAPS + 16).value(),
              512u);
}

TEST_F(D3D9ManagerTest, GetDeviceCapsAdvertisesFullscreenGamma) {
    // Learn: apps probe Caps2 for D3DCAPS2_FULLSCREENGAMMA before expecting
    // SetGammaRamp to affect the display. Present applies the LUT; host has
    // no calibrator so CANCALIBRATEGAMMA stays clear.
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    constexpr u32 D3DDEVTYPE_HAL = 1;
    constexpr u32 D3DCAPS2_FULLSCREENGAMMA = 0x00020000u;
    constexpr u32 D3DCAPS2_CANCALIBRATEGAMMA = 0x00100000u;

    EXPECT_EQ(
        invoke_method(14, {object, 0, D3DDEVTYPE_HAL,
                           static_cast<u32>(CAPS_ADDR)}),
        D3D_OK);
    const u32 caps2 =
        memory_.read_u32(CAPS_ADDR + D3DCAPS9_OFFSET_CAPS2).value();
    EXPECT_EQ(caps2 & D3DCAPS2_FULLSCREENGAMMA, D3DCAPS2_FULLSCREENGAMMA);
    EXPECT_EQ(caps2 & D3DCAPS2_CANCALIBRATEGAMMA, 0u);
}

TEST_F(D3D9ManagerTest, GetDeviceCapsRejectsNullBufferAndBadDeviceType) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    constexpr u32 D3DDEVTYPE_HAL = 1;
    constexpr u32 D3DDEVTYPE_REF = 2;
    EXPECT_EQ(invoke_method(14, {object, 0, D3DDEVTYPE_HAL, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(
        invoke_method(14, {object, 0, D3DDEVTYPE_REF,
                           static_cast<u32>(CAPS_ADDR)}),
        D3DERR_INVALIDCALL);
    EXPECT_EQ(
        invoke_method(14, {object, 1, D3DDEVTYPE_HAL,
                           static_cast<u32>(CAPS_ADDR)}),
        D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, GetAdapterDisplayModeUsesDisplayResolver) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(
        invoke_method(8, {object, 0, static_cast<u32>(MODE_ADDR)}),
        D3D_OK);

    u32 width = memory_.read_u32(MODE_ADDR + 0).value();
    u32 height = memory_.read_u32(MODE_ADDR + 4).value();
    u32 refresh = memory_.read_u32(MODE_ADDR + 8).value();

    EXPECT_EQ(width, 1920u);
    EXPECT_EQ(height, 1080u);
    EXPECT_EQ(refresh, 0u);
}

TEST_F(D3D9ManagerTest, GetAdapterModeCountEnumeratesClassicDesktopSizes) {
    constexpr u32 D3DFMT_X8R8G8B8 = 22;
    constexpr u32 D3DFMT_R5G6B5 = 23;
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);

    // Only the primary X8R8G8B8 adapter format reports modes; other formats
    // stay at zero so callers probing A8R8G8B8 / 16-bit fall through cleanly.
    EXPECT_EQ(invoke_method(6, {object, 0, D3DFMT_R5G6B5}), 0u);
    EXPECT_EQ(invoke_method(6, {object, 1, D3DFMT_X8R8G8B8}), 0u);

    const u32 count = invoke_method(6, {object, 0, D3DFMT_X8R8G8B8});
    // Desktop 1920x1080 plus every catalogued size that fits on it.
    EXPECT_GE(count, 10u);
    EXPECT_LT(count, 32u);

    bool saw_800x600 = false;
    bool saw_1024x768 = false;
    bool saw_1280x960 = false;
    bool saw_1600x1200 = false;
    bool saw_desktop = false;
    for (u32 i = 0; i < count; ++i) {
        EXPECT_EQ(
            invoke_method(7, {object, 0, D3DFMT_X8R8G8B8, i,
                              static_cast<u32>(MODE_ADDR)}),
            D3D_OK);
        const u32 width = memory_.read_u32(MODE_ADDR + 0).value();
        const u32 height = memory_.read_u32(MODE_ADDR + 4).value();
        const u32 format = memory_.read_u32(MODE_ADDR + 12).value();
        EXPECT_EQ(format, D3DFMT_X8R8G8B8);
        EXPECT_LE(width, 1920u);
        EXPECT_LE(height, 1080u);
        if (i == 0) {
            EXPECT_EQ(width, 1920u);
            EXPECT_EQ(height, 1080u);
            saw_desktop = true;
        }
        if (width == 800 && height == 600) saw_800x600 = true;
        if (width == 1024 && height == 768) saw_1024x768 = true;
        if (width == 1280 && height == 960) saw_1280x960 = true;
        if (width == 1600 && height == 1200) saw_1600x1200 = true;
    }
    EXPECT_TRUE(saw_desktop);
    EXPECT_TRUE(saw_800x600);
    EXPECT_TRUE(saw_1024x768);
    EXPECT_TRUE(saw_1280x960);
    // 1600x1200 does not fit under a 1080-tall desktop.
    EXPECT_FALSE(saw_1600x1200);

    EXPECT_EQ(
        invoke_method(7, {object, 0, D3DFMT_X8R8G8B8, count,
                          static_cast<u32>(MODE_ADDR)}),
        D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, CheckDeviceTypeReturnsOk) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(invoke_method(9, {object, 0, 1, 22, 22, 1}), D3D_OK);
}

TEST_F(D3D9ManagerTest, CheckDeviceFormatReturnsOk) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22, 0, 1, 22}), D3D_OK);
}

TEST_F(D3D9ManagerTest,
       CheckDeviceFormatAdvertisesWiredVertexTextureSampling) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    constexpr u32 D3DUSAGE_QUERY_VERTEXTEXTURE = 0x00100000u;
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_VERTEXTEXTURE,
                                 D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8}),
              D3D_OK);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_VERTEXTEXTURE,
                                 1 /* D3DRTYPE_SURFACE */, D3DFMT_A8R8G8B8}),
              D3DERR_NOTAVAILABLE);
}
