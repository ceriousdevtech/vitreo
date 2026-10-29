// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_metal_fixture.hpp
/// @brief The base fixture D3D11MetalTest: the D3D11 front-end on the real
///        Metal backend, driven through the guest entry points.
///
/// Split from the former test_d3d11_metal.cpp; every suite of the
/// vitreo_d3d11_metal_tests target derives from it.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"

#include <array>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace vitreo::winapi::d3d11_test {

inline constexpr u32 kSide = 32;

class D3D11MetalTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        graphics::set_active_backend_device(&graphics::MetalDevice::instance());
        handles.set_close_callback([](Handle, HandleType type, void* object) {
            if (type == HandleType::SharedResource) {
                release_shared_resource_handle_object(object);
            }
            return true;
        });
        d3d11.set_handle_table(&handles);
        ASSERT_TRUE(memory.map_region(DATA_BASE, DATA_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK_BASE + 0x800;
        // Every interface's methods at distinct (never executed) addresses.
        GuestAddr thunk = 0x70000010;
        DxgiManager::MethodThunks dxgi_thunks{};
        fill(dxgi_thunks, thunk);
        dxgi.set_method_thunks(dxgi_thunks);
        D3D11Manager::MethodThunks d3d11_thunks{};
        fill(d3d11_thunks, thunk);
        d3d11.set_method_thunks(d3d11_thunks);
        marshaler.attach_cpu(cpu);

        for (usize index = 0; index < 10; ++index) set_arg(index, 0);
        set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
        set_arg(6, d3d11::SDK_VERSION);
        set_arg(7, DEVICE_OUT);
        set_arg(8, LEVEL_OUT);
        set_arg(9, CONTEXT_OUT);
        d3d11.impl_D3D11CreateDevice(marshaler);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        ASSERT_NE(device, 0u);
        ASSERT_NE(context, 0u);
    }

    template <typename Thunks>
    static void fill(Thunks& thunks, GuestAddr& next) {
        static_assert(sizeof(Thunks) % sizeof(GuestAddr) == 0,
                      "MethodThunks must be a homogeneous block of GuestAddr");
        auto* fields = reinterpret_cast<GuestAddr*>(&thunks);
        for (usize i = 0; i < sizeof(Thunks) / sizeof(GuestAddr); ++i) {
            fields[i] = next;
            next += 0x10;
        }
    }

    void set_arg(usize index, u64 value) {
        switch (index) {
        case 0: cpu.gpr.rcx = value; break;
        case 1: cpu.gpr.rdx = value; break;
        case 2: cpu.gpr.r8 = value; break;
        case 3: cpu.gpr.r9 = value; break;
        default:
            ASSERT_TRUE(memory.write_u64(
                cpu.gpr.rsp + 0x28 + (index - 4) * 8, value).is_ok());
            break;
        }
    }

    u64 device_call(usize slot, std::initializer_list<u64> args) {
        set_arg(0, device);
        usize index = 1;
        for (const u64 arg : args) set_arg(index++, arg);
        d3d11.impl_DeviceMethod(marshaler, slot);
        return cpu.gpr.rax;
    }
    u64 context_call(usize slot, std::initializer_list<u64> args) {
        set_arg(0, context);
        usize index = 1;
        for (const u64 arg : args) set_arg(index++, arg);
        d3d11.impl_ContextMethod(marshaler, slot);
        return cpu.gpr.rax;
    }

    /// Bump-allocated guest memory holding @p bytes.
    GuestAddr put(const void* bytes, usize size) {
        const GuestAddr at = m_next;
        m_next += (size + 15) & ~usize{15};
        EXPECT_LE(m_next, DATA_BASE + DATA_SIZE);
        EXPECT_TRUE(memory.write_bytes(at, static_cast<const Byte*>(bytes),
                                       size).is_ok());
        return at;
    }
    template <typename T, usize N>
    GuestAddr put(const std::array<T, N>& values) {
        return put(values.data(), sizeof(T) * N);
    }
    GuestAddr out_slot() {
        const u64 zero = 0;
        return put(&zero, sizeof(zero));
    }

    /// ID3D11Device::Create{Vertex,Geometry,Pixel}Shader (slots 12, 13, 15).
    GuestAddr shader(usize slot, const unsigned char* blob, usize size) {
        const GuestAddr code = put(blob, size);
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(slot, {code, size, 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    /// A kSide^3 R10G10B10A2_UNORM Texture3D (RENDER_TARGET | SHADER_RESOURCE)
    /// and its RTV over every W slice (FirstWSlice 0, WSize -1).
    void make_lut(GuestAddr& volume, GuestAddr& rtv) {
        // D3D11_TEXTURE3D_DESC.
        const std::array<u32, 9> desc{kSide, kSide, kSide, 1, kFormat,
                                      0, 0x20 | 0x8, 0, 0};
        GuestAddr out = out_slot();
        ASSERT_EQ(device_call(6, {put(desc), 0, out}), d3d11::S_OK);
        volume = memory.read_u64(out).value();
        // D3D11_RENDER_TARGET_VIEW_DESC: TEXTURE3D (8), MipSlice 0,
        // FirstWSlice 0, WSize -1.
        const std::array<u32, 5> view{kFormat, 8, 0, 0, 0xFFFFFFFFu};
        out = out_slot();
        ASSERT_EQ(device_call(9, {volume, put(view), out}), d3d11::S_OK);
        rtv = memory.read_u64(out).value();
        const std::array<float, 4> black{0, 0, 0, 0};
        context_call(50, {rtv, put(black)});  // ClearRenderTargetView
    }

    /// Bind the LUT bake's state: @p vs, the GS and PS, the RTV, a 32x32
    /// viewport, TRIANGLESTRIP, no culling (as UE).
    void bind_bake(GuestAddr vs, GuestAddr gs, GuestAddr ps, GuestAddr rtv) {
        // D3D11_RASTERIZER_DESC: SOLID, CULL_NONE, depth clip on.
        const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
        context_call(43, {memory.read_u64(out).value()});  // RSSetState
        const std::array<float, 6> viewport{0, 0, kSide, kSide, 0, 1};
        context_call(44, {1, put(viewport)});           // RSSetViewports
        context_call(24, {5});  // IASetPrimitiveTopology TRIANGLESTRIP
        context_call(11, {vs, 0, 0});                   // VSSetShader
        context_call(23, {gs, 0, 0});                   // GSSetShader
        context_call(9, {ps, 0, 0});                    // PSSetShader
        const std::array<u64, 1> rtvs{rtv};
        context_call(33, {1, put(rtvs), 0});            // OMSetRenderTargets
    }

    /// ID3D11Device::CreateInputLayout over @p elements, each
    /// {semantic, index, DXGI format, offset, classification, step rate} in
    /// input slot 0, against @p vs_code's input signature.
    struct Element {
        const char* semantic;
        u32 index, format, offset, classification, step;
    };
    GuestAddr input_layout(std::initializer_list<Element> elements,
                           const unsigned char* vs_code, usize vs_size) {
        std::vector<u8> descs;
        for (const Element& e : elements) {
            const GuestAddr name = put(e.semantic, std::strlen(e.semantic) + 1);
            std::array<u8, 32> d{};  // D3D11_INPUT_ELEMENT_DESC (x64)
            std::memcpy(d.data(), &name, 8);
            const std::array<u32, 6> fields{e.index, e.format, 0, e.offset,
                                            e.classification, e.step};
            std::memcpy(d.data() + 8, fields.data(), sizeof(fields));
            descs.insert(descs.end(), d.begin(), d.end());
        }
        const GuestAddr array = put(descs.data(), descs.size());
        const GuestAddr code = put(vs_code, vs_size);
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(11, {array, elements.size(), code, vs_size, out}),
                  d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    /// A DEFAULT vertex buffer holding @p bytes, bound to slot 0 with
    /// @p stride.
    void vertex_buffer(const void* bytes, u32 size, u32 stride) {
        // D3D11_BUFFER_DESC { ByteWidth, DEFAULT, VERTEX_BUFFER, 0, 0, 0 },
        // D3D11_SUBRESOURCE_DATA { pSysMem, 0, 0 }.
        const std::array<u32, 6> bd{size, 0, 1, 0, 0, 0};
        const GuestAddr init = put(std::array<u64, 2>{put(bytes, size), 0});
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(3, {put(bd), init, out}), d3d11::S_OK);
        const std::array<u64, 1> buffers{memory.read_u64(out).value()};
        const std::array<u32, 1> strides{stride};
        const std::array<u32, 1> offsets{0};
        context_call(18, {0, 1, put(buffers), put(strides), put(offsets)});
    }

    /// Copy @p volume into a STAGING twin, Map it for READ and check every
    /// slice against the spec's values.
    void expect_lut(GuestAddr volume) {
        const std::array<u32, 9> desc{kSide, kSide, kSide, 1, kFormat,
                                      3 /* STAGING */, 0, 0x20000 /* READ */,
                                      0};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(6, {put(desc), 0, out}), d3d11::S_OK);
        const GuestAddr staging = memory.read_u64(out).value();
        context_call(47, {staging, volume});  // CopyResource
        // D3D11_MAPPED_SUBRESOURCE { pData, RowPitch, DepthPitch }.
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        ASSERT_EQ(context_call(14, {staging, 0, 1 /* READ */, 0, mapped}),
                  d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        const u32 row_pitch = memory.read_u32(mapped + 8).value();
        const u32 depth_pitch = memory.read_u32(mapped + 12).value();
        ASSERT_NE(data, 0u);
        const auto unorm10 = [](double v) {
            return static_cast<u32>(v * 1023.0 + 0.5);
        };
        for (u32 z = 0; z < kSide; ++z) {
            usize wrong = 0;
            usize zero = 0;
            u32 first_wrong = 0;
            for (u32 y = 0; y < kSide; ++y) {
                for (u32 x = 0; x < kSide; ++x) {
                    const u32 texel =
                        memory.read_u32(data + z * depth_pitch +
                                        y * row_pitch + x * 4).value();
                    const u32 r = texel & 0x3FFu;
                    const u32 g = (texel >> 10) & 0x3FFu;
                    const u32 b = (texel >> 20) & 0x3FFu;
                    const u32 er = unorm10((x + 0.5) / kSide);
                    const u32 eg = unorm10((y + 0.5) / kSide);
                    const bool ok = b == 33u * z && texel >> 30 == 3u &&
                                    r + 1 >= er && r <= er + 1 &&
                                    g + 1 >= eg && g <= eg + 1;
                    if (!ok && wrong++ == 0) first_wrong = texel;
                    zero += texel == 0 ? 1u : 0u;
                }
            }
            EXPECT_EQ(wrong, 0u)
                << "W slice " << z << ": B must be " << 33u * z
                << ", first wrong texel 0x" << std::hex << first_wrong;
            EXPECT_EQ(zero, 0u) << "W slice " << z;
        }
        context_call(15, {staging, 0});  // Unmap
    }

    static constexpr u32 kFormat = 24;  // DXGI_FORMAT_R10G10B10A2_UNORM
    static constexpr GuestAddr DATA_BASE = 0x100000;
    static constexpr usize DATA_SIZE = 0x40000;
    static constexpr GuestAddr DEVICE_OUT = DATA_BASE + 0x00;
    static constexpr GuestAddr LEVEL_OUT = DATA_BASE + 0x08;
    static constexpr GuestAddr CONTEXT_OUT = DATA_BASE + 0x10;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
    DxgiManager dxgi{
        memory, true,
        dxgi::AdapterInfo{
            .description = "Metal GPU",
            .vendor_id = dxgi::VENDOR_ID_APPLE,
            .dedicated_video_memory = 8ull * 1024 * 1024 * 1024,
        }};
    HandleTable handles;
    D3D11Manager d3d11{memory, true, graphics::MetalDevice::instance(), dxgi};
    Marshaler marshaler{memory};
    GuestAddr device{0};
    GuestAddr context{0};
    GuestAddr m_next{DATA_BASE + 0x100};
};

}  // namespace vitreo::winapi::d3d11_test
