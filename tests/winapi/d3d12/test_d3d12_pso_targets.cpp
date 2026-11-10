// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_pso_targets.cpp
/// @brief Pixel shader outputs with no render target behind them,
/// SampleDesc.Count 0, and the topology type / FillMode / CullMode ranges,
/// through the vtables (x64 and x86, SM 5.0 and SM 6.0), and what such
/// pipelines draw on the GPU.
///
/// Expected values: tools/oracle_probe/d3d12_a72_pso_target_sample_probe on
/// WARP and an RTX 4080 SUPER (identical for every row used here). Shaders:
/// tests/oracle_data/d3d12_a72 (our HLSL, fxc / dxc on winoracle-gpu).

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"

#include "../../oracle_data/d3d12_a72/d3d12_a72_shaders.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x40000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kIid = kData + 0x100;
constexpr GuestAddr kOut = kData + 0x200;
constexpr GuestAddr kOut2 = kData + 0x240;
constexpr GuestAddr kDesc = kData + 0x400;
constexpr GuestAddr kDesc2 = kData + 0xC00;
constexpr GuestAddr kArray = kData + 0x1000;
constexpr GuestAddr kScratch = kData + 0x1100;  // viewport, scissor, clear, handles
constexpr GuestAddr kBlob = kData + 0x2000;
constexpr GuestAddr kVs = kData + 0x4000;
constexpr GuestAddr kPs = kData + 0x8000;

struct Code {
    const unsigned char* bytes;
    usize size;
};
#define A72(name) Code{g_a72_##name, sizeof(g_a72_##name)}

struct Shaders {
    Code vs, t0, t01, t3, t03, uav;
};
const Shaders kSm[2] = {
    {A72(vs_full_sm5), A72(ps_t0_sm5), A72(ps_t01_sm5), A72(ps_t3_sm5), A72(ps_t03_sm5),
     A72(ps_uav_sm5)},
    {A72(vs_full_sm6), A72(ps_t0_sm6), A72(ps_t01_sm6), A72(ps_t3_sm6), A72(ps_t03_sm6),
     A72(ps_uav_sm6)},
};
constexpr Code kNoPs{nullptr, 0};

constexpr u32 kInvalid = static_cast<u32>(E_INVALIDARG);
constexpr u32 kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kD32 = DXGI_FORMAT_D32_FLOAT;

/// Param: (x64, SM 6).
class D3D12PsoTargetsTest : public ::testing::TestWithParam<std::tuple<bool, bool>> {
public:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        ::setenv("VITREO_D3D12", "1", 1);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
        x64 = std::get<0>(GetParam());
        sm = &kSm[std::get<1>(GetParam()) ? 1 : 0];
        ASSERT_TRUE(memory.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
        cpu.mode = x64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        manager = std::make_unique<D3D12Manager>(memory, x64, graphics::MetalDevice::instance());
        exports = create_d3d12_thunks(*manager);
        for (const auto& entry : exports.entries) by_name[entry.name] = &entry;
        D3D12Manager::MethodThunks thunks{};
        GuestAddr next = 0x70000000;
        const auto fill = [&](const char* interface_name, auto& table) {
            for (usize slot = 0; slot < table.size(); ++slot) {
                table[slot] = next;
                by_thunk[next] = d3d12_method_export_name(interface_name, slot);
                next += 0x10;
            }
        };
        for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
            thunks.tables[table].resize(INTERFACE_TABLES[table].method_count);
            fill(INTERFACE_TABLES[table].export_name, thunks.tables[table]);
        }
        fill("Blob", thunks.blob);
        fill("RootSignatureDeserializer", thunks.root_signature_deserializer);
        fill("VersionedRootSignatureDeserializer", thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
        device = made(call("D3D12CreateDevice",
                           {0, D3D_FEATURE_LEVEL_11_0, iid(IID_ID3D12DEVICE), kOut}));
        ASSERT_NE(device, 0u);
        // One root UAV at u0 (ps_uav); no input layout.
        RootSignatureDesc rs;
        RootParameter uav;
        uav.parameter_type = D3D12_ROOT_PARAMETER_TYPE_UAV;
        uav.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
        rs.parameters.push_back(uav);
        std::vector<u8> blob;
        std::string error;
        ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK)
            << error;
        ASSERT_TRUE(memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
        root = made(com(device, DeviceSlot::CreateRootSignature,
                        {0, kBlob, blob.size(), iid(IID_ID3D12ROOTSIGNATURE), kOut}));
        ASSERT_NE(root, 0u);
    }

    u32 ptr() const { return x64 ? 8u : 4u; }
    u64 read_ptr(GuestAddr at) const {
        return x64 ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 v) {
        if (x64) {
            (void)memory.write_u64(at, v);
        } else {
            (void)memory.write_u32(at, static_cast<u32>(v));
        }
    }
    void w32(GuestAddr at, u32 v) { (void)memory.write_u32(at, v); }
    void wf(GuestAddr at, float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        w32(at, bits);
    }
    void zero(GuestAddr at, usize n) {
        const std::vector<u8> z(n, 0);
        (void)memory.write_bytes(at, z.data(), n);
    }

    /// Arguments are 32 bits wide on x86 unless marked (a UINT64).
    u64 call(const std::string& name, const std::vector<u64>& args,
             const std::vector<bool>& wide = {}) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        if (x64) {
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    // A float in the first four positions travels in XMMi.
                    *regs[i] = args[i];
                    cpu.sse.xmm[i].u64_[0] = args[i];
                } else {
                    (void)memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
                }
            }
        } else {
            usize slot = 0;
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory.write_u32(kStack + 4 + slot * 4, static_cast<u32>(args[i]));
                ++slot;
                if (i < wide.size() && wide[i]) {
                    (void)memory.write_u32(kStack + 4 + slot * 4,
                                           static_cast<u32>(args[i] >> 32));
                    ++slot;
                }
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        found->second->handler(m);
        return x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull) | ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32);
    }

    u64 com(GuestAddr object, usize slot, std::vector<u64> args, std::vector<bool> wide = {}) {
        const u64 thunk = read_ptr(read_ptr(object) + slot * ptr());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), object);
        if (!wide.empty()) wide.insert(wide.begin(), false);
        return call(name->second, args, wide);
    }
    u32 hr(u64 v) const { return static_cast<u32>(v); }

    GuestAddr iid(const GuidBytes& value) {
        (void)memory.write_bytes(kIid, value.data(), value.size());
        return kIid;
    }
    GuestAddr made(u64 v) { return hr(v) == S_OK ? read_ptr(kOut) : 0; }

    // ---- pipeline descriptions

    /// vs_full + @p ps, no input layout, no target, no depth, one sample,
    /// every blend description valid and off.
    void base(Code ps) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[x64];
        zero(kDesc, gl.size);
        (void)memory.write_bytes(kVs, sm->vs.bytes, sm->vs.size);
        if (ps.bytes) (void)memory.write_bytes(kPs, ps.bytes, ps.size);
        write_ptr(kDesc + gl.pRootSignature, root);
        write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
        write_ptr(kDesc + gl.VS_BytecodeLength, sm->vs.size);
        write_ptr(kDesc + gl.PS_pShaderBytecode, ps.bytes ? kPs : 0);
        write_ptr(kDesc + gl.PS_BytecodeLength, ps.size);
        for (u32 index = 0; index < 8; ++index) {
            const GuestAddr rt = kDesc + gl.BlendState_RenderTarget + index * blend.size;
            w32(rt + blend.SrcBlend, D3D12_BLEND_ONE);
            w32(rt + blend.DestBlend, D3D12_BLEND_ZERO);
            w32(rt + blend.BlendOp, D3D12_BLEND_OP_ADD);
            w32(rt + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
            w32(rt + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
            w32(rt + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
            w32(rt + blend.LogicOp, D3D12_LOGIC_OP_NOOP);
            (void)memory.write_u8(rt + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
        }
        w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
        w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
        w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
        w32(kDesc + gl.DepthStencilState_DepthWriteMask, D3D12_DEPTH_WRITE_MASK_ALL);
        w32(kDesc + gl.DepthStencilState_DepthFunc, D3D12_COMPARISON_FUNC_ALWAYS);
        w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        w32(kDesc + gl.SampleDesc_Count, 1);
    }
    void targets(u32 n, u32 format) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
        w32(kDesc + gl.NumRenderTargets, n);
        for (u32 i = 0; i < 8; ++i) w32(kDesc + gl.RTVFormats + i * 4, i < n ? format : 0u);
    }
    void rtv_format(u32 index, u32 format) {
        w32(kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64].RTVFormats + index * 4, format);
    }
    void depth(u32 format) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
        w32(kDesc + gl.DSVFormat, format);
        w32(kDesc + gl.DepthStencilState_DepthEnable, 1);
    }
    void samples(u32 count, u32 quality) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
        w32(kDesc + gl.SampleDesc_Count, count);
        w32(kDesc + gl.SampleDesc_Quality, quality);
    }
    void field(u32 offset, u32 value) { w32(kDesc + offset, value); }

    u32 create() {
        write_ptr(kOut, 0);
        return hr(com(device, DeviceSlot::CreateGraphicsPipelineState,
                      {kDesc, iid(IID_ID3D12PIPELINESTATE), kOut}));
    }
    GuestAddr pso() {
        const u32 r = create();
        EXPECT_EQ(r, S_OK);
        return r == S_OK ? read_ptr(kOut) : 0;
    }

    // ---- GPU objects

    GuestAddr resource(u32 heap_type, u32 state, u32 dimension, u64 width, u32 height,
                       u32 format, u32 flags) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[x64];
        const auto& rd = layout::D3D12_RESOURCE_DESC[x64];
        zero(kDesc, hp.size);
        zero(kDesc2, rd.size);
        w32(kDesc + hp.Type, heap_type);
        w32(kDesc2 + rd.Dimension, dimension);
        (void)memory.write_u64(kDesc2 + rd.Width, width);
        w32(kDesc2 + rd.Height, height);
        (void)memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1);
        (void)memory.write_u16(kDesc2 + rd.MipLevels, 1);
        w32(kDesc2 + rd.Format, format);
        w32(kDesc2 + rd.SampleDesc_Count, 1);
        w32(kDesc2 + rd.Layout, dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                    ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                    : D3D12_TEXTURE_LAYOUT_UNKNOWN);
        w32(kDesc2 + rd.Flags, flags);
        const GuestAddr r = made(com(device, DeviceSlot::CreateCommittedResource,
                                     {kDesc, 0, kDesc2, state, 0, iid(IID_ID3D12RESOURCE), kOut}));
        EXPECT_NE(r, 0u);
        return r;
    }
    GuestAddr texture(u32 format, u32 state, u32 flags) {
        return resource(D3D12_HEAP_TYPE_DEFAULT, state, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize,
                        kSize, format, flags);
    }
    GuestAddr buffer(u32 heap_type, u32 state, u64 bytes, u32 flags = 0) {
        return resource(heap_type, state, D3D12_RESOURCE_DIMENSION_BUFFER, bytes, 1, 0, flags);
    }
    GuestAddr heap(u32 type, u32 count) {
        const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[x64];
        zero(kDesc, l.size);
        w32(kDesc + l.Type, type);
        w32(kDesc + l.NumDescriptors, count);
        return made(com(device, DeviceSlot::CreateDescriptorHeap,
                        {kDesc, iid(IID_ID3D12DESCRIPTORHEAP), kOut}));
    }
    u64 cpu_start(GuestAddr h) {
        (void)com(h, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart, {kOut2});
        return read_ptr(kOut2);
    }
    u64 gpu_va(GuestAddr r) { return com(r, ResourceSlot::GetGPUVirtualAddress, {}); }
    void transition(GuestAddr list, GuestAddr r, u32 before, u32 after) {
        const auto& l = layout::D3D12_RESOURCE_BARRIER[x64];
        zero(kDesc, l.size);
        w32(kDesc + l.Type, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
        write_ptr(kDesc + l.Transition_pResource, r);
        w32(kDesc + l.Transition_Subresource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        w32(kDesc + l.Transition_StateBefore, before);
        w32(kDesc + l.Transition_StateAfter, after);
        (void)com(list, GraphicsCommandListSlot::ResourceBarrier, {1, kDesc});
    }
    GuestAddr list(GuestAddr state) {
        const GuestAddr alloc = made(com(device, DeviceSlot::CreateCommandAllocator,
                                         {D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          iid(IID_ID3D12COMMANDALLOCATOR), kOut}));
        return made(com(device, DeviceSlot::CreateCommandList,
                        {0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, state,
                         iid(IID_ID3D12GRAPHICSCOMMANDLIST), kOut}));
    }
    void run(GuestAddr l) {
        if (queue == 0) {
            const auto& q = layout::D3D12_COMMAND_QUEUE_DESC[x64];
            zero(kDesc, q.size);
            queue = made(com(device, DeviceSlot::CreateCommandQueue,
                             {kDesc, iid(IID_ID3D12COMMANDQUEUE), kOut}));
            ASSERT_NE(queue, 0u);
        }
        ASSERT_EQ(hr(com(l, GraphicsCommandListSlot::Close, {})), S_OK);
        write_ptr(kArray, l);
        (void)com(queue, CommandQueueSlot::ExecuteCommandLists, {1, kArray});
        const GuestAddr fence = made(com(device, DeviceSlot::CreateFence,
                                         {0, 0, iid(IID_ID3D12FENCE), kOut}, {true}));
        ASSERT_EQ(hr(com(queue, CommandQueueSlot::Signal, {fence, 1}, {false, true})), S_OK);
        ASSERT_EQ(hr(com(fence, FenceSlot::SetEventOnCompletion, {1, 0}, {true})), S_OK);
    }
    std::vector<u8> read_back(GuestAddr b, usize bytes) {
        EXPECT_EQ(hr(com(b, ResourceSlot::Map, {0, 0, kOut2})), S_OK);
        std::vector<u8> out(bytes);
        (void)memory.read_bytes(read_ptr(kOut2), out.data(), bytes);
        (void)com(b, ResourceSlot::Unmap, {0, 0});
        return out;
    }
    void copy_to_footprint(GuestAddr l, GuestAddr dst, GuestAddr tex, u32 format) {
        const auto& c = layout::D3D12_TEXTURE_COPY_LOCATION[x64];
        zero(kDesc, c.size);
        zero(kDesc2, c.size);
        write_ptr(kDesc + c.pResource, dst);
        w32(kDesc + c.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        w32(kDesc + c.PlacedFootprint_Footprint_Format, format);
        w32(kDesc + c.PlacedFootprint_Footprint_Width, kSize);
        w32(kDesc + c.PlacedFootprint_Footprint_Height, kSize);
        w32(kDesc + c.PlacedFootprint_Footprint_Depth, 1);
        w32(kDesc + c.PlacedFootprint_Footprint_RowPitch, kRow);
        write_ptr(kDesc2 + c.pResource, tex);
        w32(kDesc2 + c.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        (void)com(l, GraphicsCommandListSlot::CopyTextureRegion, {kDesc, 0, 0, 0, kDesc2, 0});
    }

    /// Draws the full-viewport triangle with @p state into @p n colour
    /// targets (cleared to 0.2 first) and the optional depth view (cleared
    /// to 1.0), with the optional root UAV.
    void draw(GuestAddr l, GuestAddr state, const u64* rtvs, u32 n, u64 dsv, u64 uav_va) {
        const auto& vp = layout::D3D12_VIEWPORT[x64];
        const auto& rect = layout::D3D12_RECT[x64];
        const GuestAddr view = kScratch, scissor = kScratch + 0x40, clear = kScratch + 0x60,
                        handles = kScratch + 0x80, dsv_slot = kScratch + 0xC0;
        wf(view + vp.TopLeftX, 0.f);
        wf(view + vp.TopLeftY, 0.f);
        wf(view + vp.Width, static_cast<float>(kSize));
        wf(view + vp.Height, static_cast<float>(kSize));
        wf(view + vp.MinDepth, 0.f);
        wf(view + vp.MaxDepth, 1.f);
        w32(scissor + rect.left, 0);
        w32(scissor + rect.top, 0);
        w32(scissor + rect.right, kSize);
        w32(scissor + rect.bottom, kSize);
        for (u32 c = 0; c < 4; ++c) wf(clear + c * 4, 0.2f);
        for (u32 i = 0; i < n; ++i) {
            write_ptr(handles + i * ptr(), rtvs[i]);
            (void)com(l, GraphicsCommandListSlot::ClearRenderTargetView, {rtvs[i], clear, 0, 0});
        }
        if (dsv != 0) {
            write_ptr(dsv_slot, dsv);
            u32 one = 0;
            const float f = 1.0f;
            std::memcpy(&one, &f, 4);
            (void)com(l, GraphicsCommandListSlot::ClearDepthStencilView,
                      {dsv, D3D12_CLEAR_FLAG_DEPTH, one, 0, 0, 0});
        }
        (void)com(l, GraphicsCommandListSlot::OMSetRenderTargets,
                  {n, n ? handles : 0, 0, dsv ? dsv_slot : 0});
        (void)com(l, GraphicsCommandListSlot::RSSetViewports, {1, view});
        (void)com(l, GraphicsCommandListSlot::RSSetScissorRects, {1, scissor});
        (void)com(l, GraphicsCommandListSlot::IASetPrimitiveTopology,
                  {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST});
        (void)com(l, GraphicsCommandListSlot::SetGraphicsRootSignature, {root});
        if (uav_va != 0) {
            (void)com(l, GraphicsCommandListSlot::SetGraphicsRootUnorderedAccessView, {0, uav_va},
                      {false, true});
        }
        (void)com(l, GraphicsCommandListSlot::SetPipelineState, {state});
        (void)com(l, GraphicsCommandListSlot::DrawInstanced, {3, 1, 0, 0});
    }

    static constexpr u32 kSize = 8;
    static constexpr u32 kRow = 256;

    bool x64{true};
    const Shaders* sm{nullptr};
    GuestMemory memory;
    cpu::CpuState cpu;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    GuestAddr device{0};
    GuestAddr root{0};
    GuestAddr queue{0};
};

INSTANTIATE_TEST_SUITE_P(BitnessAndShaderModel, D3D12PsoTargetsTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool()),
                         [](const auto& info) {
                             return std::string(std::get<0>(info.param) ? "x64" : "x86") +
                                    (std::get<1>(info.param) ? "_sm6" : "_sm5");
                         });

// a72 pso "PS SV_Target..." rows: a pixel shader may write SV_Target
// outputs past NumRenderTargets or into a slot with no format.
TEST_P(D3D12PsoTargetsTest, PixelShaderOutputsWithoutATarget) {
    base(sm->t0);
    EXPECT_EQ(create(), S_OK) << "SV_Target0, NumRT 0, no DSV";
    base(sm->t0);
    depth(kD32);
    EXPECT_EQ(create(), S_OK) << "SV_Target0, NumRT 0, DSV D32";
    base(sm->t0);
    depth(DXGI_FORMAT_D16_UNORM);
    EXPECT_EQ(create(), S_OK) << "SV_Target0, NumRT 0, DSV D16";
    base(sm->t0);
    targets(1, DXGI_FORMAT_UNKNOWN);
    EXPECT_EQ(create(), S_OK) << "SV_Target0, NumRT 1, RTVFormats[0] UNKNOWN";
    base(sm->t01);
    targets(1, kRgba8);
    EXPECT_EQ(create(), S_OK) << "SV_Target0..1, NumRT 1 RGBA8";
    base(sm->t01);
    targets(1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    depth(kD32);
    EXPECT_EQ(create(), S_OK) << "SV_Target0..1, NumRT 1 RGBA16F, DSV D32";
    base(sm->t3);
    targets(2, kRgba8);
    EXPECT_EQ(create(), S_OK) << "SV_Target3, NumRT 2";
    base(sm->t03);
    targets(2, kRgba8);
    EXPECT_EQ(create(), S_OK) << "SV_Target0+3, NumRT 2";
    base(sm->t3);
    targets(4, kRgba8);
    rtv_format(3, DXGI_FORMAT_UNKNOWN);
    EXPECT_EQ(create(), S_OK) << "SV_Target3, NumRT 4, RTVFormats[3] UNKNOWN";
    // A format past NumRenderTargets is still refused.
    base(sm->t0);
    rtv_format(0, kRgba8);
    EXPECT_EQ(create(), kInvalid) << "NumRT 0, RTVFormats[0] RGBA8";
}

// a72 pso "SampleDesc {count, quality}" rows (identical on WARP and the
// RTX 4080 SUPER; counts above 4 with an output format are the device's).
TEST_P(D3D12PsoTargetsTest, SampleDescWithAndWithoutOutputs) {
    struct Case {
        u32 count, quality;
        u32 no_output;   // PS uav, NumRT 0, no DSV
        u32 with_depth;  // no PS, NumRT 0, DSV D32
        u32 with_color;  // SV_Target0, NumRT 1 RGBA8
    };
    const Case cases[] = {
        {0, 0, S_OK, kInvalid, kInvalid},
        {0, 1, kInvalid, kInvalid, kInvalid},
        {0, 0xFFFFFFFFu, kInvalid, kInvalid, kInvalid},
        {0, 0xFFFFFFFEu, kInvalid, kInvalid, kInvalid},
        {1, 0, S_OK, S_OK, S_OK},
        {1, 1, kInvalid, kInvalid, kInvalid},
        {1, 0xFFFFFFFFu, kInvalid, S_OK, S_OK},
        {1, 0xFFFFFFFEu, kInvalid, S_OK, S_OK},
        {2, 0, S_OK, S_OK, S_OK},
        {2, 0xFFFFFFFFu, S_OK, S_OK, S_OK},
        {3, 0, kInvalid, kInvalid, kInvalid},
        {4, 0, S_OK, S_OK, S_OK},
        {4, 1, kInvalid, kInvalid, kInvalid},
        {4, 0xFFFFFFFFu, S_OK, S_OK, S_OK},
        {4, 0xFFFFFFFEu, kInvalid, S_OK, S_OK},
        {8, 0, S_OK, 0, 0},
        {8, 0xFFFFFFFFu, S_OK, 0, 0},
        {16, 0, S_OK, 0, 0},
        {16, 0xFFFFFFFFu, S_OK, 0, 0},
        {16, 1, kInvalid, kInvalid, kInvalid},
        {32, 0, kInvalid, kInvalid, kInvalid},
        {33, 0, kInvalid, kInvalid, kInvalid},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::Message() << "SampleDesc {" << c.count << ", 0x" << std::hex
                                          << c.quality << "}");
        base(sm->uav);
        samples(c.count, c.quality);
        EXPECT_EQ(create(), c.no_output) << "PS uav, no output";
        base(kNoPs);
        samples(c.count, c.quality);
        EXPECT_EQ(create(), c.no_output) << "no PS, no output";
        base(sm->t0);
        targets(1, DXGI_FORMAT_UNKNOWN);
        samples(c.count, c.quality);
        EXPECT_EQ(create(), c.no_output) << "NumRT 1 UNKNOWN, no output";
        if (c.count > 4) continue;  // the device's sample counts
        base(kNoPs);
        depth(kD32);
        samples(c.count, c.quality);
        EXPECT_EQ(create(), c.with_depth) << "DSV D32";
        base(sm->t0);
        targets(1, kRgba8);
        samples(c.count, c.quality);
        EXPECT_EQ(create(), c.with_color) << "RGBA8";
    }
    // a72 "SampleDesc.Count 0, ForcedSampleCount 4, PS uav, no targets".
    base(sm->uav);
    samples(0, 0);
    field(layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64].RasterizerState_ForcedSampleCount, 4);
    EXPECT_EQ(create(), S_OK);
}

// a72 pso "PrimitiveTopologyType" / "FillMode" / "CullMode" rows, with no
// output and with an RGBA8 target alike.
TEST_P(D3D12PsoTargetsTest, TopologyFillAndCullRanges) {
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
    for (int shape = 0; shape < 2; ++shape) {
        const auto start = [&] {
            if (shape) {
                base(sm->t0);
                targets(1, kRgba8);
            } else {
                base(sm->uav);
            }
        };
        const u32 topology[6] = {kInvalid, S_OK, S_OK, S_OK, kInvalid, kInvalid};
        for (u32 t = 0; t < 6; ++t) {
            start();
            field(gl.PrimitiveTopologyType, t);
            EXPECT_EQ(create(), topology[t]) << "topology type " << t << " shape " << shape;
        }
        const u32 fill[5] = {kInvalid, kInvalid, S_OK, S_OK, kInvalid};
        for (u32 f = 0; f < 5; ++f) {
            start();
            field(gl.RasterizerState_FillMode, f);
            EXPECT_EQ(create(), fill[f]) << "FillMode " << f << " shape " << shape;
        }
        const u32 cull[5] = {kInvalid, S_OK, S_OK, S_OK, kInvalid};
        for (u32 c = 0; c < 5; ++c) {
            start();
            field(gl.RasterizerState_CullMode, c);
            EXPECT_EQ(create(), cull[c]) << "CullMode " << c << " shape " << shape;
        }
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// The fields the old combined rule lumped together are refused one reason
// each (VITREO_D3D12_DUMP_REFUSED's reason file names the field).
TEST_P(D3D12PsoTargetsTest, EachRangeCheckNamesItsField) {
    if (std::get<1>(GetParam()) || !x64) GTEST_SKIP() << "one configuration is enough";
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("vitreo-d3d12-a72-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
    struct Case {
        u32 offset, value;
        const char* reason;
    };
    const Case cases[] = {
        {gl.NumRenderTargets, 9, "NumRenderTargets 9 is above 8"},
        {gl.PrimitiveTopologyType, 0, "PrimitiveTopologyType UNDEFINED"},
        {gl.PrimitiveTopologyType, 5, "PrimitiveTopologyType 5 is out of range"},
        {gl.RasterizerState_FillMode, 4, "FillMode 4 is out of range"},
        {gl.RasterizerState_CullMode, 0, "CullMode 0 is out of range"},
        {gl.SampleDesc_Count, 3, "SampleDesc.Count 3 with no output"},
    };
    for (const Case& c : cases) {
        fs::remove_all(dir);
        ::setenv("VITREO_D3D12_DUMP_REFUSED", dir.c_str(), 1);
        base(sm->uav);
        field(c.offset, c.value);
        EXPECT_EQ(create(), kInvalid) << c.reason;
        ::unsetenv("VITREO_D3D12_DUMP_REFUSED");
        std::string text;
        if (fs::exists(dir)) {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().filename().string().ends_with(".reason.txt")) {
                    text = read_text(entry.path());
                }
            }
        }
        EXPECT_NE(text.find(c.reason), std::string::npos) << text;
    }
    fs::remove_all(dir);
    ::setenv("VITREO_D3D12_DUMP_REFUSED", dir.c_str(), 1);
    base(sm->t0);
    depth(kD32);
    samples(0, 0);
    EXPECT_EQ(create(), kInvalid);
    ::unsetenv("VITREO_D3D12_DUMP_REFUSED");
    std::string text;
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().filename().string().ends_with(".reason.txt")) {
                text = read_text(entry.path());
            }
        }
    }
    fs::remove_all(dir);
    EXPECT_NE(text.find("SampleDesc.Count 0 with a render-target or depth-stencil format"),
              std::string::npos)
        << text;
}

// a72 draw: the bound targets get what the pixel shader wrote to them, a
// target it does not write keeps its clear, and the outputs with no target
// go nowhere. SV_Target0 = (0.2, 0.4, 0.6, 0.8) = 0xcc996633; cleared 0.2 =
// 0x33333333.
TEST_P(D3D12PsoTargetsTest, ExtraOutputsLeaveTheBoundTargetsCorrect) {
    const GuestAddr rt[2] = {
        texture(kRgba8, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        texture(kRgba8, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
    };
    const GuestAddr out[2] = {
        buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kRow * kSize),
        buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kRow * kSize),
    };
    const GuestAddr rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    ASSERT_NE(rtv_heap, 0u);
    const u64 start = cpu_start(rtv_heap);
    const u32 step = static_cast<u32>(com(device, DeviceSlot::GetDescriptorHandleIncrementSize,
                                          {D3D12_DESCRIPTOR_HEAP_TYPE_RTV}));
    const u64 rtvs[2] = {start, start + step};
    for (u32 i = 0; i < 2; ++i) {
        (void)com(device, DeviceSlot::CreateRenderTargetView, {rt[i], 0, rtvs[i]});
    }
    struct Case {
        const char* what;
        Code ps;
        u32 targets;
        u32 expect[2];
    };
    const Case cases[] = {
        {"SV_Target0..1, NumRT 1", sm->t01, 1, {0xcc996633u, 0}},
        {"SV_Target3, NumRT 2", sm->t3, 2, {0x33333333u, 0x33333333u}},
        {"SV_Target0+3, NumRT 2", sm->t03, 2, {0xcc996633u, 0x33333333u}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        base(c.ps);
        targets(c.targets, kRgba8);
        const GuestAddr state = pso();
        ASSERT_NE(state, 0u);
        const GuestAddr l = list(state);
        draw(l, state, rtvs, c.targets, 0, 0);
        for (u32 i = 0; i < c.targets; ++i) {
            transition(l, rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy_to_footprint(l, out[i], rt[i], kRgba8);
            transition(l, rt[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        run(l);
        for (u32 i = 0; i < c.targets; ++i) {
            const auto px = read_back(out[i], kRow * kSize);
            for (const u32 at : {1u, 4u, 6u}) {
                u32 got = 0;
                std::memcpy(&got, px.data() + at * kRow + at * 4, 4);
                EXPECT_EQ(got, c.expect[i]) << "RT" << i << " (" << at << "," << at << ") 0x"
                                            << std::hex << got;
            }
        }
    }
}

// a72 draw "SV_Target0, NumRT 0, DSV D32, Count 1": a depth-only pipeline
// whose pixel shader writes SV_Target0 writes depth 0.5 (0x3f000000).
TEST_P(D3D12PsoTargetsTest, DepthOnlyPipelineWithAColourOutputWritesDepth) {
    const GuestAddr ds = texture(kD32, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const GuestAddr out = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                 kRow * kSize);
    const GuestAddr dsv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    ASSERT_NE(dsv_heap, 0u);
    const u64 dsv = cpu_start(dsv_heap);
    (void)com(device, DeviceSlot::CreateDepthStencilView, {ds, 0, dsv});
    base(sm->t0);
    depth(kD32);
    const GuestAddr state = pso();
    ASSERT_NE(state, 0u);
    const GuestAddr l = list(state);
    draw(l, state, nullptr, 0, dsv, 0);
    transition(l, ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_to_footprint(l, out, ds, kD32);
    run(l);
    const auto px = read_back(out, kRow * kSize);
    for (const u32 at : {1u, 4u, 6u}) {
        u32 got = 0;
        std::memcpy(&got, px.data() + at * kRow + at * 4, 4);
        EXPECT_EQ(got, 0x3f000000u) << "(" << at << "," << at << ") 0x" << std::hex << got;
    }
}

// a72 draw "PS uav, no targets, Count N, ForcedSampleCount F": Count 0 draws
// as one sample (64 invocations over 8x8, coverage 0x1); Count 4 and a
// forced count of 4 cover the four samples (coverage 0xf).
TEST_P(D3D12PsoTargetsTest, SampleCountZeroWithNoOutputDrawsOneSample) {
    const GuestAddr counts = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 256,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr zeros = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    const GuestAddr out = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(hr(com(zeros, ResourceSlot::Map, {0, 0, kOut2})), S_OK);
    zero(read_ptr(kOut2), 256);
    (void)com(zeros, ResourceSlot::Unmap, {0, 0});
    struct Case {
        u32 count, forced, coverage;
    };
    const Case cases[] = {{0, 0, 0x1}, {1, 0, 0x1}, {4, 0, 0xf}, {0, 4, 0xf}};
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::Message() << "Count " << c.count << " forced " << c.forced);
        base(sm->uav);
        samples(c.count, 0);
        field(layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64].RasterizerState_ForcedSampleCount,
              c.forced);
        const GuestAddr state = pso();
        ASSERT_NE(state, 0u);
        const GuestAddr l = list(state);
        (void)com(l, GraphicsCommandListSlot::CopyBufferRegion, {counts, 0, zeros, 0, 256},
                  {false, true, false, true, true});
        transition(l, counts, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        draw(l, state, nullptr, 0, 0, gpu_va(counts));
        transition(l, counts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)com(l, GraphicsCommandListSlot::CopyBufferRegion, {out, 0, counts, 0, 8},
                  {false, true, false, true, true});
        transition(l, counts, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        run(l);
        const auto got = read_back(out, 8);
        u32 invocations = 0, coverage = 0;
        std::memcpy(&invocations, got.data(), 4);
        std::memcpy(&coverage, got.data() + 4, 4);
        // WARP counts 72 invocations at 4 samples (its helper lanes); the
        // RTX 4080 SUPER 64. One invocation per pixel is the contract.
        EXPECT_EQ(invocations, 64u);
        EXPECT_EQ(coverage, c.coverage);
    }
}

}  // namespace
}  // namespace vitreo::winapi
