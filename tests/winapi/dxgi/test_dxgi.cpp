// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"
#include "winapi/graphics/d3d10/d3d10.hpp"

#include <cstring>

namespace vitreo::winapi::dxgi_test {
namespace {

// Every IDXGIFactory2 method Vitreo declines must still write its COM
// out-parameter.
//
// This is what the dxgi.1 audit actually turned up. All three real swap-chain
// paths -- CreateSwapChain, CreateSwapChainForHwnd, CreateSwapChainForComposition
// -- have creators registered and wired, validate the factory, and null their
// out-parameter on every failure. The one swap-chain path with no creator is
// CreateSwapChainForCoreWindow, correctly, because a CoreWindow is a WinRT
// object a Win32 guest does not have. But it shared a stub helper that returned
// DXGI_ERROR_INVALID_CALL and wrote nothing, and so did four more Factory2
// methods with out-parameters.
//
// Leaving one untouched hands the guest whatever was on its stack: an
// IDXGISwapChain1* it may Release, or an occlusion-status cookie it may later
// pass to UnregisterOcclusionStatus. The test drives the registered exports
// rather than the helper directly, because the per-method argument index is the
// part that can be wrong.
// Measured on Windows 11 (dxgi_a11_contracts_probe64.expected
// [factory.methods]): a CoreWindow chain is refused with *ppSwapChain
// untouched; the stereo / occlusion registrations succeed with a cookie, and
// a NULL window, zero message or NULL event is INVALID_CALL with the cookie
// untouched. This test used to require every out-parameter to be zeroed and
// every call to fail.
TEST_F(DxgiManagerTest, Factory2StatusRegistrationsAndCoreWindowAsOnWindows) {
    constexpr GuestAddr OUT = DATA_BASE + 0xA00;
    const auto exports = create_dxgi_thunks(manager);
    const auto handler_for = [&exports](const char* name) {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) {
                return winapi::ApiHandler{
                    [&entry](Marshaler& m) { call_export(entry, m); }};
            }
        }
        return winapi::ApiHandler{};
    };
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    // x64 fastcall: RCX, RDX, R8, R9, then the stack at RSP+0x28.
    const auto call = [&](const char* name, u64 a1, u64 a2, u64 a3,
                          u64 a4 = 0, u64 a5 = 0) {
        const auto handler = handler_for(name);
        EXPECT_TRUE(static_cast<bool>(handler)) << name;
        cpu.gpr.rsp = STACK_BASE;
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = a1;
        cpu.gpr.r8 = a2;
        cpu.gpr.r9 = a3;
        EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, a4).is_ok());
        EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, a5).is_ok());
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    constexpr u64 kPoison = 0xA5A5A5A5A5A5A5A5ull;
    ASSERT_TRUE(memory.write_u64(OUT, kPoison).is_ok());
    EXPECT_EQ(call("_DXGI_CreateSwapChainForCoreWindow", 0x1000, 0x2000,
                   0x3000, 0, OUT),
              dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u64(OUT).value(), kPoison);

    constexpr u64 kWindow = 0x10010;
    for (const char* name : {"_DXGI_RegisterStereoStatusWindow",
                             "_DXGI_RegisterOcclusionStatusWindow"}) {
        ASSERT_TRUE(memory.write_u32(OUT, 0xA5A5A5A5u).is_ok());
        EXPECT_EQ(call(name, 0, 0x401, OUT), dxgi::ERROR_INVALID_CALL);
        EXPECT_EQ(memory.read_u32(OUT).value(), 0xA5A5A5A5u) << name;
        EXPECT_EQ(call(name, kWindow, 0, OUT), dxgi::ERROR_INVALID_CALL);
        EXPECT_EQ(memory.read_u32(OUT).value(), 0xA5A5A5A5u) << name;
        EXPECT_EQ(call(name, kWindow, 0x401, OUT), dxgi::S_OK) << name;
        EXPECT_NE(memory.read_u32(OUT).value(), 0xA5A5A5A5u) << name;
    }
    for (const char* name : {"_DXGI_RegisterStereoStatusEvent",
                             "_DXGI_RegisterOcclusionStatusEvent"}) {
        ASSERT_TRUE(memory.write_u32(OUT, 0xA5A5A5A5u).is_ok());
        EXPECT_EQ(call(name, 0, OUT, 0), dxgi::ERROR_INVALID_CALL);
        EXPECT_EQ(memory.read_u32(OUT).value(), 0xA5A5A5A5u) << name;
        EXPECT_EQ(call(name, 0x44, OUT, 0), dxgi::S_OK) << name;
        const u32 cookie = memory.read_u32(OUT).value();
        EXPECT_NE(cookie, 0xA5A5A5A5u) << name;
        // Unregister is void and accepts its own cookie.
        (void)call(std::strstr(name, "Stereo") != nullptr
                       ? "_DXGI_UnregisterStereoStatus"
                       : "_DXGI_UnregisterOcclusionStatus",
                   cookie, 0, 0);
    }
    // No shared resource is known to a manager without the D3D11 frontend.
    ASSERT_TRUE(memory.write_u64(OUT, kPoison).is_ok());
    EXPECT_EQ(call("_DXGI_GetSharedResourceAdapterLuid", 0, OUT, 0),
              dxgi::ERROR_INVALID_CALL);
}

// The three swap-chain paths that Vitreo does serve each refuse honestly when no
// creator is registered, and null their out-parameter while doing it.
TEST_F(DxgiManagerTest, SwapChainPathsRefuseAndNullWithoutACreator) {
    constexpr GuestAddr OUT = DATA_BASE + 0xA80;
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    // This fixture's manager has no creators wired, which is the condition
    // under test: a swap-chain call must fail rather than return a handle to
    // nothing.
    struct Case {
        const char* name;
        void (DxgiManager::*method)(Marshaler&);
        usize out_index;
    };
    const Case cases[] = {
        {"CreateSwapChain", &DxgiManager::impl_CreateSwapChain, 3},
        {"CreateSwapChainForHwnd", &DxgiManager::impl_CreateSwapChainForHwnd, 6},
        {"CreateSwapChainForComposition",
         &DxgiManager::impl_CreateSwapChainForComposition, 4},
    };
    for (const Case& test_case : cases) {
        ASSERT_TRUE(memory.write_u64(OUT, 0xA5A5A5A5ull).is_ok());
        cpu.gpr.rsp = STACK_BASE;
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = 0;
        cpu.gpr.r9 = 0;
        const u64 value = static_cast<u64>(OUT);
        if (test_case.out_index == 3) {
            cpu.gpr.r9 = value;
        } else {
            ASSERT_TRUE(memory.write_u64(
                cpu.gpr.rsp + 0x28 + (test_case.out_index - 4) * 8,
                value).is_ok());
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        (manager.*test_case.method)(marshaler);
        EXPECT_NE(cpu.gpr.rax, dxgi::S_OK) << test_case.name;
        EXPECT_EQ(memory.read_u64(OUT).value(), 0u)
            << test_case.name << " must null its out-parameter on failure";
    }
}

// IDXGIFactory3 through IDXGIFactory7.
//
// A guest that cannot obtain IDXGIFactory6 falls back to EnumAdapters1, so the
// old E_NOINTERFACE was honest -- but every method these versions add has a
// definite, honest answer here, and answering is better than making the guest
// guess. The assertion that matters most is the LUID round trip: the adapter
// descriptor writes a LUID and EnumAdapterByLuid matches one, and if those two
// literals ever drift the factory reports NOT_FOUND for the adapter it just
// handed out.
TEST_F(DxgiManagerTest, Factory3Through7MethodsAnswerDefinitively) {
    constexpr GuestAddr DESC_OUT = DATA_BASE + 0xB00;
    constexpr GuestAddr IID_OUT = DATA_BASE + 0xC00;
    constexpr GuestAddr OUT_PTR = DATA_BASE + 0xC40;
    constexpr GuestAddr FEATURE = DATA_BASE + 0xC60;
    constexpr GuestAddr COOKIE = DATA_BASE + 0xC70;

    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);

    // Every version's IID must be answerable, and the vtable must be fully
    // populated: a test that calls impl_* directly never touches the vtable, so
    // a missing thunk would be a guest call to address zero that nothing here
    // would see.
    const auto vtable = memory.read_u64(factory);
    ASSERT_TRUE(vtable.is_ok());
    ASSERT_NE(vtable.value(), 0u);
    for (u32 slot = 0; slot < 32; ++slot) {
        const auto thunk = memory.read_u64(vtable.value() + slot * 8);
        ASSERT_TRUE(thunk.is_ok()) << "slot " << slot;
        EXPECT_NE(thunk.value(), 0u)
            << "factory vtable slot " << slot << " is null";
    }

    // GetCreationFlags returns UINT: a CreateDXGIFactory1 factory was made
    // with no flags.
    cpu.gpr.rcx = factory;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_GetCreationFlags(marshaler);
    }
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // Read the LUID the adapter actually reports, then hand it back.
    const GuestAddr adapter = enum_adapter(factory);
    ASSERT_NE(adapter, 0u);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_OUT;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_GetDesc(marshaler);
    }
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    // DXGI_ADAPTER_DESC: 128 wide chars of Description, then VendorId, DeviceId,
    // SubSysId, Revision (16 bytes), then three SIZE_T memory fields, then
    // AdapterLuid.
    constexpr GuestAddr LUID_OFFSET = 256 + 16 + 3 * 8;
    const u32 luid_low = memory.read_u32(DESC_OUT + LUID_OFFSET).value();
    const u32 luid_high = memory.read_u32(DESC_OUT + LUID_OFFSET + 4).value();
    EXPECT_NE(luid_low, 0u) << "the adapter must report a LUID";

    const std::array<u8, 16> adapter_iid{
        0xE1, 0xE7, 0x11, 0x24, 0xAC, 0x12, 0xCF, 0x4C,
        0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0};  // IID_IDXGIAdapter
    ASSERT_TRUE(memory.write_bytes(
        IID_OUT, reinterpret_cast<const Byte*>(adapter_iid.data()),
        adapter_iid.size()).is_ok());

    const auto by_luid = [&](u32 low, u32 high) {
        (void)memory.write_u64(OUT_PTR, 0xA5A5A5A5ull);
        cpu.gpr.rsp = STACK_BASE;
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = (static_cast<u64>(high) << 32) | low;  // LUID by value
        cpu.gpr.r8 = IID_OUT;
        cpu.gpr.r9 = OUT_PTR;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_EnumAdapterByLuid(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(by_luid(luid_low, luid_high), dxgi::S_OK)
        << "the LUID the adapter reports must be the one EnumAdapterByLuid "
           "accepts";
    EXPECT_NE(memory.read_u64(OUT_PTR).value(), 0u);
    EXPECT_EQ(by_luid(luid_low ^ 0xFFu, luid_high), dxgi::ERROR_NOT_FOUND);
    EXPECT_EQ(memory.read_u64(OUT_PTR).value(), 0u)
        << "a failed lookup must null the out-parameter";

    // EnumWarpAdapter: there is no software rasterizer, and handing back the
    // hardware adapter would silently give the GPU to a guest that deliberately
    // asked for software.
    (void)memory.write_u64(OUT_PTR, 0xA5A5A5A5ull);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_OUT;
    cpu.gpr.r8 = OUT_PTR;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_EnumWarpAdapter(marshaler);
    }
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_NOT_FOUND);
    EXPECT_EQ(memory.read_u64(OUT_PTR).value(), 0u);

    // CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) must answer FALSE
    // definitively. A guest told TRUE passes DXGI_PRESENT_ALLOW_TEARING with a
    // zero sync interval and then gets a frame pace it did not ask for.
    ASSERT_TRUE(memory.write_u32(FEATURE, 0xEEEEEEEEu).is_ok());
    cpu.gpr.rsp = STACK_BASE;
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0;  // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    cpu.gpr.r8 = FEATURE;
    cpu.gpr.r9 = 4;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_FactoryCheckFeatureSupport(marshaler);
    }
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(FEATURE).value(), 0u) << "tearing is FALSE";
    // An unknown feature is refused rather than answered.
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 99;
    cpu.gpr.r8 = FEATURE;
    cpu.gpr.r9 = 4;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_FactoryCheckFeatureSupport(marshaler);
    }
    // Measured on Windows: an unrecognised feature is DXGI_ERROR_INVALID_CALL.
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL);

    // EnumAdapterByGpuPreference: one adapter, so every preference resolves to
    // it. An engine asking for HIGH_PERFORMANCE must get the GPU, not an error.
    const auto by_preference = [&](u32 index, u32 preference) {
        (void)memory.write_u64(OUT_PTR, 0xA5A5A5A5ull);
        cpu.gpr.rsp = STACK_BASE;
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = index;
        cpu.gpr.r8 = preference;
        cpu.gpr.r9 = IID_OUT;
        (void)memory.write_u64(cpu.gpr.rsp + 0x28, OUT_PTR);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_EnumAdapterByGpuPreference(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    for (u32 preference = 0; preference <= 2; ++preference) {
        EXPECT_EQ(by_preference(0, preference), dxgi::S_OK)
            << "preference " << preference;
        EXPECT_NE(memory.read_u64(OUT_PTR).value(), 0u);
    }
    EXPECT_EQ(by_preference(1, 0), dxgi::ERROR_NOT_FOUND);
    EXPECT_EQ(memory.read_u64(OUT_PTR).value(), 0u);

    // Adapters-changed registration succeeds and the event never fires, which is
    // correct: the host exposes one Metal adapter for the process's life, so
    // there is nothing to report.
    ASSERT_TRUE(memory.write_u32(COOKIE, 0xEEEEEEEEu).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0x1234;  // a guest event handle
    cpu.gpr.r8 = COOKIE;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_RegisterAdaptersChangedEvent(marshaler);
    }
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const u32 cookie = memory.read_u32(COOKIE).value();
    EXPECT_NE(cookie, 0u) << "the guest needs a cookie to unregister with";
    const auto unregister = [&](u32 value) {
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = value;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_UnregisterAdaptersChangedEvent(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    // Measured (dxgi_a11_contracts_probe64.expected): an unknown cookie and
    // a second unregister are both DXGI_ERROR_NOT_FOUND. This used to expect
    // INVALID_CALL.
    EXPECT_EQ(unregister(cookie + 1000), dxgi::ERROR_NOT_FOUND)
        << "a cookie this factory never issued is not found";
    EXPECT_EQ(unregister(cookie), dxgi::S_OK);
    EXPECT_EQ(unregister(cookie), dxgi::ERROR_NOT_FOUND)
        << "a second unregister finds nothing";

    // A null event handle has nothing to signal.
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = COOKIE;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_RegisterAdaptersChangedEvent(marshaler);
    }
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL);
}

TEST_F(DxgiManagerTest, CreatesFactory1WithNativeWidthVtable) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(vtable + 0 * 8).value(), QUERY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(), ENUM_ADAPTERS_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 12 * 8).value(), ENUM_ADAPTERS1_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 13 * 8).value(), IS_CURRENT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 17 * 8).value(),
              GET_DESC1_THUNK + 0x10);
    EXPECT_EQ(memory.read_u64(vtable + 23 * 8).value(),
              GET_DESC1_THUNK + 0x70);
    EXPECT_EQ(memory.read_u64(vtable + 24 * 8).value(),
              CREATE_SWAPCHAIN_THUNK + 0x30);
}

TEST_F(DxgiManagerTest, ReleasedFactoryStorageIsReusable) {
    GuestAddr first_factory = 0;
    for (usize iteration = 0; iteration < 2048; ++iteration) {
        const GuestAddr factory = create_factory();
        ASSERT_NE(factory, 0u) << "iteration " << iteration;
        if (iteration == 0) {
            first_factory = factory;
        } else {
            EXPECT_EQ(factory, first_factory);
        }

        cpu.gpr.rcx = factory;
        Marshaler release(memory);
        release.attach_cpu(cpu);
        manager.impl_Release(release);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
}

// Learn CreateDXGIFactory2 (dxgi.dll, Windows 8.1+): the flags-taking form
// every modern engine imports statically — Unity 6 for both its D3D12 and
// D3D11 back ends. DXGI_CREATE_FACTORY_DEBUG needs the SDK debug layer,
// which a non-developer machine does not have; an undefined flag is refused.
TEST_F(DxgiManagerTest, CreateDXGIFactory2HonoursItsFlags) {
    EXPECT_EQ(create_factory2(0), dxgi::S_OK);
    EXPECT_NE(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
    // Measured on Windows 11 without the debug layer
    // (dxgi_a11_contracts_probe64.expected): DXGI_CREATE_FACTORY_DEBUG is
    // INVALID_CALL, and undefined flags are accepted. This used to expect
    // SDK_COMPONENT_MISSING and E_INVALIDARG.
    EXPECT_EQ(create_factory2(0x1), dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
    EXPECT_EQ(create_factory2(0x2), dxgi::S_OK);
    EXPECT_NE(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
}

// Measured on Windows 11 (tools/oracle_probe/dxgi_a11_contracts_probe64
// .expected [factory.create] / [factory.methods] / [adapter] / nullargs).
TEST_F(DxgiManagerTest, FactoryAndAdapterContractsAsMeasured) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // CreateDXGIFactory (1.0) refuses exactly IDXGIFactory1.
    cpu.gpr.rcx = IID_ADDRESS;  // IID_IDXGIFactory1
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 1).is_ok());
    manager.impl_CreateDXGIFactory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_NOINTERFACE);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
    // CreateDXGIFactory2(0x4): GetCreationFlags echoes it; bit 31 dropped.
    ASSERT_EQ(create_factory2(0x4), dxgi::S_OK);
    cpu.gpr.rcx = memory.read_u64(OUTPUT_ADDRESS).value();
    manager.impl_GetCreationFlags(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x4u);
    ASSERT_EQ(create_factory2(0x80000000u), dxgi::S_OK);
    cpu.gpr.rcx = memory.read_u64(OUTPUT_ADDRESS).value();
    manager.impl_GetCreationFlags(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    const GuestAddr factory = create_factory();
    // MakeWindowAssociation: DXGI_MWA_* only (8 and 0x100 INVALID_CALL); a
    // NULL window is accepted; GetWindowAssociation reports NULL.
    const auto mwa = [&](u64 window, u32 flags) {
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = window;
        cpu.gpr.r8 = flags;
        manager.impl_MakeWindowAssociation(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(mwa(0x10010, 0x7), dxgi::S_OK);
    EXPECT_EQ(mwa(0x10010, 0x8), dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(mwa(0x10010, 0x100), dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(mwa(0, 0x2), dxgi::S_OK);
    ASSERT_EQ(mwa(0x10010, 0x2), dxgi::S_OK);
    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 1).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    manager.impl_GetWindowAssociation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
    // CheckFeatureSupport(ALLOW_TEARING) with a wrong size: INVALID_CALL.
    for (const u64 size : {0ull, 8ull}) {
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        cpu.gpr.r9 = size;
        manager.impl_FactoryCheckFeatureSupport(marshaler);
        EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL) << size;
    }
    // RegisterAdaptersChangedEvent(NULL): INVALID_CALL, cookie untouched.
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 0xA5A5A5A5u).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    manager.impl_RegisterAdaptersChangedEvent(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value(), 0xA5A5A5A5u);
    // EnumOutputs(0, NULL): E_INVALIDARG.
    const GuestAddr adapter = enum_adapter(factory);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    manager.impl_EnumOutputs(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);
    // CheckInterfaceSupport(ID3D10Device) while the D3D10 frontend is off
    // (its default): UNSUPPORTED, UMD version untouched.
    const std::array<u8, 16> d3d10_device{
        0x0F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
        0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
    ASSERT_TRUE(memory.write_bytes(
        DESC_ADDRESS, reinterpret_cast<const Byte*>(d3d10_device.data()),
        d3d10_device.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 0xABCDull).is_ok());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    manager.impl_CheckInterfaceSupport(marshaler);
    if (!d3d10::d3d10_enabled()) {
        EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_UNSUPPORTED);
        EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0xABCDull);
    }
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
