// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_d3d10.hpp
/// @brief D3D10 device fixtures (D3D10DeviceTest, D3D10DeviceX86Test), the vtable wiring helper and the D3D10 oracle blobs.

#include "d3d11_test_fixture_x86.hpp"

namespace d3d10_blobs {
#include "../../oracle_data/d3d10_contract_shaders.h"
}  // namespace d3d10_blobs

namespace vitreo::winapi::d3d11_test {

// =============================================================================
// Direct3D 10 (winapi/graphics/d3d10/): the device and the D3D10 facets of
// D3D11Manager's objects, driven through the vtables a guest reads. Every
// expected value is Windows' (tools/oracle_probe/d3d10_contract_probe64 /
// 32.expected, Windows) unless a comment names Learn.
// =============================================================================

using D3D10Dispatch = void (D3D11Manager::*)(Marshaler&, usize);
struct D3D10Target {
    D3D10Dispatch method;
    usize slot;
};

/// Give every D3D11 and D3D10 vtable slot a thunk address and remember which
/// dispatcher each one reaches, so a test can call through the vtable in
/// guest memory the way a guest does.
inline void wire_every_vtable(D3D11Manager::MethodThunks& t, GuestAddr next,
                       std::map<GuestAddr, D3D10Target>& targets) {
    const auto wire = [&](auto& array, D3D10Dispatch method) {
        for (usize i = 0; i < array.size(); ++i) {
            array[i] = next;
            targets[next] = {method, i};
            next += 0x10;
        }
    };
    wire(t.device, &D3D11Manager::impl_DeviceMethod);
    wire(t.context, &D3D11Manager::impl_ContextMethod);
    wire(t.dxgi_device, &D3D11Manager::impl_DxgiDeviceMethod);
    wire(t.buffer, &D3D11Manager::impl_BufferMethod);
    wire(t.texture_2d, &D3D11Manager::impl_Texture2DMethod);
    wire(t.dxgi_resource, &D3D11Manager::impl_DxgiResourceMethod);
    wire(t.keyed_mutex, &D3D11Manager::impl_KeyedMutexMethod);
    wire(t.shader_resource_view,
         &D3D11Manager::impl_ShaderResourceViewMethod);
    wire(t.unordered_access_view,
         &D3D11Manager::impl_UnorderedAccessViewMethod);
    wire(t.render_target_view, &D3D11Manager::impl_RenderTargetViewMethod);
    wire(t.depth_stencil_view, &D3D11Manager::impl_DepthStencilViewMethod);
    wire(t.query, &D3D11Manager::impl_QueryMethod);
    wire(t.fence, &D3D11Manager::impl_FenceMethod);
    wire(t.command_list, &D3D11Manager::impl_CommandListMethod);
    wire(t.swap_chain, &D3D11Manager::impl_SwapChainMethod);
    wire(t.swap_chain_media, &D3D11Manager::impl_SwapChainMediaMethod);
    wire(t.annotation, &D3D11Manager::impl_AnnotationMethod);
    wire(t.vertex_shader, &D3D11Manager::impl_VertexShaderMethod);
    wire(t.pixel_shader, &D3D11Manager::impl_PixelShaderMethod);
    wire(t.compute_shader, &D3D11Manager::impl_ComputeShaderMethod);
    wire(t.geometry_shader, &D3D11Manager::impl_GeometryShaderMethod);
    wire(t.hull_shader, &D3D11Manager::impl_HullShaderMethod);
    wire(t.domain_shader, &D3D11Manager::impl_DomainShaderMethod);
    wire(t.dxgi_surface, &D3D11Manager::impl_DxgiSurfaceMethod);
    wire(t.device_context_state,
         &D3D11Manager::impl_DeviceContextStateMethod);
    wire(t.input_layout, &D3D11Manager::impl_InputLayoutMethod);
    wire(t.blend_state, &D3D11Manager::impl_BlendStateMethod);
    wire(t.rasterizer_state, &D3D11Manager::impl_RasterizerStateMethod);
    wire(t.depth_stencil_state, &D3D11Manager::impl_DepthStencilStateMethod);
    wire(t.sampler_state, &D3D11Manager::impl_SamplerStateMethod);
    wire(t.video_context, &D3D11Manager::impl_VideoContextMethod);
    wire(t.multithread, &D3D11Manager::impl_MultithreadMethod);
    wire(t.class_linkage, &D3D11Manager::impl_ClassLinkageMethod);
    wire(t.class_instance, &D3D11Manager::impl_ClassInstanceMethod);
    for (const D3D10VtableSpec& spec : d3d10_vtable_specs()) {
        GuestAddr* thunks = spec.thunks(t);
        for (usize i = 0; i < spec.slots; ++i) {
            thunks[i] = next;
            targets[next] = {spec.method, i};
            next += 0x10;
        }
    }
}

// ID3D10Device1 vtable slots (d3d10_1.h ID3D10Device1Vtbl).
constexpr usize kD3D10VSSetConstantBuffers = 3, kD3D10PSSetShader = 5,
                kD3D10VSSetShader = 7, kD3D10IASetPrimitiveTopology = 18,
                kD3D10SOSetTargets = 27, kD3D10RSSetViewports = 30,
                kD3D10ClearDepthStencilView = 36,
                kD3D10VSGetConstantBuffers = 39, kD3D10VSGetShader = 43,
                kD3D10IAGetPrimitiveTopology = 50, kD3D10SOGetTargets = 59,
                kD3D10RSGetViewports = 61, kD3D10ClearState = 69,
                kD3D10CreateBuffer = 71, kD3D10CreateTexture2D = 73,
                kD3D10CreateDepthStencilView = 77,
                kD3D10CreateVertexShader = 79,
                kD3D10CreateGeometryShaderWithStreamOutput = 81,
                kD3D10CreatePixelShader = 82, kD3D10CreateBlendState = 83,
                kD3D10CreateQuery = 87, kD3D10CreatePredicate = 88,
                kD3D10CreateCounter = 89, kD3D10CheckCounter = 93,
                kD3D10GetCreationFlags = 94, kD3D10SetTextFilterSize = 96,
                kD3D10GetTextFilterSize = 97, kD3D10GetFeatureLevel = 100;

class D3D10DeviceTest : public D3D11ManagerTest {
protected:
    void SetUp() override {
        D3D11ManagerTest::SetUp();
        ASSERT_TRUE(memory.map_region(BASE, 0x8000,
                                      MemoryProtection::ReadWrite).is_ok());
        D3D11Manager::MethodThunks thunks{};
        wire_every_vtable(thunks, 0x7A000000, targets);
        d3d11.set_method_thunks(thunks);
        marshaler.attach_cpu(cpu);
    }

    /// Call slot @p slot of @p iface through the vtable in guest memory.
    /// Arguments after `this` go where the x64 ABI puts them, a float also in
    /// its XMM register.
    u64 vcall(GuestAddr iface, usize slot, std::initializer_list<u64> args) {
        const auto vtable = memory.read_u64(iface);
        EXPECT_TRUE(vtable.is_ok() && vtable.value() != 0)
            << "no vtable at 0x" << std::hex << iface;
        if (!vtable.is_ok() || vtable.value() == 0) return 0xBADu;
        const GuestAddr entry = memory.read_u64(vtable.value() + slot * 8).value();
        const auto target = targets.find(entry);
        EXPECT_NE(target, targets.end()) << "slot " << slot << " is not wired";
        if (target == targets.end()) return 0xBADu;
        EXPECT_EQ(target->second.slot, slot);
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, iface);
        usize index = 1;
        for (const u64 value : args) {
            set_arg(index, value);
            if (index < 4) cpu.sse.xmm[index].u32_[0] = static_cast<u32>(value);
            ++index;
        }
        cpu.gpr.rax = 0xDEADBEEFull;
        (d3d11.*target->second.method)(marshaler, target->second.slot);
        return cpu.gpr.rax;
    }

    u64 module(const std::function<void(Marshaler&)>& entry,
               std::initializer_list<u64> args) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        usize index = 0;
        for (const u64 value : args) set_arg(index++, value);
        cpu.gpr.rax = 0xDEADBEEFull;
        entry(marshaler);
        return cpu.gpr.rax;
    }
    u64 module(void (D3D11Manager::*entry)(Marshaler&),
               std::initializer_list<u64> args) {
        return module([&](Marshaler& m) { (d3d11.*entry)(m); }, args);
    }
    u64 signature(bool input, bool output, std::initializer_list<u64> args) {
        return module(
            [&](Marshaler& m) {
                d3d11.impl_D3D10GetSignatureBlob(m, input, output);
            },
            args);
    }
    u64 profile(char stage) {
        return module(
            [&](Marshaler& m) { d3d11.impl_D3D10GetShaderProfile(m, stage); },
            {0});
    }

    /// D3D10CreateDevice (10.0 API) or D3D10CreateDevice1 at @p level.
    GuestAddr create10(bool api_10_1, u32 flags = 0, u32 level = 0xA100u) {
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        const u64 hr =
            api_10_1 ? module(&D3D11Manager::impl_D3D10CreateDevice1,
                              {0, 0, 0, flags, level, 0x20, OUT})
                     : module(&D3D11Manager::impl_D3D10CreateDevice,
                              {0, 0, 0, flags, 29, OUT});
        EXPECT_EQ(hr, 0u);
        return memory.read_u64(OUT).value();
    }

    /// QueryInterface(@p iid); the interface, or 0 with @p hr the result.
    GuestAddr qi(GuestAddr object, const GuidBytes& iid, u64* hr = nullptr) {
        EXPECT_TRUE(memory.write_bytes(IID_AT, iid.data(), iid.size()).is_ok());
        EXPECT_TRUE(memory.write_u64(QI_OUT, 0x5A5A5A50ull).is_ok());
        const u64 result = vcall(object, 0, {IID_AT, QI_OUT});
        if (hr != nullptr) *hr = result;
        return memory.read_u64(QI_OUT).value();
    }
    u64 add_ref(GuestAddr object) { return vcall(object, 1, {}); }
    u64 release(GuestAddr object) { return vcall(object, 2, {}); }

    void words(GuestAddr at, std::initializer_list<u32> values) {
        for (const u32 value : values) {
            ASSERT_TRUE(memory.write_u32(at, value).is_ok());
            at += 4;
        }
    }
    u32 word(GuestAddr at) { return memory.read_u32(at).value(); }
    GuestAddr pointer(GuestAddr at) { return memory.read_u64(at).value(); }

    /// A D3D10 buffer: D3D10_BUFFER_DESC {ByteWidth, Usage, BindFlags,
    /// CPUAccessFlags, MiscFlags}.
    GuestAddr buffer10(GuestAddr device, u32 bytes, u32 usage, u32 bind,
                       u32 cpu_access = 0, u32 misc = 0, u64* hr = nullptr) {
        words(DESC, {bytes, usage, bind, cpu_access, misc});
        EXPECT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
        const u64 result = vcall(device, kD3D10CreateBuffer, {DESC, 0, OBJ_OUT});
        if (hr != nullptr) *hr = result;
        return pointer(OBJ_OUT);
    }
    /// A D3D10 Texture2D; D3D10_TEXTURE2D_DESC is D3D11's layout.
    GuestAddr texture10(GuestAddr device, u32 width, u32 array, u32 format,
                        u32 bind, u32 misc = 0, u64* hr = nullptr,
                        u32 usage = 0, u32 cpu_access = 0) {
        words(DESC, {width, width, 1, array, format, 1, 0, usage, bind,
                     cpu_access, misc});
        EXPECT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
        const u64 result =
            vcall(device, kD3D10CreateTexture2D, {DESC, 0, OBJ_OUT});
        if (hr != nullptr) *hr = result;
        return pointer(OBJ_OUT);
    }
    GuestAddr shader10(GuestAddr device, usize slot, const unsigned char* code,
                       usize size, u64* hr = nullptr) {
        EXPECT_TRUE(memory.write_bytes(CODE, code, size).is_ok());
        EXPECT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
        const u64 result = vcall(device, slot, {CODE, size, OBJ_OUT});
        if (hr != nullptr) *hr = result;
        return pointer(OBJ_OUT);
    }

    static constexpr GuestAddr BASE = 0x20000;
    static constexpr GuestAddr OUT = BASE + 0x000;
    static constexpr GuestAddr QI_OUT = BASE + 0x010;
    static constexpr GuestAddr OBJ_OUT = BASE + 0x020;
    static constexpr GuestAddr IID_AT = BASE + 0x040;
    static constexpr GuestAddr PTRS = BASE + 0x080;
    static constexpr GuestAddr U32S = BASE + 0x100;
    static constexpr GuestAddr DESC = BASE + 0x200;
    static constexpr GuestAddr GET = BASE + 0x400;
    static constexpr GuestAddr NAMES = BASE + 0x600;
    static constexpr GuestAddr CODE = BASE + 0x1000;
    static constexpr u32 kSOk = 0, kSFalse = 1, kEInvalidArg = 0x80070057u,
                         kENoInterface = 0x80004002u, kEFail = 0x80004005u;

    std::map<GuestAddr, D3D10Target> targets;
    Marshaler marshaler{memory};
};

// The x86 frames and structure layouts (pointer-size fields, stdcall
// arguments on the stack, the float on the stack).
class D3D10DeviceX86Test : public D3D11ManagerX86Test {
protected:
    void SetUp() override {
        D3D11ManagerX86Test::SetUp();
        ASSERT_TRUE(memory.map_region(BASE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        D3D11Manager::MethodThunks thunks{};
        wire_every_vtable(thunks, 0x7A000000, targets);
        d3d11.set_method_thunks(thunks);
        marshaler.attach_cpu(cpu);
    }
    u32 vcall(u32 iface, usize slot, std::initializer_list<u32> args) {
        const u32 vtable = memory.read_u32(iface).value();
        const u32 entry = memory.read_u32(vtable + slot * 4).value();
        const auto target = targets.find(entry);
        EXPECT_NE(target, targets.end()) << "slot " << slot;
        if (target == targets.end()) return 0xBADu;
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, iface);
        usize index = 1;
        for (const u32 value : args) set_arg(index++, value);
        cpu.gpr.rax = 0xDEADBEEFull;
        (d3d11.*target->second.method)(marshaler, target->second.slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void words(u32 at, std::initializer_list<u32> values) {
        for (const u32 value : values) {
            ASSERT_TRUE(memory.write_u32(at, value).is_ok());
            at += 4;
        }
    }
    u32 word(u32 at) { return memory.read_u32(at).value(); }

    static constexpr u32 BASE = 0x30000;
    static constexpr u32 OUT = BASE, DESC = BASE + 0x100, GET = BASE + 0x200,
                         NAMES = BASE + 0x300, CODE = BASE + 0x1000;
    std::map<GuestAddr, D3D10Target> targets;
    Marshaler marshaler{memory};
};

}  // namespace vitreo::winapi::d3d11_test
