// SPDX-License-Identifier: LGPL-2.1-only
//
// x86 native-width stdcall frames and layouts, plus the x64 siblings measured with them.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

namespace classlink_blobs {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_classlink_shaders.h"
#pragma clang diagnostic pop
}  // namespace classlink_blobs

#include "d3d11_test_fixture_x86.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerX86Test, CreatesNativeWidthDeviceAndImmediateContext) {
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);

    const u32 device = memory.read_u32(DEVICE_OUT).value();
    const u32 context = memory.read_u32(CONTEXT_OUT).value();
    ASSERT_NE(device, 0u);
    ASSERT_NE(context, 0u);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(),
              d3d11::FEATURE_LEVEL_11_0);

    const u32 device_vtable = memory.read_u32(device).value();
    const u32 context_vtable = memory.read_u32(context).value();
    EXPECT_NE(memory.read_u32(device_vtable + 37 * 4).value(), 0u);
    EXPECT_NE(memory.read_u32(device_vtable + 40 * 4).value(), 0u);
    EXPECT_NE(memory.read_u32(device_vtable + 68 * 4).value(), 0u);
    EXPECT_NE(memory.read_u32(context_vtable + 3 * 4).value(), 0u);
    EXPECT_NE(memory.read_u32(context_vtable + 112 * 4).value(), 0u);
}

TEST_F(D3D11ManagerX86Test, TiledUint64ArgumentsTakeTwoStackSlots) {
    // ResizeTilePool(pTilePool, UINT64) and CopyTiles(..., UINT64
    // BufferStartOffsetInBytes, Flags): a UINT64 is two stdcall slots on x86
    // (d3d11_2.h; CONTEXT_STDCALL_BYTES 16 and 32), low dword first.
    backend.state.tiled_resources = true;
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 device = memory.read_u32(DEVICE_OUT).value();
    const u32 context = memory.read_u32(CONTEXT_OUT).value();
    const auto call = [&](bool on_device, usize slot,
                          std::initializer_list<u32> args) {
        set_arg(0, on_device ? device : context);
        usize i = 1;
        for (const u32 a : args) set_arg(i++, a);
        if (on_device) {
            d3d11.impl_DeviceMethod(marshaler, slot);
        } else {
            d3d11.impl_ContextMethod(marshaler, slot);
        }
        return static_cast<u32>(cpu.gpr.rax);
    };
    constexpr GuestAddr DESC = DATA_BASE + 0x1800;
    constexpr GuestAddr OUT = DATA_BASE + 0x1880;
    constexpr GuestAddr COORD = DATA_BASE + 0x1900;
    constexpr GuestAddr SIZE = DATA_BASE + 0x1920;
    constexpr GuestAddr OFFSETS = DATA_BASE + 0x1940;
    const auto buffer = [&](u32 bytes, u32 bind, u32 misc) {
        const u32 words[6] = {bytes, 0, bind, 0, misc, 0};
        for (u32 i = 0; i < 6; ++i) {
            EXPECT_TRUE(memory.write_u32(DESC + i * 4, words[i]).is_ok());
        }
        EXPECT_EQ(call(true, 3, {static_cast<u32>(DESC), 0,
                                 static_cast<u32>(OUT)}),
                  d3d11::S_OK);
        return memory.read_u32(OUT).value();
    };
    const u32 pool = buffer(2 * 65536, 0, 0x20000);
    const u32 tiled = buffer(2 * 65536, 0x8, 0x40000);
    const u32 linear = buffer(4 * 65536, 0, 0);
    EXPECT_EQ(call(false, 138, {pool, 3 * 65536, 0}), d3d11::S_OK);
    set_arg(0, pool);
    set_arg(1, static_cast<u32>(OUT));
    d3d11.impl_BufferMethod(marshaler, 10);  // GetDesc
    EXPECT_EQ(memory.read_u32(OUT).value(), 3u * 65536u);
    EXPECT_EQ(call(false, 138, {pool, 0, 1}), d3d11::E_INVALIDARG)
        << "4 GiB: the high dword counts";
    ASSERT_TRUE(memory.write_u32(OFFSETS, 0).is_ok());
    EXPECT_EQ(call(false, 134, {tiled, 1, 0, 0, pool, 1, 0,
                                static_cast<u32>(OFFSETS), 0, 0}),
              d3d11::S_OK);
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory.write_u32(COORD + i * 4, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(SIZE + i * 4, i == 0 ? 1u : 0u).is_ok());
    }
    backend.state.buffer_region_copies.clear();
    call(false, 136, {tiled, static_cast<u32>(COORD), static_cast<u32>(SIZE),
                      linear, 65536, 0, 4});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 65536u);
    backend.state.buffer_region_copies.clear();
    call(false, 136, {tiled, static_cast<u32>(COORD), static_cast<u32>(SIZE),
                      linear, 0, 1, 4});
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "offset 4 GiB overruns the linear buffer";
}

TEST_F(D3D11ManagerX86Test, CreateDeviceAndSwapChainUsesNativeDescLayout) {
    d3d11.set_presentation_view_resolver(
        [](u64 hwnd) -> void* {
            return hwnd == 0x55u ? reinterpret_cast<void*>(0xABCD)
                                 : nullptr;
        });

    constexpr GuestAddr SWAP_DESC = DATA_BASE + 0x180;
    constexpr GuestAddr SWAP_OUT = DATA_BASE + 0x200;
    // x86 DXGI_SWAP_CHAIN_DESC: HWND at offset 44.
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 0, 640).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 4, 480).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 8, 60).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 16, 87).is_ok()); // B8G8R8A8
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 28, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 32, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 36, 0x20).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 40, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 44, 0x55).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 48, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 52, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 56, 0).is_ok());

    for (usize index = 0; index < 12; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(SWAP_DESC));
    set_arg(8, static_cast<u32>(SWAP_OUT));
    set_arg(9, static_cast<u32>(DEVICE_OUT));
    set_arg(10, static_cast<u32>(LEVEL_OUT));
    set_arg(11, static_cast<u32>(CONTEXT_OUT));

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDeviceAndSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    EXPECT_NE(memory.read_u32(SWAP_OUT).value(), 0u);
    EXPECT_NE(memory.read_u32(DEVICE_OUT).value(), 0u);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
}

// x86: D3D11CreateDeviceAndSwapChain with the x86 DXGI_SWAP_CHAIN_DESC
// (HWND at 44, Windowed at 48, Flags at 56) and Windowed=FALSE enters
// full-screen; GetFullscreenState writes a 4-byte BOOL and a 4-byte
// IDXGIOutput pointer.
TEST_F(D3D11ManagerX86Test, FullscreenStateUsesNativeWidthOutParameters) {
    d3d11.set_presentation_view_resolver(
        [](u64 hwnd) -> void* {
            return hwnd == 0x55u ? reinterpret_cast<void*>(0xABCD) : nullptr;
        });
    std::vector<std::tuple<u64, u32, u32, bool>> enters;
    std::vector<std::pair<u64, bool>> host;
    D3D11Manager::WindowServices window;
    window.client_size = [](u64) -> std::optional<std::pair<u32, u32>> {
        return std::pair<u32, u32>{640, 480};
    };
    window.window_rect = [](u64) -> std::optional<std::array<i32, 4>> {
        return std::array<i32, 4>{10, 10, 650, 490};
    };
    window.enter_exclusive = [&](u64 hwnd, std::array<i32, 4>, u32 w, u32 h,
                                 bool change) {
        enters.emplace_back(hwnd, w, h, change);
        return true;
    };
    window.leave_exclusive = [](u64, bool, std::optional<std::array<i32, 4>>) {};
    window.host_fullscreen = [&](u64 hwnd, bool enable) {
        host.emplace_back(hwnd, enable);
    };
    window.application_active = [] { return true; };
    window.has_keyboard_focus = [](u64) { return true; };
    d3d11.set_window_services(window);
    dxgi.set_output_resolver([] {
        return std::vector<dxgi::OutputInfo>{dxgi::OutputInfo{
            .device_name = "\\\\.\\DISPLAY1",
            .left = 0, .top = 0, .right = 1920, .bottom = 1200,
            .monitor = 0xD0001, .bits_per_color = 8, .refresh_rate_hz = 60.0}};
    });

    constexpr GuestAddr SWAP_DESC = DATA_BASE + 0x180;
    constexpr GuestAddr SWAP_OUT = DATA_BASE + 0x200;
    constexpr GuestAddr FS_OUT = DATA_BASE + 0x210;
    constexpr GuestAddr TARGET_OUT = DATA_BASE + 0x218;
    const std::array<u32, 15> desc{640, 480, 60, 1, 87, 0, 0, 1, 0, 0x20, 1,
                                   0x55, 0 /* Windowed */, 0 /* DISCARD */,
                                   0x2 /* ALLOW_MODE_SWITCH */};
    ASSERT_TRUE(memory.write_bytes(SWAP_DESC,
                                   reinterpret_cast<const Byte*>(desc.data()),
                                   sizeof(desc)).is_ok());
    for (usize index = 0; index < 12; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(SWAP_DESC));
    set_arg(8, static_cast<u32>(SWAP_OUT));
    set_arg(9, static_cast<u32>(DEVICE_OUT));
    set_arg(10, static_cast<u32>(LEVEL_OUT));
    set_arg(11, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDeviceAndSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 chain = memory.read_u32(SWAP_OUT).value();
    ASSERT_NE(chain, 0u);
    // 640x480 is a listed mode of the 1920x1200 output: that mode.
    EXPECT_EQ(enters, (std::vector<std::tuple<u64, u32, u32, bool>>{
                          {0x55, 640, 480, true}}));
    EXPECT_EQ(host, (std::vector<std::pair<u64, bool>>{{0x55, true}}));

    ASSERT_TRUE(memory.write_u32(FS_OUT, 7).is_ok());
    ASSERT_TRUE(memory.write_u32(FS_OUT + 4, 0xA5A5A5A5u).is_ok());
    ASSERT_TRUE(memory.write_u32(TARGET_OUT, 0x1111).is_ok());
    ASSERT_TRUE(memory.write_u32(TARGET_OUT + 4, 0xA5A5A5A5u).is_ok());
    set_arg(0, chain);
    set_arg(1, static_cast<u32>(FS_OUT));
    set_arg(2, static_cast<u32>(TARGET_OUT));
    d3d11.impl_SwapChainMethod(marshaler, 11);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(FS_OUT).value(), 1u);
    EXPECT_EQ(memory.read_u32(FS_OUT + 4).value(), 0xA5A5A5A5u);
    const u32 output = memory.read_u32(TARGET_OUT).value();
    EXPECT_NE(output, 0u);
    EXPECT_NE(output, 0x1111u);
    EXPECT_EQ(memory.read_u32(TARGET_OUT + 4).value(), 0xA5A5A5A5u)
        << "an x86 interface pointer is four bytes";
    EXPECT_TRUE(dxgi.is_output_object(output));

    // SetFullscreenState(FALSE, NULL): stdcall this + 2 arguments.
    set_arg(0, chain);
    set_arg(1, 0);
    set_arg(2, 0);
    d3d11.impl_SwapChainMethod(marshaler, 10);
    EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{0x55, false}));
}

// The replay rebuilds a stdcall frame on x86: every argument is a 32-bit
// stack slot, FLOATs included.
TEST_F(D3D11ManagerX86Test, DeferredRecordingReplaysThroughTheStdcallFrame) {
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 device = memory.read_u32(DEVICE_OUT).value();
    const u32 immediate = memory.read_u32(CONTEXT_OUT).value();
    constexpr u32 CTX_OUT = DATA_BASE + 0x1000;
    constexpr u32 DESC = DATA_BASE + 0x1010;
    constexpr u32 OUT = DATA_BASE + 0x1040;
    constexpr u32 LIST_OUT = DATA_BASE + 0x1068;
    constexpr u32 BOX = DATA_BASE + 0x1070;
    constexpr u32 SOURCE = DATA_BASE + 0x1100;
    const auto context_call = [&](u32 self, usize slot,
                                  std::initializer_list<u32> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u32 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto make = [&](usize slot, const auto& desc) {
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, slot);
        EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
        return memory.read_u32(OUT).value();
    };
    const u32 destination = make(3, std::array<u32, 6>{64, 0, 0x1, 0, 0, 0});
    const u32 depth = make(5, std::array<u32, 11>{4, 4, 1, 1, 45, 1, 0, 0,
                                                   0x40, 0, 0});
    set_arg(0, device);
    set_arg(1, depth);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 dsv = memory.read_u32(OUT).value();
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 deferred = memory.read_u32(CTX_OUT).value();
    EXPECT_EQ(context_call(deferred, 112, {}), 1u);

    std::array<u8, 64> source{};
    for (usize i = 0; i < source.size(); ++i) source[i] = static_cast<u8>(0x40 + i);
    ASSERT_TRUE(memory.write_bytes(SOURCE, source.data(), source.size()).is_ok());
    const std::array<u32, 6> box{4, 0, 0, 12, 1, 1};
    ASSERT_TRUE(memory.write_bytes(
        BOX, reinterpret_cast<const Byte*>(box.data()), sizeof(box)).is_ok());
    const unsigned updates = backend.state.buffer_updates;
    context_call(deferred, 48, {destination, 0, BOX, SOURCE, 0, 0});
    context_call(deferred, 53, {dsv, 3, 0x3E800000u /* 0.25f */, 9});
    EXPECT_EQ(backend.state.buffer_updates, updates);
    EXPECT_EQ(context_call(deferred, 114, {0, LIST_OUT}) , d3d11::S_OK);
    const u32 list = memory.read_u32(LIST_OUT).value();
    ASSERT_NE(list, 0u);
    backend.state.last_depth_clear_value = -1.0f;
    context_call(immediate, 58, {list, 0});
    ASSERT_EQ(backend.state.buffer_updates, updates + 1);
    EXPECT_EQ(backend.state.last_buffer_update_offset, 4u);
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(source.begin() + 4, source.begin() + 12));
    EXPECT_EQ(backend.state.last_depth_clear_value, 0.25f);
    EXPECT_EQ(backend.state.last_stencil_clear_value, 9u);
}

// D3D11On12CreateDevice, measured on Windows 11
// (tools/oracle_probe/gfx_media_exports_probe.expected): out-params NULLed,
// pChosenFeatureLevel untouched, NULL / non-D3D12 device -> E_INVALIDARG after
// exactly one QueryInterface(IID_ID3D12Device); a Vitreo D3D12 device cannot be
// wrapped yet -> E_NOTIMPL.
TEST_F(D3D11ManagerTest, D3D11On12CreateDeviceRefusesLikeWindows) {
    constexpr GuestAddr OBJ = DATA_BASE + 0x800;
    constexpr GuestAddr VTBL = DATA_BASE + 0x840;
    constexpr GuestAddr QI = 0x72000100;
    ASSERT_TRUE(memory.write_u64(OBJ, VTBL).is_ok());
    ASSERT_TRUE(memory.write_u64(VTBL, QI).is_ok());
    std::vector<std::array<u8, 16>> qi_iids;
    d3d11.set_guest_function_caller(
        [&](GuestAddr fn, const u64* args, usize n) -> u64 {
            EXPECT_EQ(fn, QI);
            EXPECT_EQ(n, 3u);
            std::array<u8, 16> iid{};
            (void)memory.read_bytes(args[1], reinterpret_cast<Byte*>(iid.data()), 16);
            qi_iids.push_back(iid);
            (void)memory.write_u64(args[2], 0);
            return 0x80004002u;  // E_NOINTERFACE
        });
    Marshaler m(memory);
    m.attach_cpu(cpu);
    auto call = [&](GuestAddr device) {
        (void)memory.write_u64(DEVICE_OUT, 1);
        (void)memory.write_u64(CONTEXT_OUT, 1);
        (void)memory.write_u32(LEVEL_OUT, 0xDEAD);
        cpu.gpr.rcx = device;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = 0;
        cpu.gpr.r9 = 0;
        const GuestAddr sp = cpu.gpr.rsp;
        for (u32 i = 4; i < 10; ++i) (void)memory.write_u64(sp + 8 + i * 8, 0);
        (void)memory.write_u64(sp + 8 + 7 * 8, DEVICE_OUT);
        (void)memory.write_u64(sp + 8 + 8 * 8, CONTEXT_OUT);
        (void)memory.write_u64(sp + 8 + 9 * 8, LEVEL_OUT);
        d3d11.impl_D3D11On12CreateDevice(m);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(call(0), 0x80070057u);
    EXPECT_EQ(memory.read_u64(DEVICE_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u64(CONTEXT_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), 0xDEADu);
    EXPECT_TRUE(qi_iids.empty());

    EXPECT_EQ(call(OBJ), 0x80070057u);
    ASSERT_EQ(qi_iids.size(), 1u);
    // IID_ID3D12Device {189819f1-1db6-4b57-be54-1821339b85f7} (d3d12.h).
    const std::array<u8, 16> iid_d3d12_device{
        0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
        0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7};
    EXPECT_EQ(qi_iids[0], iid_d3d12_device);
    EXPECT_EQ(memory.read_u64(DEVICE_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), 0xDEADu);

    d3d11.set_d3d12_device_query([&](GuestAddr a) { return a == OBJ; });
    EXPECT_EQ(call(OBJ), 0x80004001u);
    EXPECT_EQ(memory.read_u64(DEVICE_OUT).value(), 0u);
}

TEST_F(D3D11ManagerX86Test, ReportsStoreOnlyUavFormatsWithoutTypedLoad) {
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const u32 device = memory.read_u32(DEVICE_OUT).value();
    for (u32 format : {49u, 24u, 26u}) {
        set_arg(0, device);
        set_arg(1, format);
        set_arg(2, static_cast<u32>(LEVEL_OUT));
        d3d11.impl_DeviceMethod(marshaler, 29);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        EXPECT_NE(memory.read_u32(LEVEL_OUT).value() &
                      D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW, 0u);
        for (u32 feature : {2u, 3u}) {
            ASSERT_TRUE(memory.write_u32(LEVEL_OUT, format).is_ok());
            set_arg(0, device);
            set_arg(1, feature);
            set_arg(2, static_cast<u32>(LEVEL_OUT));
            set_arg(3, 8);
            d3d11.impl_DeviceMethod(marshaler, 33);
            ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
            const u32 mask = feature == 2 ?
                D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW :
                D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
            const u32 expected = feature == 2 ? mask : D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
            EXPECT_EQ(memory.read_u32(LEVEL_OUT + 4).value() & mask, expected);
            EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), format);
        }
    }
}

// ID3D11DeviceContext::Map on a buffer: Learn calls RowPitch/DepthPitch the
// "physical size (in bytes) of the data" and gives no number. Measured on
// Windows for every usage and bind class, x86 and x64 --
// tools/oracle_probe/d3d11_buffer_map_pitch_probe.c, output in its .expected:
// both pitches are ByteWidth rounded up to 4, and a rejected Map returns
// E_INVALIDARG, sets pData to NULL and leaves both pitches untouched.
// Vitreo wrote 0 for both pitches, and UE5's D3D11 RHI asserts
// `MappedSubresource.RowPitch >= NumBytes` on every constant-buffer update.
TEST_F(D3D11ManagerTest, BufferMapReportsMeasuredPitchAndNullsDataOnFailure) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr u32 W = 0x10000;  // D3D11_CPU_ACCESS_WRITE
    constexpr u32 R = 0x20000;  // D3D11_CPU_ACCESS_READ
    constexpr u32 kUntouchedRow = 0xAAAAAAAAu;
    constexpr u32 kUntouchedDepth = 0xBBBBBBBBu;
    struct Case {
        const char* name;
        u32 byte_width, usage, bind, cpu_access, map_type;
        u32 expect_hr;
        u32 expect_pitch;  // for failures: pitches stay untouched
    };
    // Rows copied from d3d11_buffer_map_pitch_probe.expected.
    for (const Case& c : {
             Case{"DYNAMIC CB 16", 16, 2, 0x4, W, 4, d3d11::S_OK, 16},
             Case{"DYNAMIC CB 272", 272, 2, 0x4, W, 4, d3d11::S_OK, 272},
             Case{"DYNAMIC CB 65536", 65536, 2, 0x4, W, 4, d3d11::S_OK, 65536},
             Case{"DYNAMIC VB 1000", 1000, 2, 0x1, W, 4, d3d11::S_OK, 1000},
             Case{"DYNAMIC VB 1000 NO_OVERWRITE", 1000, 2, 0x1, W, 5,
                  d3d11::S_OK, 1000},
             Case{"DYNAMIC IB 2", 2, 2, 0x2, W, 4, d3d11::S_OK, 4},
             Case{"DYNAMIC IB 6", 6, 2, 0x2, W, 4, d3d11::S_OK, 8},
             Case{"DYNAMIC IB 1001", 1001, 2, 0x2, W, 4, d3d11::S_OK, 1004},
             Case{"DYNAMIC SRV 6", 6, 2, 0x8, W, 4, d3d11::S_OK, 8},
             Case{"STAGING RW 1 READ", 1, 3, 0, R | W, 1, d3d11::S_OK, 4},
             Case{"STAGING RW 1001 READ_WRITE", 1001, 3, 0, R | W, 3,
                  d3d11::S_OK, 1004},
             Case{"STAGING W 12 WRITE", 12, 3, 0, W, 2, d3d11::S_OK, 12},
             Case{"STAGING R 12 WRITE (illegal)", 12, 3, 0, R, 2,
                  d3d11::E_INVALIDARG, 0},
             Case{"DYNAMIC CB 256 WRITE (illegal)", 256, 2, 0x4, W, 2,
                  d3d11::E_INVALIDARG, 0},
             Case{"DEFAULT VB 64 WRITE_DISCARD (illegal)", 64, 0, 0x1, 0, 4,
                  d3d11::E_INVALIDARG, 0},
         }) {
        SCOPED_TRACE(c.name);
        const std::array<u32, 6> desc{c.byte_width, c.usage, c.bind,
                                      c.cpu_access, 0, 0};
        ASSERT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();

        if (c.map_type == 5) {
            // Establish contents with a discard first, as the probe does.
            set_arg(0, context);
            set_arg(1, buffer);
            set_arg(2, 0);
            set_arg(3, 4);
            set_arg(4, 0);
            set_arg(5, MAPPED_OUT);
            d3d11.impl_ContextMethod(marshaler, 14);
            ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
            set_arg(0, context);
            set_arg(1, buffer);
            set_arg(2, 0);
            d3d11.impl_ContextMethod(marshaler, 15);
        }

        ASSERT_TRUE(memory.write_u64(MAPPED_OUT, 1).is_ok());
        ASSERT_TRUE(memory.write_u32(MAPPED_OUT + 8, kUntouchedRow).is_ok());
        ASSERT_TRUE(memory.write_u32(MAPPED_OUT + 12, kUntouchedDepth).is_ok());
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        set_arg(3, c.map_type);
        set_arg(4, 0);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
        EXPECT_EQ(cpu.gpr.rax, c.expect_hr);
        if (c.expect_hr == d3d11::S_OK) {
            EXPECT_NE(memory.read_u64(MAPPED_OUT).value(), 0u);
            EXPECT_NE(memory.read_u64(MAPPED_OUT).value(), 1u);
            EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), c.expect_pitch);
            EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(),
                      c.expect_pitch);
            set_arg(0, context);
            set_arg(1, buffer);
            set_arg(2, 0);
            d3d11.impl_ContextMethod(marshaler, 15);
        } else {
            EXPECT_EQ(memory.read_u64(MAPPED_OUT).value(), 0u);
            EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), kUntouchedRow);
            EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(),
                      kUntouchedDepth);
        }
    }

    // A rejected TEXTURE map follows the same output rule (probe rows
    // "TEX2D STAGING R, WRITE (illegal)" and "... subresource 5 (illegal)").
    constexpr GuestAddr TEX_DESC = GPU_BUFFER_DESC + 0x700;
    constexpr GuestAddr TEX_OUT = TEX_DESC + 0x40;
    const std::array<u32, 11> tex_desc{16, 16, 1, 1, 28, 1, 0, 3, 0, R, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEX_DESC, reinterpret_cast<const Byte*>(tex_desc.data()),
        sizeof(tex_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEX_DESC);
    set_arg(2, 0);
    set_arg(3, TEX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEX_OUT).value();
    for (const auto& [map_type, subresource] :
         {std::pair<u32, u32>{2, 0}, std::pair<u32, u32>{1, 5}}) {
        SCOPED_TRACE(subresource);
        ASSERT_TRUE(memory.write_u64(MAPPED_OUT, 1).is_ok());
        ASSERT_TRUE(memory.write_u32(MAPPED_OUT + 8, kUntouchedRow).is_ok());
        ASSERT_TRUE(memory.write_u32(MAPPED_OUT + 12, kUntouchedDepth).is_ok());
        set_arg(0, context);
        set_arg(1, texture);
        set_arg(2, subresource);
        set_arg(3, map_type);
        set_arg(4, 0);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
        EXPECT_EQ(memory.read_u64(MAPPED_OUT).value(), 0u);
        EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), kUntouchedRow);
        EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), kUntouchedDepth);
    }
}

// The same contract through the 32-bit D3D11_MAPPED_SUBRESOURCE layout
// { void* pData; UINT RowPitch; UINT DepthPitch; } = offsets 0, 4, 8.
// The probe's i686 build printed output identical to x64.
// d3d11_classlink_probe [get] on x86 (classlink_32.exe matches): the
// ppClassInstances arrays of *SetShader and *GetShader hold 4-byte
// pointers.
TEST_F(D3D11ManagerX86Test, SetShaderClassInstanceArraysUseNativePointers) {
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 device = memory.read_u32(DEVICE_OUT).value();
    const u32 context = memory.read_u32(CONTEXT_OUT).value();
    constexpr GuestAddr OUT = DATA_BASE + 0x400;
    constexpr GuestAddr NAME = DATA_BASE + 0x410;
    constexpr GuestAddr SET = DATA_BASE + 0x440;
    constexpr GuestAddr GOT = DATA_BASE + 0x480;
    constexpr GuestAddr COUNT = DATA_BASE + 0x4C0;
    constexpr GuestAddr BYTECODE = 0x100000;
    ASSERT_TRUE(memory.map_region(BYTECODE, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());

    set_arg(0, device);
    set_arg(1, static_cast<u32>(OUT));
    d3d11.impl_DeviceMethod(marshaler, 19);  // CreateClassLinkage
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 linkage = memory.read_u32(OUT).value();
    ASSERT_TRUE(memory.write_bytes(BYTECODE, classlink_blobs::k_cs_multi,
                                   sizeof(classlink_blobs::k_cs_multi)).is_ok());
    set_arg(0, device);
    set_arg(1, static_cast<u32>(BYTECODE));
    set_arg(2, sizeof(classlink_blobs::k_cs_multi));
    set_arg(3, linkage);
    set_arg(4, static_cast<u32>(OUT));
    d3d11.impl_DeviceMethod(marshaler, 18);  // CreateComputeShader
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 multi = memory.read_u32(OUT).value();

    const auto instance = [&](const char* name, bool create) {
        EXPECT_TRUE(memory.write_bytes(NAME, reinterpret_cast<const Byte*>(name),
                                       std::strlen(name) + 1).is_ok());
        set_arg(0, linkage);
        set_arg(1, static_cast<u32>(NAME));
        set_arg(2, 0);
        if (create) {
            set_arg(3, 0);
            set_arg(4, 0);
            set_arg(5, 0);
            set_arg(6, static_cast<u32>(OUT));
        } else {
            set_arg(3, static_cast<u32>(OUT));
        }
        d3d11.impl_ClassLinkageMethod(marshaler, create ? 8 : 7);
        EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK) << name;
        return memory.read_u32(OUT).value();
    };
    const std::vector<u32> good = {
        instance("XorOp", true), instance("g_add0", false),
        instance("g_mul", false), instance("g_add1", false),
        instance("g_buf", false), instance("g_shl", false)};
    for (usize i = 0; i < good.size(); ++i) {
        ASSERT_TRUE(memory.write_u32(SET + i * 4, good[i]).is_ok());
    }
    set_arg(0, context);
    set_arg(1, multi);
    set_arg(2, static_cast<u32>(SET));
    set_arg(3, 6);
    d3d11.impl_ContextMethod(marshaler, 69);  // CSSetShader

    for (const u32 capacity : {8u, 2u}) {
        for (u32 i = 0; i < 8; ++i) {
            ASSERT_TRUE(memory.write_u32(GOT + i * 4, 0x5A5A5A50u).is_ok());
        }
        ASSERT_TRUE(memory.write_u32(COUNT, capacity).is_ok());
        set_arg(0, context);
        set_arg(1, static_cast<u32>(OUT));
        set_arg(2, static_cast<u32>(GOT));
        set_arg(3, static_cast<u32>(COUNT));
        d3d11.impl_ContextMethod(marshaler, 107);  // CSGetShader
        EXPECT_EQ(memory.read_u32(OUT).value(), multi);
        const u32 count = memory.read_u32(COUNT).value();
        EXPECT_EQ(count, std::min(capacity, 6u));
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_EQ(memory.read_u32(GOT + i * 4).value(),
                      i < count ? good[i] : 0x5A5A5A50u)
                << capacity << " " << i;
        }
    }
}

TEST_F(D3D11ManagerX86Test, BufferMapReportsMeasuredPitchAndNullsDataOnFailure) {
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 device = memory.read_u32(DEVICE_OUT).value();
    const u32 context = memory.read_u32(CONTEXT_OUT).value();

    constexpr GuestAddr DESC = DATA_BASE + 0x400;
    constexpr GuestAddr OUT = DATA_BASE + 0x440;
    constexpr GuestAddr MAPPED = DATA_BASE + 0x460;
    const auto create = [&](u32 byte_width, u32 usage, u32 bind,
                            u32 cpu_access) -> u32 {
        const std::array<u32, 6> desc{byte_width, usage, bind, cpu_access, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, static_cast<u32>(DESC));
        set_arg(2, 0);
        set_arg(3, static_cast<u32>(OUT));
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
        return memory.read_u32(OUT).value();
    };
    const auto map = [&](u32 buffer, u32 map_type) -> u32 {
        EXPECT_TRUE(memory.write_u32(MAPPED, 1).is_ok());
        EXPECT_TRUE(memory.write_u32(MAPPED + 4, 0xAAAAAAAAu).is_ok());
        EXPECT_TRUE(memory.write_u32(MAPPED + 8, 0xBBBBBBBBu).is_ok());
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        set_arg(3, map_type);
        set_arg(4, 0);
        set_arg(5, static_cast<u32>(MAPPED));
        d3d11.impl_ContextMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax & 0xffffffffu);
    };

    // "DYNAMIC CB 272 map4 -> RowPitch=272 DepthPitch=272".
    const u32 cb = create(272, 2, 0x4, 0x10000);
    ASSERT_EQ(map(cb, 4), d3d11::S_OK);
    EXPECT_NE(memory.read_u32(MAPPED).value(), 0u);
    EXPECT_NE(memory.read_u32(MAPPED).value(), 1u);
    EXPECT_EQ(memory.read_u32(MAPPED + 4).value(), 272u);
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 272u);
    set_arg(0, context);
    set_arg(1, cb);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    // "DYNAMIC IB 6 map4 -> RowPitch=8 DepthPitch=8".
    const u32 ib = create(6, 2, 0x2, 0x10000);
    ASSERT_EQ(map(ib, 4), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(MAPPED + 4).value(), 8u);
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 8u);
    set_arg(0, context);
    set_arg(1, ib);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    // "DYNAMIC CB 256 WRITE (illegal) map2 -> 0x80070057 pData=NULL",
    // pitches untouched.
    const u32 cb2 = create(256, 2, 0x4, 0x10000);
    EXPECT_EQ(map(cb2, 2), d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u32(MAPPED).value(), 0u);
    EXPECT_EQ(memory.read_u32(MAPPED + 4).value(), 0xAAAAAAAAu);
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 0xBBBBBBBBu);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
