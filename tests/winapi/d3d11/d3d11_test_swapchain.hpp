// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_swapchain.hpp
/// @brief DxgiSwapChainContractTest and the HRESULT constants the swap-chain suites compare against.

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// DXGI swap-chain contract (area 11 of the 2026-09-28 D3D11 audit). Every
// expected value is a line of tools/oracle_probe/dxgi_a11_contracts_probe
// {64,32}.expected or dxgi_a11_swapchain_rules_probe{64,32}.expected, both
// measured on Windows 11; the probe line is named next to the assertion.
// ---------------------------------------------------------------------------

constexpr u32 kIC = 0x887A0001u;          // DXGI_ERROR_INVALID_CALL
constexpr u32 kOccluded = 0x087A0001u;    // DXGI_STATUS_OCCLUDED
constexpr u32 kStillDrawing = 0x887A000Au;
constexpr u32 kNotFoundDxgi = 0x887A0002u;
constexpr u32 kNotCurrentlyAvailable = 0x887A0022u;
constexpr u32 kAccessDenied = 0x80070005u;
constexpr u32 kOutOfMemory = 0x8007000Eu;

class DxgiSwapChainContractTest : public D3D11ManagerTest {
protected:
    static constexpr u64 kWindow = 0xCAFE;
    static constexpr u64 kMissingWindow = 0xDEAD;
    static constexpr GuestAddr DESC = DATA_BASE + 0x1800;
    static constexpr GuestAddr OUT = DATA_BASE + 0x1900;
    static constexpr GuestAddr OUT2 = DATA_BASE + 0x1910;
    static constexpr GuestAddr IID = DATA_BASE + 0x1920;
    static constexpr GuestAddr SCRATCH = DATA_BASE + 0x1A00;

    void SetUp() override {
        D3D11ManagerTest::SetUp();
        D3D11Manager::WindowServices window;
        window.client_size = [this](u64 hwnd)
            -> std::optional<std::pair<u32, u32>> {
            if (hwnd == kMissingWindow) return std::nullopt;
            return std::pair<u32, u32>{client_w, client_h};
        };
        window.resize_client = [this](u64 hwnd, u32 w, u32 h) {
            resized_hwnd = hwnd;
            client_w = w;
            client_h = h;
            return true;
        };
        window.window_rect = [](u64) -> std::optional<std::array<i32, 4>> {
            return std::array<i32, 4>{10, 10, 210, 160};
        };
        d3d11.set_window_services(window);
        D3D11Manager::SyncServices sync;
        sync.create_semaphore = [this](i32 initial, i32 maximum) -> u64 {
            semaphore_count = initial;
            semaphore_max = maximum;
            return 0x5E4A;
        };
        sync.duplicate_handle = [this](u64 handle) -> u64 {
            return handle + 0x100 * ++duplicates;
        };
        sync.release_semaphore = [this](u64, i32 count, i32 cap) {
            semaphore_count = std::min(semaphore_count + count,
                                       std::max(cap, semaphore_count));
        };
        sync.close_handle = [this](u64 handle) { closed.push_back(handle); };
        d3d11.set_sync_services(sync);
        create_default_device();
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        ASSERT_TRUE(memory.write_bytes(
            IID, reinterpret_cast<const Byte*>(&IID_IDXGIFACTORY1),
            sizeof(IID_IDXGIFACTORY1)).is_ok());
        set_arg(0, IID);
        set_arg(1, FACTORY_OUT);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        dxgi.impl_CreateDXGIFactory1(marshaler);
        factory = memory.read_u64(FACTORY_OUT).value();
        ASSERT_NE(factory, 0u);
    }

    struct Desc {
        u32 width{64}, height{48}, format{87}, samples{1}, quality{0};
        u32 usage{0x20}, count{1}, windowed{1}, effect{0}, flags{0};
        u64 hwnd{kWindow};
    };

    // DXGI_SWAP_CHAIN_DESC, x64 layout (HWND at 48).
    u32 create(const Desc& d, GuestAddr* chain = nullptr,
               GuestAddr device_arg = 0) {
        const std::array<u32, 11> prefix{d.width, d.height, 0, 0, d.format,
                                         0, 0, d.samples, d.quality,
                                         d.usage, d.count};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(prefix.data()),
            sizeof(prefix)).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 44, 0).is_ok());
        EXPECT_TRUE(memory.write_u64(DESC + 48, d.hwnd).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 56, d.windowed).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 60, d.effect).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 64, d.flags).is_ok());
        EXPECT_TRUE(memory.write_u64(SWAP_OUT, 0x1111).is_ok());
        set_arg(0, factory);
        set_arg(1, device_arg == 0 ? device : device_arg);
        set_arg(2, DESC);
        set_arg(3, SWAP_OUT);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_CreateSwapChain(marshaler);
        const u32 hr = static_cast<u32>(cpu.gpr.rax);
        if (chain != nullptr) *chain = memory.read_u64(SWAP_OUT).value();
        return hr;
    }

    // DXGI_SWAP_CHAIN_DESC1 through CreateSwapChainForHwnd / Composition.
    u32 create1(const Desc& d, u32 scaling, u32 alpha, bool composition,
                GuestAddr* chain = nullptr, u32 stereo = 0) {
        const std::array<u32, 12> desc1{d.width, d.height, d.format, stereo,
                                        d.samples, d.quality, d.usage,
                                        d.count, scaling, d.effect, alpha,
                                        d.flags};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc1.data()),
            sizeof(desc1)).is_ok());
        EXPECT_TRUE(memory.write_u64(SWAP_OUT, 0x1111).is_ok());
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        set_arg(0, factory);
        set_arg(1, device);
        if (composition) {
            set_arg(2, DESC);
            set_arg(3, 0);
            set_arg(4, SWAP_OUT);
            d3d11.impl_CreateSwapChainForComposition(marshaler);
        } else {
            set_arg(2, d.hwnd);
            set_arg(3, DESC);
            set_arg(4, 0);
            set_arg(5, 0);
            set_arg(6, SWAP_OUT);
            d3d11.impl_CreateSwapChainForHwnd(marshaler);
        }
        if (chain != nullptr) *chain = memory.read_u64(SWAP_OUT).value();
        return static_cast<u32>(cpu.gpr.rax);
    }

    u32 call(GuestAddr chain, usize slot, std::initializer_list<u64> args) {
        set_arg(0, chain);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_SwapChainMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }

    void write_iid(const GuidBytes& iid) {
        ASSERT_TRUE(memory.write_bytes(
            IID, reinterpret_cast<const Byte*>(iid.data()), iid.size())
                        .is_ok());
    }
    void write_iid(const Guid& iid) {
        ASSERT_TRUE(memory.write_bytes(
            IID, reinterpret_cast<const Byte*>(&iid), sizeof(iid)).is_ok());
    }

    void release_view(GuestAddr view) {
        if (view == 0) return;
        set_arg(0, view);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_RenderTargetViewMethod(marshaler, 2);
    }
    void release_texture(GuestAddr texture) {
        if (texture == 0) return;
        set_arg(0, texture);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_Texture2DMethod(marshaler, 2);
    }
    GuestAddr make_rtv(GuestAddr texture) {
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, VIEW_OUT);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_DeviceMethod(marshaler, 9);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(VIEW_OUT).value();
    }
    void bind_rtv(GuestAddr rtv) {
        EXPECT_TRUE(memory.write_u64(SCRATCH + 0x100, rtv).is_ok());
        set_arg(0, context);
        set_arg(1, 1);
        set_arg(2, SCRATCH + 0x100);
        set_arg(3, 0);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 33);  // OMSetRenderTargets
    }
    GuestAddr bound_rtv() {
        EXPECT_TRUE(memory.write_u64(SCRATCH + 0x108, 0).is_ok());
        set_arg(0, context);
        set_arg(1, 1);
        set_arg(2, SCRATCH + 0x108);
        set_arg(3, 0);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 89);  // OMGetRenderTargets
        return memory.read_u64(SCRATCH + 0x108).value();
    }
    void clear_state() {
        set_arg(0, context);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 110);  // ClearState
    }

    u32 present_count(GuestAddr chain) {
        EXPECT_EQ(call(chain, 17, {OUT2}), d3d11::S_OK);
        return memory.read_u32(OUT2).value();
    }

    GuestAddr device{0};
    GuestAddr context{0};
    GuestAddr factory{0};
    u32 client_w{200};
    u32 client_h{150};
    u64 resized_hwnd{0};
    i32 semaphore_count{-1};
    i32 semaphore_max{0};
    u32 duplicates{0};
    std::vector<u64> closed;
};

}  // namespace vitreo::winapi::d3d11_test
