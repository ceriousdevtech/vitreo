// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_metal.cpp
/// @brief d3d12.dll against a real GPU: command lists recorded through the
/// guest vtables, executed on Metal, read back through READBACK heaps.
///
/// Kept out of vitreo_winapi_tests because that target deliberately does not
/// link the Metal backend. The shaders are the probes' own fxc output
/// (tools/oracle_probe/d3d12_shaders), and every expected value is WARP's
/// (tools/oracle_probe/d3d12_a0*_probe.expected) or follows from the D3D
/// conversion rules cited next to it.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"

#include "../../oracle_data/d3d12_shaders/d3d12_shaders.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
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
constexpr GuestAddr kDesc2 = kData + 0x800;
constexpr GuestAddr kArray = kData + 0xC00;
constexpr GuestAddr kBlob = kData + 0x2000;
constexpr GuestAddr kShader = kData + 0x4000;

/// Calls through the exports and vtables, x64 or x86 (as test_d3d12.cpp).
class Harness {
public:
    explicit Harness(bool is_64bit) : m_is_64bit(is_64bit) {
        EXPECT_TRUE(memory.map_region(kData, kDataSize,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = is_64bit ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        manager = std::make_unique<D3D12Manager>(
            memory, is_64bit, graphics::MetalDevice::instance());
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
        fill("VersionedRootSignatureDeserializer",
             thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
    }

    [[nodiscard]] u32 ptr_size() const { return m_is_64bit ? 8u : 4u; }
    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return m_is_64bit ? memory.read_u64(at).value()
                          : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 value) {
        EXPECT_TRUE((m_is_64bit ? memory.write_u64(at, value)
                                : memory.write_u32(at, static_cast<u32>(value)))
                        .is_ok());
    }
    void w32(GuestAddr at, u32 v) { EXPECT_TRUE(memory.write_u32(at, v).is_ok()); }
    void w64(GuestAddr at, u64 v) { EXPECT_TRUE(memory.write_u64(at, v).is_ok()); }
    void zero(GuestAddr at, usize bytes) {
        const std::vector<u8> z(bytes, 0);
        EXPECT_TRUE(memory.write_bytes(at, z.data(), z.size()).is_ok());
    }
    [[nodiscard]] u32 r32(GuestAddr at) const { return memory.read_u32(at).value(); }

    struct Arg {
        u64 value;
        bool wide{false};
    };
    [[nodiscard]] static Arg wide(u64 v) { return {v, true}; }

    u64 call(const std::string& name, const std::vector<Arg>& args) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        if (m_is_64bit) {
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    // A float in the first four positions travels in XMMi.
                    *regs[i] = args[i].value;
                    cpu.sse.xmm[i].u64_[0] = args[i].value;
                } else {
                    EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8,
                                                 args[i].value).is_ok());
                }
            }
        } else {
            usize slot = 0;
            for (const Arg& a : args) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + slot * 4,
                                             static_cast<u32>(a.value)).is_ok());
                ++slot;
                if (a.wide) {
                    EXPECT_TRUE(memory.write_u32(
                        kStack + 4 + slot * 4,
                        static_cast<u32>(a.value >> 32)).is_ok());
                    ++slot;
                }
            }
            EXPECT_EQ(found->second->stdcall_bytes, slot * 4) << name;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        found->second->handler(m);
        return m_is_64bit ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    u64 com(GuestAddr object, usize slot, std::vector<Arg> args) {
        const u64 vtable = read_ptr(object);
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), Arg{object});
        return call(name->second, args);
    }

    GuestAddr iid(const GuidBytes& value) {
        EXPECT_TRUE(memory.write_bytes(kIid, value.data(), value.size()).is_ok());
        return kIid;
    }

    /// Create through (…, riid, ppv) and return the object.
    GuestAddr made(u64 hr) {
        EXPECT_EQ(hr, S_OK);
        return read_ptr(kOut);
    }

    GuestAddr device() {
        return made(call("D3D12CreateDevice", {{0}, {D3D_FEATURE_LEVEL_11_0},
                                               {iid(IID_ID3D12DEVICE)}, {kOut}}));
    }

    GuestAddr queue(GuestAddr dev) {
        const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[m_is_64bit];
        zero(kDesc, l.size);
        w32(kDesc + l.Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
        return made(com(dev, DeviceSlot::CreateCommandQueue,
                        {{kDesc}, {iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}));
    }

    /// A direct allocator and an open list on it.
    GuestAddr list(GuestAddr dev, GuestAddr pso = 0) {
        const GuestAddr alloc =
            made(com(dev, DeviceSlot::CreateCommandAllocator,
                     {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                      {iid(IID_ID3D12COMMANDALLOCATOR)}, {kOut}}));
        return made(com(dev, DeviceSlot::CreateCommandList,
                        {{0}, {D3D12_COMMAND_LIST_TYPE_DIRECT}, {alloc}, {pso},
                         {iid(IID_ID3D12GRAPHICSCOMMANDLIST)}, {kOut}}));
    }

    /// A committed buffer or 2D texture.
    GuestAddr resource(GuestAddr dev, u32 heap_type, u32 state, u32 dimension,
                       u64 width, u32 height, u32 format, u32 flags) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
        const auto& rd = layout::D3D12_RESOURCE_DESC[m_is_64bit];
        zero(kDesc, hp.size);
        zero(kDesc2, rd.size);
        w32(kDesc + hp.Type, heap_type);
        w32(kDesc2 + rd.Dimension, dimension);
        w64(kDesc2 + rd.Width, width);
        w32(kDesc2 + rd.Height, height);
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        w32(kDesc2 + rd.Format, format);
        w32(kDesc2 + rd.SampleDesc_Count, 1);
        w32(kDesc2 + rd.Layout, dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                     ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                     : D3D12_TEXTURE_LAYOUT_UNKNOWN);
        w32(kDesc2 + rd.Flags, flags);
        return made(com(dev, DeviceSlot::CreateCommittedResource,
                        {{kDesc}, {0}, {kDesc2}, {state}, {0},
                         {iid(IID_ID3D12RESOURCE)}, {kOut}}));
    }

    GuestAddr buffer(GuestAddr dev, u32 heap_type, u32 state, u64 bytes,
                     u32 flags = 0) {
        return resource(dev, heap_type, state, D3D12_RESOURCE_DIMENSION_BUFFER,
                        bytes, 1, 0, flags);
    }

    GuestAddr heap(GuestAddr dev, u32 type, u32 count, bool visible) {
        const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[m_is_64bit];
        zero(kDesc, l.size);
        w32(kDesc + l.Type, type);
        w32(kDesc + l.NumDescriptors, count);
        w32(kDesc + l.Flags, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 0u);
        return made(com(dev, DeviceSlot::CreateDescriptorHeap,
                        {{kDesc}, {iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}));
    }
    u64 cpu_start(GuestAddr heap) {
        (void)com(heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                  {{kOut2}});
        return read_ptr(kOut2);
    }
    u64 gpu_start(GuestAddr heap) {
        (void)com(heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart,
                  {{kOut2}});
        return memory.read_u64(kOut2).value();
    }

    void transition(GuestAddr list, GuestAddr resource, u32 before, u32 after) {
        const auto& l = layout::D3D12_RESOURCE_BARRIER[m_is_64bit];
        zero(kDesc, l.size);
        w32(kDesc + l.Type, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
        write_ptr(kDesc + l.Transition_pResource, resource);
        w32(kDesc + l.Transition_Subresource,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        w32(kDesc + l.Transition_StateBefore, before);
        w32(kDesc + l.Transition_StateAfter, after);
        (void)com(list, GraphicsCommandListSlot::ResourceBarrier, {{1}, {kDesc}});
    }

    /// Close, execute, and wait for the queue to finish.
    void run(GuestAddr dev, GuestAddr queue, GuestAddr list) {
        ASSERT_EQ(com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        write_ptr(kArray, list);
        (void)com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
        const GuestAddr fence =
            made(com(dev, DeviceSlot::CreateFence,
                     {wide(0), {0}, {iid(IID_ID3D12FENCE)}, {kOut}}));
        ASSERT_EQ(com(queue, CommandQueueSlot::Signal, {{fence}, wide(1)}), S_OK);
        // A NULL event blocks until the value is reached.
        ASSERT_EQ(com(fence, FenceSlot::SetEventOnCompletion, {wide(1), {0}}),
                  S_OK);
    }

    /// Map a READBACK buffer and copy @p bytes out.
    std::vector<u8> read_back(GuestAddr buffer, usize bytes) {
        EXPECT_EQ(com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
        const GuestAddr host = read_ptr(kOut2);
        std::vector<u8> out(bytes);
        EXPECT_TRUE(memory.read_bytes(host, out.data(), bytes).is_ok());
        (void)com(buffer, ResourceSlot::Unmap, {{0}, {0}});
        return out;
    }

    /// CopyTextureRegion of subresource 0 of @p texture into @p buffer at a
    /// 256-byte row pitch footprint.
    void copy_to_footprint(GuestAddr list, GuestAddr buffer, GuestAddr texture,
                           u32 format, u32 width, u32 height) {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[m_is_64bit];
        zero(kDesc, l.size);
        zero(kDesc2, l.size);
        write_ptr(kDesc + l.pResource, buffer);
        w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        w64(kDesc + l.PlacedFootprint_Offset, 0);
        w32(kDesc + l.PlacedFootprint_Footprint_Format, format);
        w32(kDesc + l.PlacedFootprint_Footprint_Width, width);
        w32(kDesc + l.PlacedFootprint_Footprint_Height, height);
        w32(kDesc + l.PlacedFootprint_Footprint_Depth, 1);
        w32(kDesc + l.PlacedFootprint_Footprint_RowPitch, 256);
        write_ptr(kDesc2 + l.pResource, texture);
        w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        w32(kDesc2 + l.SubresourceIndex, 0);
        (void)com(list, GraphicsCommandListSlot::CopyTextureRegion,
                  {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    bool m_is_64bit;
};

class D3D12MetalTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        ::setenv("VITREO_D3D12", "1", 1);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
        h = std::make_unique<Harness>(GetParam());
    }
    std::unique_ptr<Harness> h;
};

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12MetalTest, ::testing::Values(true, false),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });

// a07 barriers' compute chain on the GPU: cs_madd (v = v * cs_mul + cs_add
// over a raw UAV at u2, constants at b0) with a root-constant parameter and a
// descriptor table, fed by an upload copy and read back through a copy.
TEST_P(D3D12MetalTest, ComputeDispatchReadsRootConstantsAndATableUav) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter constants;
    constants.parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.num_32bit_values = 4;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav.base_shader_register = 2;
    table.ranges.push_back(uav);
    rs.parameters = {constants, table};
    std::vector<u8> blob;
    std::string error;
    ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error),
              S_OK)
        << error;
    ASSERT_TRUE(h->memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root =
        h->made(h->com(dev, DeviceSlot::CreateRootSignature,
                       {{0}, {kBlob}, {blob.size()},
                        {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));

    ASSERT_TRUE(h->memory.write_bytes(kShader, g_sh_cs_madd, sizeof(g_sh_cs_madd))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_madd));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr upload =
        h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    const GuestAddr data = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_COPY_DEST, 256,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < 64; ++i) h->w32(mapped + i * 4u, i);

    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w64(kDesc + ul.Buffer_FirstElement, 0);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {h->cpu_start(heap)}});

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(0), {upload}, Harness::wide(0),
                  Harness::wide(256)});
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    h->w32(kOut2, 3);  // cs_mul
    h->w32(kOut2 + 4, 7);  // cs_add
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRoot32BitConstants,
                 {{0}, {2}, {kOut2}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(heap))});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, 256);
    for (u32 i = 0; i < 64; ++i) {
        u32 v = 0;
        std::memcpy(&v, out.data() + i * 4u, 4);
        EXPECT_EQ(v, i * 3u + 7u) << i;
    }
}

// a03 rtv_dsv on the GPU: a full RTV clear, then a rectangle clear (stored
// as encoded texels, D3D 3.2 UNORM conversion: 0.5 -> 128, 0.25 -> 64), and
// a D32_FLOAT depth clear with a rectangle (a03 "ds mip0 (0.75, rect x<2 =
// 0.5)"), both read back through footprint copies.
TEST_P(D3D12MetalTest, ClearsWithRectanglesReachTheTextures) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr color = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr depth = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, DXGI_FORMAT_D32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const GuestAddr color_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr depth_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr rtvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const GuestAddr dsvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    const u64 rtv = h->cpu_start(rtvs);
    const u64 dsv = h->cpu_start(dsvs);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView, {{depth}, {0}, {dsv}});

    const GuestAddr list = h->list(dev);
    const float kHalf[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    const float kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    ASSERT_TRUE(h->memory.write_bytes(kDesc, reinterpret_cast<const u8*>(kHalf), sizeof(kHalf)).is_ok());
    (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                 {{rtv}, {kDesc}, {0}, {0}});
    ASSERT_TRUE(h->memory.write_bytes(kDesc + 0x20, reinterpret_cast<const u8*>(kGreen), sizeof(kGreen)).is_ok());
    const i32 rect[4] = {1, 1, 3, 3};
    ASSERT_TRUE(h->memory.write_bytes(kDesc + 0x40, reinterpret_cast<const u8*>(rect), sizeof(rect)).is_ok());
    (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                 {{rtv}, {kDesc + 0x20}, {1}, {kDesc + 0x40}});
    const auto f32_arg = [](float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        return Harness::Arg{bits};
    };
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{dsv}, {D3D12_CLEAR_FLAG_DEPTH}, f32_arg(0.75f), {0}, {0}, {0}});
    const i32 drect[4] = {0, 0, 2, 4};
    ASSERT_TRUE(h->memory.write_bytes(kDesc + 0x60, reinterpret_cast<const u8*>(drect), sizeof(drect)).is_ok());
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{dsv}, {D3D12_CLEAR_FLAG_DEPTH}, f32_arg(0.5f), {0}, {1},
                  {kDesc + 0x60}});
    h->transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, color_out, color, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4);
    h->copy_to_footprint(list, depth_out, depth, DXGI_FORMAT_R32_TYPELESS, 4, 4);
    h->run(dev, queue, list);

    const std::vector<u8> pixels = h->read_back(color_out, 1024);
    const auto texel = [&](u32 x, u32 y) {
        u32 v = 0;
        std::memcpy(&v, pixels.data() + y * 256u + x * 4u, 4);
        return v;
    };
    EXPECT_EQ(texel(0, 0), 0xFF4080FFu);  // R=FF G=80 B=40 A=FF, little endian
    EXPECT_EQ(texel(3, 3), 0xFF4080FFu);
    EXPECT_EQ(texel(1, 1), 0xFF00FF00u);
    EXPECT_EQ(texel(2, 2), 0xFF00FF00u);
    EXPECT_EQ(texel(3, 1), 0xFF4080FFu);

    const std::vector<u8> depths = h->read_back(depth_out, 1024);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            float d = 0.0f;
            std::memcpy(&d, depths.data() + y * 256u + x * 4u, 4);
            EXPECT_EQ(d, x < 2 ? 0.5f : 0.75f) << x << "," << y;
        }
    }
}

}  // namespace
}  // namespace vitreo::winapi
