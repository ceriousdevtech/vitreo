// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"

#include <bit>
#include <chrono>

namespace vitreo::winapi::dxgi_test {
namespace {

// The IDXGIOutput methods that need full-screen output ownership report the
// documented "you are not in that mode" code, not E_NOTIMPL.
//
// Learn attaches the same note to gamma control, display-surface access and
// output frame statistics: "Calling this method is only supported while in
// full-screen mode", and GetFrameStatistics documents DXGI_ERROR_INVALID_CALL.
// TakeOwnership never succeeds here, so a guest can never reach that mode --
// which makes INVALID_CALL factually right for every reachable call, and safer
// than E_NOTIMPL, which an engine may read as a driver fault.
TEST_F(DxgiManagerTest, FullScreenOnlyOutputMethodsReportInvalidCall) {
    const GuestAddr factory = create_factory();
    const GuestAddr adapter = enum_adapter(factory);
    ASSERT_NE(adapter, 0u);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_EnumOutputs(marshaler);
    }
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    struct Case { usize slot; const char* name; };
    const Case cases[] = {
        {13, "GetGammaControlCapabilities"},
        {14, "SetGammaControl"},
        {15, "GetGammaControl"},
        {16, "SetDisplaySurface"},
        {17, "GetDisplaySurfaceData"},
        {18, "GetFrameStatistics"},
        {21, "GetDisplaySurfaceData1"},
    };
    for (const Case& test_case : cases) {
        cpu.gpr.rsp = STACK_BASE;
        cpu.gpr.rcx = output;
        cpu.gpr.rdx = 0;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_OutputMethod(marshaler, test_case.slot);
        EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL)
            << test_case.name << " (slot " << test_case.slot << ")";
    }

    // And the reason those codes are right: ownership is never granted.
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_OutputMethod(marshaler, 11);  // TakeOwnership
    }
    EXPECT_NE(cpu.gpr.rax, dxgi::S_OK)
        << "if TakeOwnership ever succeeds, the codes above stop being right";
}

TEST_F(DxgiManagerTest, EnumeratesPrimaryOutputAndReportsRealDescriptor) {
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler enumerate(memory);
    enumerate.attach_cpu(cpu);
    manager.impl_EnumOutputs(enumerate);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    const GuestAddr vtable = memory.read_u64(output).value();
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(),
              OUTPUT_METHOD_BASE + 0x70);

    cpu.gpr.rcx = output;
    cpu.gpr.rdx = DESC_ADDRESS;
    Marshaler get_desc(memory);
    get_desc.attach_cpu(cpu);
    manager.impl_OutputMethod(get_desc, 7);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u16(DESC_ADDRESS).value(), u'\\');
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 64).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 72).value(), 2560u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 76).value(), 1440u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 80).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 84).value(), 1u);
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 88).value(), 0x000D0001u);

    write_guid(IID_ADDRESS, IID_IDXGIADAPTER);
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = ADAPTER_ADDRESS;
    Marshaler parent(memory);
    parent.attach_cpu(cpu);
    manager.impl_GetParent(parent);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(ADAPTER_ADDRESS).value(), adapter);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    manager.impl_EnumOutputs(enumerate);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_NOT_FOUND);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
}

TEST_F(DxgiManagerTest, WaitForVBlankBlocksUntilTheNextRefreshBoundary) {
    // Learn IDXGIOutput::WaitForVBlank: block until the next vertical blank
    // and return S_OK. Compositors clock their frames from this call, so
    // E_NOTIMPL left them with no tick at all.
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler enumerate(memory);
    enumerate.attach_cpu(cpu);
    manager.impl_EnumOutputs(enumerate);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    cpu.gpr.rcx = output;
    Marshaler wait(memory);
    wait.attach_cpu(cpu);
    const auto started = std::chrono::steady_clock::now();
    manager.impl_OutputMethod(wait, 10);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    // The fixture output runs at 120 Hz, so one blank is at most ~8.3 ms.
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));
}

TEST_F(DxgiManagerTest, OutputModeListSupportsCountFillAndMoreData) {
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler enumerate(memory);
    enumerate.attach_cpu(cpu);
    manager.impl_EnumOutputs(enumerate);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    constexpr u32 DXGI_FORMAT_B8G8R8A8_UNORM = 87;
    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = DXGI_FORMAT_B8G8R8A8_UNORM;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = PRIVATE_SIZE_ADDRESS;
    Marshaler modes(memory);
    modes.attach_cpu(cpu);
    manager.impl_OutputMethod(modes, 8);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const u32 mode_count = memory.read_u32(PRIVATE_SIZE_ADDRESS).value();
    ASSERT_GT(mode_count, 1u);

    // Learn GetDisplayModeList: with pDesc set, *pNumModes "returns the
    // number of display modes returned in pDesc"; a short array is filled
    // and DXGI_ERROR_MORE_DATA says there were more. This used to report the
    // required count and fill nothing.
    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28,
                                 PRIVATE_OUTPUT_ADDRESS).is_ok());
    ASSERT_TRUE(memory.write_u32(PRIVATE_OUTPUT_ADDRESS, 0xCCCCCCCCu).is_ok());
    manager.impl_OutputMethod(modes, 8);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_MORE_DATA);
    EXPECT_EQ(memory.read_u32(PRIVATE_SIZE_ADDRESS).value(), 1u);
    EXPECT_NE(memory.read_u32(PRIVATE_OUTPUT_ADDRESS).value(), 0xCCCCCCCCu);

    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, mode_count).is_ok());
    manager.impl_OutputMethod(modes, 8);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS).value(), 2560u);
    EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS + 4).value(), 1440u);
    EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS + 8).value(), 120u);
    EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS + 12).value(), 1u);
    EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS + 16).value(),
              DXGI_FORMAT_B8G8R8A8_UNORM);
    for (const usize slot : {8u, 19u}) {
        manager.impl_OutputMethod(modes, slot);
        ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
        bool has_720p = false;
        bool has_1080p = false;
        const usize stride = slot == 19 ? 32 : 28;
        for (u32 index = 0; index < mode_count; ++index) {
            const GuestAddr entry = PRIVATE_OUTPUT_ADDRESS + index * stride;
            const u32 width = memory.read_u32(entry).value();
            const u32 height = memory.read_u32(entry + 4).value();
            has_720p |= width == 1280 && height == 720;
            has_1080p |= width == 1920 && height == 1080;
            EXPECT_LE(width, 2560u);
            EXPECT_LE(height, 1440u);
            if (slot == 19) EXPECT_EQ(memory.read_u32(entry + 28).value(), 0u);
        }
        EXPECT_TRUE(has_720p);
        EXPECT_TRUE(has_1080p);
    }
    for (const usize slot : {9u, 20u}) {
        ASSERT_TRUE(memory.write_u32(DESC_ADDRESS, 1280).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC_ADDRESS + 4, 720).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC_ADDRESS + 16, 87).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC_ADDRESS + 28, 0).is_ok());
        cpu.gpr.rcx = output;
        cpu.gpr.rdx = DESC_ADDRESS;
        cpu.gpr.r8 = PRIVATE_OUTPUT_ADDRESS;
        cpu.gpr.r9 = 0;
        manager.impl_OutputMethod(modes, slot);
        ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
        EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS).value(), 1280u);
        EXPECT_EQ(memory.read_u32(PRIVATE_OUTPUT_ADDRESS + 4).value(), 720u);
    }
}

TEST_F(DxgiManagerTest, Output3ReportsNoOverlayCapability) {
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler enumerate(memory);
    enumerate.attach_cpu(cpu);
    manager.impl_EnumOutputs(enumerate);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    write_guid(IID_ADDRESS, IID_IDXGIOUTPUT3);
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), output);

    const GuestAddr vtable = memory.read_u64(output).value();
    EXPECT_EQ(memory.read_u64(vtable + 24 * 8).value(),
              OUTPUT_METHOD_BASE + 0x180);

    cpu.gpr.rcx = output;
    Marshaler overlays(memory);
    overlays.attach_cpu(cpu);
    manager.impl_OutputMethod(overlays, 23);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, 0xFFFFFFFFu).is_ok());
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = 87;
    cpu.gpr.r8 = 0x63001000;
    cpu.gpr.r9 = PRIVATE_SIZE_ADDRESS;
    manager.impl_OutputMethod(overlays, 24);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(PRIVATE_SIZE_ADDRESS).value(), 0u);
}

TEST_F(DxgiManagerTest, Output6ReportsSdrDescriptorAndNoOverlayColorSpaces) {
    const GuestAddr adapter = enum_adapter(create_factory());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler enumerate(memory);
    enumerate.attach_cpu(cpu);
    manager.impl_EnumOutputs(enumerate);
    const GuestAddr output = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(output, 0u);

    write_guid(IID_ADDRESS, IID_IDXGIOUTPUT6);
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), output);

    const GuestAddr vtable = memory.read_u64(output).value();
    EXPECT_EQ(memory.read_u64(vtable + 27 * 8).value(),
              OUTPUT_METHOD_BASE + 0x1B0);

    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28,
                                 PRIVATE_SIZE_ADDRESS).is_ok());
    cpu.gpr.rcx = output;
    cpu.gpr.rdx = 87;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0x63001000;
    Marshaler overlay_color_space(memory);
    overlay_color_space.attach_cpu(cpu);
    manager.impl_OutputMethod(overlay_color_space, 25);
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(PRIVATE_SIZE_ADDRESS).value(), 0u);

    cpu.gpr.rcx = output;
    cpu.gpr.rdx = DESC_ADDRESS;
    Marshaler get_desc1(memory);
    get_desc1.attach_cpu(cpu);
    manager.impl_OutputMethod(get_desc1, 27);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 88).value(), 0x000D0001u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 96).value(), 10u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 100).value(), 0u);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(
                        memory.read_u32(DESC_ADDRESS + 104).value()),
                    0.6400f);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(
                        memory.read_u32(DESC_ADDRESS + 132).value()),
                    0.3290f);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 136).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 140).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 144).value(), 0u);
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
