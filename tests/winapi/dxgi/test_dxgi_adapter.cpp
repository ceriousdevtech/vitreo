// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"

#include <array>
#include <cstring>

namespace vitreo::winapi::dxgi_test {
namespace {

TEST_F(DxgiManagerTest, EnumeratesOneAdapterAndReturnsNotFoundAfterIt) {
    const GuestAddr factory = create_factory();
    const GuestAddr adapter = enum_adapter(factory);
    ASSERT_NE(adapter, 0u);

    const GuestAddr vtable = memory.read_u64(adapter).value();
    EXPECT_EQ(memory.read_u64(vtable + 8 * 8).value(), GET_DESC_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 10 * 8).value(), GET_DESC1_THUNK);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = ADAPTER_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_EnumAdapters1(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_NOT_FOUND);
    EXPECT_EQ(memory.read_u64(ADAPTER_ADDRESS).value(), 0u);
}

// Adapter profile policy (.docs/contracts/dxgi.md): a discrete-class adapter.
// DXGI_ADAPTER_DESC1 x64 (dxgi.h): Description[128] WCHAR, four UINTs from
// 256, DedicatedVideoMemory / DedicatedSystemMemory / SharedSystemMemory as
// 8-byte SIZE_Ts at 272 / 280 / 288, AdapterLuid at 296, Flags at 304
// (DXGI_ADAPTER_FLAG_NONE: not remote, not software).
TEST_F(DxgiManagerTest, GetDesc1ReportsMetalAdapterAsDiscreteClass) {
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_GetDesc1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);

    std::array<u16, 128> description{};
    ASSERT_TRUE(memory.read_bytes(
        DESC_ADDRESS, reinterpret_cast<Byte*>(description.data()),
        description.size() * sizeof(u16)).is_ok());
    const char expected[] = "Test Apple GPU";
    for (usize index = 0; index < std::strlen(expected); ++index) {
        EXPECT_EQ(description[index], static_cast<u16>(expected[index]));
    }
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 256).value(),
              dxgi::VENDOR_ID_APPLE);
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 272).value(),
              8ull * 1024 * 1024 * 1024) << "DedicatedVideoMemory";
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 280).value(), 0u)
        << "DedicatedSystemMemory";
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 288).value(), 0u)
        << "SharedSystemMemory";
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 304).value(), 0u) << "Flags";
}

TEST_F(DxgiManagerTest, Adapter2GetDesc2AndFactory2QueryInterface) {
    constexpr Guid IID_IDXGIADAPTER2{
        0x0AA1AE0Au, 0xFA0Eu, 0x4B84u,
        {0x86, 0x44, 0xE0, 0x5F, 0xF8, 0xE5, 0xAC, 0xB5}};
    constexpr Guid IID_IDXGIFACTORY2{
        0x50C83A1Cu, 0xE072u, 0x4C48u,
        {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};

    const GuestAddr factory = create_factory();
    write_guid(IID_ADDRESS, IID_IDXGIFACTORY2);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_QueryInterface(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), factory);

    const GuestAddr adapter = enum_adapter(factory);
    write_guid(IID_ADDRESS, IID_IDXGIADAPTER2);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    manager.impl_QueryInterface(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    manager.impl_GetDesc2(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 304).value(), 0u); // Flags
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 308).value(), 0u); // Graphics
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 312).value(), 0u); // Compute
}

TEST_F(DxgiManagerTest, AdapterParentPreservesFactoryIdentity) {
    const GuestAddr factory = create_factory();
    const GuestAddr adapter = enum_adapter(factory);
    write_guid(IID_ADDRESS, IID_IDXGIFACTORY);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler get_parent(memory);
    get_parent.attach_cpu(cpu);
    manager.impl_GetParent(get_parent);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), factory);
}

TEST_F(DxgiManagerTest, UnsupportedInterfaceCheckUsesDxgiError) {
    const GuestAddr adapter = enum_adapter(create_factory());
    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 0xFFFFFFFFFFFFFFFFull).is_ok());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler check(memory);
    check.attach_cpu(cpu);
    manager.impl_CheckInterfaceSupport(check);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_UNSUPPORTED);
    // The UMD version is left untouched (measured,
    // dxgi_a11_contracts_probe64.expected "CheckInterfaceSupport(IUnknown)");
    // this used to expect it zeroed.
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0xFFFFFFFFFFFFFFFFull);
}

TEST_F(DxgiManagerTest, ReportsImplementedDxgiDeviceInterfaceAndDriverVersion) {
    const GuestAddr adapter = enum_adapter(create_factory());
    write_guid(IID_ADDRESS, IID_IDXGIDEVICE);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler check(memory);
    check.attach_cpu(cpu);
    manager.impl_CheckInterfaceSupport(check);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(),
              0x0020000000651754ull);
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
