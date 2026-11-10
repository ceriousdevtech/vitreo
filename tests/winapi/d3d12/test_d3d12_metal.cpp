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
#include "winapi/graphics/d3d12/d3d12_sampler_record_cache.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

#include "../../oracle_data/d3d12_shaders/d3d12_shaders.h"
#include "../../oracle_data/d3d12_binding/d3d12_binding_big.h"
#include "../../oracle_data/d3d12_heapwin/d3d12_heapwin_shaders.h"

namespace sysval {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../graphics/system_value_shaders.h"
#pragma clang diagnostic pop
}  // namespace sysval
namespace fl111 {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_fl111_shaders.h"
#pragma clang diagnostic pop
}  // namespace fl111

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <memory>
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

    u64 call(const std::string& name, const std::vector<Arg>& args,
             bool wide_return = false) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        cpu.gpr.rdx = 0xDDDDDDDDDDDDDDDDull;
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
        if (m_is_64bit) return cpu.gpr.rax;
        // An x86 UINT64 return is EDX:EAX. GetGPUVirtualAddress is one.
        if (wide_return) {
            return (cpu.gpr.rax & 0xFFFFFFFFull) |
                   ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32);
        }
        return cpu.gpr.rax & 0xFFFFFFFFull;
    }

    u64 com(GuestAddr object, usize slot, std::vector<Arg> args,
            bool wide_return = false) {
        const u64 vtable = read_ptr(object);
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), Arg{object});
        return call(name->second, args, wide_return);
    }

    /// Learn: D3D12_GPU_VIRTUAL_ADDRESS is a UINT64 on both bitnesses.
    u64 gpu_va(GuestAddr resource) {
        return com(resource, ResourceSlot::GetGPUVirtualAddress, {}, true);
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

    /// A committed buffer or 2D texture. Sample count is 1.
    GuestAddr resource(GuestAddr dev, u32 heap_type, u32 state, u32 dimension,
                       u64 width, u32 height, u32 format, u32 flags) {
        return texture(dev, heap_type, state, dimension, width, height, format,
                       flags, 1);
    }

    /// As resource(), with an explicit sample count (a 2x depth buffer).
    GuestAddr texture(GuestAddr dev, u32 heap_type, u32 state, u32 dimension,
                      u64 width, u32 height, u32 format, u32 flags,
                      u32 samples) {
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
        w32(kDesc2 + rd.SampleDesc_Count, samples);
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

    /// A committed 2D texture on a CPU-visible custom heap. Upload and
    /// readback heaps hold buffers only, and WriteToSubresource refuses a
    /// texture that is not CPU-visible.
    GuestAddr cpu_texture(GuestAddr dev, u32 format, u32 width, u32 height,
                          u32 flags) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
        const auto& rd = layout::D3D12_RESOURCE_DESC[m_is_64bit];
        zero(kDesc, hp.size);
        zero(kDesc2, rd.size);
        w32(kDesc + hp.Type, D3D12_HEAP_TYPE_CUSTOM);
        w32(kDesc + hp.CPUPageProperty, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
        w32(kDesc + hp.MemoryPoolPreference, D3D12_MEMORY_POOL_L0);
        w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        w64(kDesc2 + rd.Width, width);
        w32(kDesc2 + rd.Height, height);
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        w32(kDesc2 + rd.Format, format);
        w32(kDesc2 + rd.SampleDesc_Count, 1);
        w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
        w32(kDesc2 + rd.Flags, flags);
        const u64 hr = com(dev, DeviceSlot::CreateCommittedResource,
                           {{kDesc},
                            {0},
                            {kDesc2},
                            {D3D12_RESOURCE_STATE_COMMON},
                            {0},
                            {iid(IID_ID3D12RESOURCE)},
                            {kOut}});
        EXPECT_EQ(hr, S_OK);
        return hr == S_OK ? read_ptr(kOut) : 0;
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
                           u32 format, u32 width, u32 height,
                           u32 subresource = 0) {
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
        w32(kDesc2 + l.SubresourceIndex, subresource);
        (void)com(list, GraphicsCommandListSlot::CopyTextureRegion,
                  {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }

    /// CopyTextureRegion of one subresource, whole rectangle, Dst at (0,0,0).
    void copy_subresource(GuestAddr list, GuestAddr dst, u32 dst_sub,
                          GuestAddr src, u32 src_sub) {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[m_is_64bit];
        zero(kDesc, l.size);
        zero(kDesc2, l.size);
        write_ptr(kDesc + l.pResource, dst);
        w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        w32(kDesc + l.SubresourceIndex, dst_sub);
        write_ptr(kDesc2 + l.pResource, src);
        w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        w32(kDesc2 + l.SubresourceIndex, src_sub);
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
        if (!d3d12_enabled()) GTEST_SKIP() << "D3D12 explicitly disabled by VITREO_D3D12=0";
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

// Learn (CopyTextureRegion): depth and stencil are separate subresources, so
// a plane-0 copy leaves stencil and a plane-1 copy leaves depth. One mip and
// one slice: subresource 0 is the depth plane, 1 the stencil plane. D24's
// depth footprint is UNORM24 in an R32 row (a05 clear "D24S8", nearest even:
// 0.5 -> 0x800000, 1 -> 0xFFFFFF) and the stencil plane is the R8 byte.
TEST_P(D3D12MetalTest, DepthAndStencilPlanesCopyApart) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr src = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, DXGI_FORMAT_D24_UNORM_S8_UINT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const GuestAddr dst = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, DXGI_FORMAT_D24_UNORM_S8_UINT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const GuestAddr depth_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr stencil_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                            D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr depth_out2 = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr stencil_out2 = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 1024);
    const GuestAddr src_dsv = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    const GuestAddr dst_dsv = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView,
                 {{src}, {0}, {h->cpu_start(src_dsv)}});
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView,
                 {{dst}, {0}, {h->cpu_start(dst_dsv)}});

    const GuestAddr list = h->list(dev);
    const auto f32_arg = [](float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        return Harness::Arg{bits};
    };
    const u32 both = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{h->cpu_start(src_dsv)}, {both}, f32_arg(0.5f), {0x5A}, {0}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{h->cpu_start(dst_dsv)}, {both}, f32_arg(1.0f), {0x11}, {0}, {0}});
    h->transition(list, src, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->transition(list, dst, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    h->copy_subresource(list, dst, 0, src, 0);
    h->transition(list, dst, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, depth_out, dst, DXGI_FORMAT_R32_TYPELESS, 4, 4, 0);
    h->copy_to_footprint(list, stencil_out, dst, DXGI_FORMAT_R8_TYPELESS, 4, 4, 1);
    h->transition(list, dst, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    h->copy_subresource(list, dst, 1, src, 1);
    h->transition(list, dst, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, depth_out2, dst, DXGI_FORMAT_R32_TYPELESS, 4, 4, 0);
    h->copy_to_footprint(list, stencil_out2, dst, DXGI_FORMAT_R8_TYPELESS, 4, 4, 1);
    h->run(dev, queue, list);

    const std::vector<u8> depth = h->read_back(depth_out, 1024);
    const std::vector<u8> stencil = h->read_back(stencil_out, 1024);
    const std::vector<u8> depth_after = h->read_back(depth_out2, 1024);
    const std::vector<u8> stencil_after = h->read_back(stencil_out2, 1024);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            u32 d = 0;
            std::memcpy(&d, depth.data() + y * 256u + x * 4u, 4);
            EXPECT_EQ(d, 0x800000u) << "depth " << x << "," << y;
            EXPECT_EQ(stencil[y * 256u + x], 0x11) << "stencil " << x << "," << y;
            u32 d2 = 0;
            std::memcpy(&d2, depth_after.data() + y * 256u + x * 4u, 4);
            EXPECT_EQ(d2, 0x800000u) << "depth after stencil copy " << x << "," << y;
            EXPECT_EQ(stencil_after[y * 256u + x], 0x5A)
                << "stencil after stencil copy " << x << "," << y;
        }
    }
}

// Learn (ResolveSubresource, D3D12_RESOLVE_MODE_AVERAGE): the resolve with
// no mode is the average of the samples, and AVERAGE includes the depth
// plane. A 2x depth buffer whose samples were uploaded as 0.25 and 0.75
// therefore resolves to 0.5. Those two values are the bytes written into
// the footprint (sample 0, then sample 1, the order CopyTextureRegion of a
// multisampled depth uses). 0.25 and 0.75 are exact float32, and so is
// their mean.
TEST_P(D3D12MetalTest, DepthResolveAveragesTheTwoSamples) {
    constexpr u32 kSize = 2;
    constexpr u32 kPitch = 256;
    constexpr u32 kSamples = 2;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr src = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize, DXGI_FORMAT_D32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, kSamples);
    const GuestAddr dst = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize, DXGI_FORMAT_D32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 1);
    const GuestAddr upload = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 4096);
    const GuestAddr samples_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr resolved_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);

    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    const float sample0 = 0.25f;
    const float sample1 = 0.75f;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const usize at = static_cast<usize>(y) * kPitch +
                             static_cast<usize>(x) * kSamples * 4u;
            ASSERT_TRUE(h->memory
                            .write_bytes(mapped + at,
                                         reinterpret_cast<const u8*>(&sample0), 4)
                            .is_ok());
            ASSERT_TRUE(h->memory
                            .write_bytes(mapped + at + 4,
                                         reinterpret_cast<const u8*>(&sample1), 4)
                            .is_ok());
        }
    }
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    const GuestAddr list = h->list(dev);
    const auto& loc = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
    h->zero(kDesc, loc.size);
    h->zero(kDesc2, loc.size);
    h->write_ptr(kDesc + loc.pResource, src);
    h->w32(kDesc + loc.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
    h->write_ptr(kDesc2 + loc.pResource, upload);
    h->w32(kDesc2 + loc.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
    h->w32(kDesc2 + loc.PlacedFootprint_Footprint_Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc2 + loc.PlacedFootprint_Footprint_Width, kSize);
    h->w32(kDesc2 + loc.PlacedFootprint_Footprint_Height, kSize);
    h->w32(kDesc2 + loc.PlacedFootprint_Footprint_Depth, 1);
    h->w32(kDesc2 + loc.PlacedFootprint_Footprint_RowPitch, kPitch);
    (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                 {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    h->transition(list, src, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, samples_out, src, DXGI_FORMAT_R32_TYPELESS, kSize,
                         kSize);
    h->transition(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::ResolveSubresource,
                 {{dst}, {0}, {src}, {0}, {DXGI_FORMAT_D32_FLOAT}});
    h->transition(list, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, resolved_out, dst, DXGI_FORMAT_R32_TYPELESS, kSize,
                         kSize);
    h->run(dev, queue, list);

    const std::vector<u8> samples = h->read_back(samples_out, kPitch * kSize);
    const std::vector<u8> resolved = h->read_back(resolved_out, kPitch * kSize);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            float s0 = 0.0f;
            float s1 = 0.0f;
            float got = 0.0f;
            const usize sample_at = static_cast<usize>(y) * kPitch +
                                    static_cast<usize>(x) * kSamples * 4u;
            const usize resolved_at =
                static_cast<usize>(y) * kPitch + static_cast<usize>(x) * 4u;
            std::memcpy(&s0, samples.data() + sample_at, 4);
            std::memcpy(&s1, samples.data() + sample_at + 4, 4);
            std::memcpy(&got, resolved.data() + resolved_at, 4);
            EXPECT_EQ(s0, 0.25f) << x << "," << y;
            EXPECT_EQ(s1, 0.75f) << x << "," << y;
            EXPECT_EQ(got, 0.5f) << x << "," << y;
        }
    }
}

// Learn (ResolveSubresource): a depth-stencil format's stencil plane is its
// own subresource (one mip, one slice: subresource 1). The same average of
// the stored samples applies. Both samples are the clear (0x3C), so the
// average is 0x3C, and the depth plane of the destination stays the 0 it
// was created with. The source is typeless so the stencil plane can be
// read as its own view (R24G8_TYPELESS, DSV D24).
TEST_P(D3D12MetalTest, StencilResolveAveragesTheStoredSamples) {
    constexpr u32 kSize = 4;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr src = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize,
        DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 2);
    const GuestAddr dst = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize,
        DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        1);
    const GuestAddr depth_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr stencil_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr dsvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    const auto& dsv = layout::D3D12_DEPTH_STENCIL_VIEW_DESC[GetParam()];
    h->zero(kDesc, dsv.size);
    h->w32(kDesc + dsv.Format, DXGI_FORMAT_D24_UNORM_S8_UINT);
    h->w32(kDesc + dsv.ViewDimension, D3D12_DSV_DIMENSION_TEXTURE2DMS);
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView,
                 {{src}, {kDesc}, {h->cpu_start(dsvs)}});

    const GuestAddr list = h->list(dev);
    const auto f32_arg = [](float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        return Harness::Arg{bits};
    };
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{h->cpu_start(dsvs)},
                  {D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL},
                  f32_arg(0.5f),
                  {0x3C},
                  {0},
                  {0}});
    h->transition(list, src, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::ResolveSubresource,
                 {{dst},
                  {1},
                  {src},
                  {1},
                  {DXGI_FORMAT_X24_TYPELESS_G8_UINT}});
    h->transition(list, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, depth_out, dst, DXGI_FORMAT_R32_TYPELESS, kSize,
                         kSize, 0);
    h->copy_to_footprint(list, stencil_out, dst, DXGI_FORMAT_R8_TYPELESS, kSize,
                         kSize, 1);
    h->run(dev, queue, list);

    const std::vector<u8> depth = h->read_back(depth_out, 4096);
    const std::vector<u8> stencil = h->read_back(stencil_out, 4096);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            u32 d = 0;
            std::memcpy(&d, depth.data() + y * 256u + x * 4u, 4);
            EXPECT_EQ(d, 0u) << "depth " << x << "," << y;
            EXPECT_EQ(stencil[y * 256u + x], 0x3C) << "stencil " << x << "," << y;
        }
    }
}

// Learn (ResolveSubresource): resolving the depth plane of a combined
// depth-stencil buffer averages that plane and leaves the destination's
// stencil plane alone. Both depth samples are the clear 0.5, whose UNORM24
// encoding is 0x800000 (measured, a05 "D24S8").
TEST_P(D3D12MetalTest, DepthResolveLeavesTheStencilPlane) {
    constexpr u32 kSize = 4;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr src = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize,
        DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        2);
    const GuestAddr dst = h->texture(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSize, kSize,
        DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        1);
    const GuestAddr depth_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr stencil_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr dsvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView,
                 {{src}, {0}, {h->cpu_start(dsvs)}});

    const GuestAddr list = h->list(dev);
    const auto f32_arg = [](float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        return Harness::Arg{bits};
    };
    (void)h->com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                 {{h->cpu_start(dsvs)},
                  {D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL},
                  f32_arg(0.5f),
                  {0x3C},
                  {0},
                  {0}});
    h->transition(list, src, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::ResolveSubresource,
                 {{dst}, {0}, {src}, {0}, {DXGI_FORMAT_D24_UNORM_S8_UINT}});
    h->transition(list, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, depth_out, dst, DXGI_FORMAT_R32_TYPELESS, kSize,
                         kSize, 0);
    h->copy_to_footprint(list, stencil_out, dst, DXGI_FORMAT_R8_TYPELESS, kSize,
                         kSize, 1);
    h->run(dev, queue, list);

    const std::vector<u8> depth = h->read_back(depth_out, 4096);
    const std::vector<u8> stencil = h->read_back(stencil_out, 4096);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            u32 d = 0;
            std::memcpy(&d, depth.data() + y * 256u + x * 4u, 4);
            EXPECT_EQ(d, 0x800000u) << "depth " << x << "," << y;
            EXPECT_EQ(stencil[y * 256u + x], 0) << "stencil " << x << "," << y;
        }
    }
}

// Learn (WriteToSubresource / ReadFromSubresource): the box is in the
// subresource's own texels and an empty box copies nothing. Plane 0 of a
// depth-stencil format is an R32 row (D24 UNORM24 in the low 24 bits,
// measured a05 "D24S8": 0.5 -> 0x800000 and 1 -> 0xFFFFFF; D32 is the float
// bits) and plane 1 is the R8 stencil byte. NV12 plane 0 is the full-size
// luma byte and plane 1 is R8G8 at half size; P010/P016 luma is R16 and
// chroma is R16G16. A write of one plane leaves the other.
TEST_P(D3D12MetalTest, CpuSubresourceTransfersAddressablePlanes) {
    const GuestAddr dev = h->device();
    const GuestAddr src = kData + 0x18000;
    const GuestAddr dst = kData + 0x18200;
    const GuestAddr box_at = kData + 0x18400;
    auto put = [&](GuestAddr at, const void* bytes, usize n) {
        ASSERT_TRUE(h->memory.write_bytes(at, static_cast<const u8*>(bytes), n).is_ok());
    };
    auto take = [&](GuestAddr at, usize n) -> std::vector<u8> {
        std::vector<u8> out(n);
        EXPECT_TRUE(h->memory.read_bytes(at, out.data(), n).is_ok());
        return out;
    };
    auto write_box = [&](u32 left, u32 top, u32 right, u32 bottom) {
        const u32 box[6] = {left, top, 0, right, bottom, 1};
        put(box_at, box, sizeof(box));
    };
    auto write_sub = [&](GuestAddr tex, u32 sub, GuestAddr box, u32 pitch) {
        return h->com(tex, ResourceSlot::WriteToSubresource,
                      {{sub}, {box}, {src}, {pitch}, {0}});
    };
    auto read_sub = [&](GuestAddr tex, u32 sub, GuestAddr box, u32 pitch) {
        return h->com(tex, ResourceSlot::ReadFromSubresource,
                      {{dst}, {pitch}, {0}, {sub}, {box}});
    };
    auto u32_at = [](const std::vector<u8>& bytes, usize offset) {
        u32 v = 0;
        std::memcpy(&v, bytes.data() + offset, 4);
        return v;
    };

    // D24: depth UNORM24 and stencil are separate subresources (one mip, one
    // slice: subresource 0 is depth, 1 is stencil). Row pitch is wider than
    // the row so padding must be left alone.
    const GuestAddr d24 = h->cpu_texture(dev, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, 4,
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ASSERT_NE(d24, 0u);
    constexpr u32 kDepthPitch = 20;
    constexpr u32 kStencilPitch = 8;
    u8 depth_src[4 * kDepthPitch];
    std::memset(depth_src, 0xA5, sizeof(depth_src));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 texel = 0x800000u;
            std::memcpy(depth_src + y * kDepthPitch + x * 4, &texel, 4);
        }
    }
    put(src, depth_src, sizeof(depth_src));
    ASSERT_EQ(write_sub(d24, 0, 0, kDepthPitch), S_OK);
    u8 stencil_src[4 * kStencilPitch];
    std::memset(stencil_src, 0xA5, sizeof(stencil_src));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) stencil_src[y * kStencilPitch + x] = 0x5A;
    }
    put(src, stencil_src, sizeof(stencil_src));
    ASSERT_EQ(write_sub(d24, 1, 0, kStencilPitch), S_OK);

    std::vector<u8> depth_dst(sizeof(depth_src), 0xEE);
    put(dst, depth_dst.data(), depth_dst.size());
    ASSERT_EQ(read_sub(d24, 0, 0, kDepthPitch), S_OK);
    depth_dst = take(dst, sizeof(depth_src));
    std::vector<u8> stencil_dst(sizeof(stencil_src), 0xEE);
    put(dst, stencil_dst.data(), stencil_dst.size());
    ASSERT_EQ(read_sub(d24, 1, 0, kStencilPitch), S_OK);
    stencil_dst = take(dst, sizeof(stencil_src));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            EXPECT_EQ(u32_at(depth_dst, y * kDepthPitch + x * 4), 0x800000u)
                << x << "," << y;
            EXPECT_EQ(stencil_dst[y * kStencilPitch + x], 0x5A) << x << "," << y;
        }
        EXPECT_EQ(u32_at(depth_dst, y * kDepthPitch + 16), 0xEEEEEEEEu) << y;
        EXPECT_EQ(u32_at(stencil_dst, y * kStencilPitch + 4), 0xEEEEEEEEu) << y;
    }

    // A one-texel depth write must not touch stencil or the other depth texels.
    const u32 one = 0xFFFFFFu;
    put(src, &one, 4);
    write_box(1, 2, 2, 3);
    ASSERT_EQ(write_sub(d24, 0, box_at, 4), S_OK);
    const u8 stencil_one = 0x22;
    put(src, &stencil_one, 1);
    write_box(3, 0, 4, 1);
    ASSERT_EQ(write_sub(d24, 1, box_at, 1), S_OK);
    put(dst, std::vector<u8>(sizeof(depth_src), 0xEE).data(), sizeof(depth_src));
    ASSERT_EQ(read_sub(d24, 0, 0, kDepthPitch), S_OK);
    depth_dst = take(dst, sizeof(depth_src));
    put(dst, std::vector<u8>(sizeof(stencil_src), 0xEE).data(), sizeof(stencil_src));
    ASSERT_EQ(read_sub(d24, 1, 0, kStencilPitch), S_OK);
    stencil_dst = take(dst, sizeof(stencil_src));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 expect_depth = (x == 1 && y == 2) ? 0xFFFFFFu : 0x800000u;
            const u8 expect_stencil = (x == 3 && y == 0) ? 0x22 : 0x5A;
            EXPECT_EQ(u32_at(depth_dst, y * kDepthPitch + x * 4), expect_depth)
                << x << "," << y;
            EXPECT_EQ(stencil_dst[y * kStencilPitch + x], expect_stencil)
                << x << "," << y;
        }
    }
    write_box(0, 0, 0, 1);  // empty: right <= left
    ASSERT_EQ(write_sub(d24, 0, box_at, 4), S_OK);
    write_box(0, 0, 5, 1);  // past the plane
    ASSERT_EQ(write_sub(d24, 0, box_at, 4), E_INVALIDARG);
    const u32 still = u32_at(depth_dst, 0);
    put(dst, std::vector<u8>(4, 0).data(), 4);
    ASSERT_EQ(read_sub(d24, 0, 0, kDepthPitch), S_OK);
    EXPECT_EQ(u32_at(take(dst, 4), 0), still);

    // D32 plane 0 is the float itself, not UNORM24. 0.5f bits must survive.
    const GuestAddr d32 = h->cpu_texture(
        dev, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 2, 2,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ASSERT_NE(d32, 0u);
    const u32 floats[4] = {0x3E800000u, 0x3F000000u, 0x3F400000u, 0x3F800000u};
    u8 d32_src[2 * 16];
    std::memset(d32_src, 0xA5, sizeof(d32_src));
    std::memcpy(d32_src + 0, floats + 0, 8);
    std::memcpy(d32_src + 16, floats + 2, 8);
    put(src, d32_src, sizeof(d32_src));
    ASSERT_EQ(write_sub(d32, 0, 0, 16), S_OK);
    const u8 d32_stencil[2 * 4] = {0x11, 0x22, 0xA5, 0xA5, 0x33, 0x44, 0xA5, 0xA5};
    put(src, d32_stencil, sizeof(d32_stencil));
    ASSERT_EQ(write_sub(d32, 1, 0, 4), S_OK);
    put(dst, std::vector<u8>(sizeof(d32_src), 0xEE).data(), sizeof(d32_src));
    ASSERT_EQ(read_sub(d32, 0, 0, 16), S_OK);
    const std::vector<u8> d32_back = take(dst, sizeof(d32_src));
    EXPECT_EQ(u32_at(d32_back, 0), 0x3E800000u);
    EXPECT_EQ(u32_at(d32_back, 4), 0x3F000000u);
    EXPECT_EQ(u32_at(d32_back, 16), 0x3F400000u);
    EXPECT_EQ(u32_at(d32_back, 20), 0x3F800000u);
    EXPECT_EQ(u32_at(d32_back, 8), 0xEEEEEEEEu);
    put(dst, std::vector<u8>(8, 0xEE).data(), 8);
    ASSERT_EQ(read_sub(d32, 1, 0, 4), S_OK);
    const std::vector<u8> d32_s = take(dst, 8);
    EXPECT_EQ(d32_s[0], 0x11);
    EXPECT_EQ(d32_s[1], 0x22);
    EXPECT_EQ(d32_s[4], 0x33);
    EXPECT_EQ(d32_s[5], 0x44);
    EXPECT_EQ(d32_s[2], 0xEE);

    // NV12 plane 0 is the full-size luma byte (Learn DXGI_FORMAT: Y -> R8).
    // Plane 1 is R8G8 at half size. A write of either plane leaves the other.
    const GuestAddr nv12 = h->cpu_texture(dev, DXGI_FORMAT_NV12, 4, 4, 0);
    ASSERT_NE(nv12, 0u);
    constexpr u32 kLumaPitch = 8;
    u8 luma[4 * kLumaPitch];
    std::memset(luma, 0xA5, sizeof(luma));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            luma[y * kLumaPitch + x] = static_cast<u8>(0x10 + y * 4 + x);
        }
    }
    put(src, luma, sizeof(luma));
    ASSERT_EQ(write_sub(nv12, 0, 0, 3), E_INVALIDARG);  // pitch shorter than the row
    ASSERT_EQ(write_sub(nv12, 0, 0, kLumaPitch), S_OK);
    u8 chroma[2 * 8];
    std::memset(chroma, 0xA5, sizeof(chroma));
    const u8 chroma_texels[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    std::memcpy(chroma + 0, chroma_texels + 0, 4);
    std::memcpy(chroma + 8, chroma_texels + 4, 4);
    put(src, chroma, sizeof(chroma));
    ASSERT_EQ(write_sub(nv12, 1, 0, 8), S_OK);
    // The chroma write must not change luma, including the unread row padding.
    put(dst, std::vector<u8>(sizeof(luma), 0xEE).data(), sizeof(luma));
    ASSERT_EQ(read_sub(nv12, 0, 0, kLumaPitch), S_OK);
    std::vector<u8> luma_back = take(dst, sizeof(luma));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            EXPECT_EQ(luma_back[y * kLumaPitch + x], static_cast<u8>(0x10 + y * 4 + x))
                << x << "," << y;
        }
        EXPECT_EQ(u32_at(luma_back, y * kLumaPitch + 4), 0xEEEEEEEEu) << y;
    }
    put(dst, std::vector<u8>(sizeof(chroma), 0xEE).data(), sizeof(chroma));
    ASSERT_EQ(read_sub(nv12, 1, 0, 8), S_OK);
    const std::vector<u8> chroma_back = take(dst, sizeof(chroma));
    EXPECT_EQ(chroma_back[0], 0x10);
    EXPECT_EQ(chroma_back[1], 0x20);
    EXPECT_EQ(chroma_back[2], 0x30);
    EXPECT_EQ(chroma_back[3], 0x40);
    EXPECT_EQ(chroma_back[8], 0x50);
    EXPECT_EQ(chroma_back[9], 0x60);
    EXPECT_EQ(chroma_back[10], 0x70);
    EXPECT_EQ(chroma_back[11], 0x80);
    EXPECT_EQ(chroma_back[4], 0xEE);
    const u8 luma_one = 0x7F;
    put(src, &luma_one, 1);
    write_box(1, 2, 2, 3);
    ASSERT_EQ(write_sub(nv12, 0, box_at, 1), S_OK);
    put(dst, std::vector<u8>(sizeof(chroma), 0xEE).data(), sizeof(chroma));
    ASSERT_EQ(read_sub(nv12, 1, 0, 8), S_OK);
    const std::vector<u8> chroma_after_luma = take(dst, sizeof(chroma));
    EXPECT_EQ(chroma_after_luma[0], 0x10);
    EXPECT_EQ(chroma_after_luma[8], 0x50);
    EXPECT_EQ(chroma_after_luma[4], 0xEE);
    put(dst, std::vector<u8>(sizeof(luma), 0xEE).data(), sizeof(luma));
    ASSERT_EQ(read_sub(nv12, 0, 0, kLumaPitch), S_OK);
    luma_back = take(dst, sizeof(luma));
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 expect = (x == 1 && y == 2) ? 0x7F : static_cast<u8>(0x10 + y * 4 + x);
            EXPECT_EQ(luma_back[y * kLumaPitch + x], expect) << x << "," << y;
        }
    }
    const u8 chroma_box[2] = {0xAB, 0xCD};
    put(src, chroma_box, 2);
    write_box(1, 1, 2, 2);
    ASSERT_EQ(write_sub(nv12, 1, box_at, 2), S_OK);
    put(dst, std::vector<u8>(2, 0).data(), 2);
    write_box(0, 0, 1, 1);
    ASSERT_EQ(read_sub(nv12, 1, box_at, 2), S_OK);
    EXPECT_EQ(take(dst, 2)[0], 0x10);
    EXPECT_EQ(take(dst, 2)[1], 0x20);
    write_box(1, 1, 2, 2);
    ASSERT_EQ(read_sub(nv12, 1, box_at, 2), S_OK);
    EXPECT_EQ(take(dst, 2)[0], 0xAB);
    EXPECT_EQ(take(dst, 2)[1], 0xCD);
    // The chroma box must not change the luma texel written above.
    put(dst, std::vector<u8>(1, 0).data(), 1);
    write_box(1, 2, 2, 3);
    ASSERT_EQ(read_sub(nv12, 0, box_at, 1), S_OK);
    EXPECT_EQ(take(dst, 1)[0], 0x7F);
    write_box(0, 0, 0, 1);  // empty: right <= left
    ASSERT_EQ(write_sub(nv12, 0, box_at, 1), S_OK);
    write_box(0, 0, 5, 1);  // past the luma plane
    ASSERT_EQ(write_sub(nv12, 0, box_at, 1), E_INVALIDARG);
    write_box(2, 0, 3, 1);  // inside luma, past the half-width chroma plane
    ASSERT_EQ(write_sub(nv12, 1, box_at, 2), E_INVALIDARG);
    ASSERT_EQ(write_sub(nv12, 0, box_at, 1), S_OK);

    // P010 / P016 luma is a 16-bit sample per texel (Learn: R16). Chroma is
    // one R16G16 texel per 2x2 block. Pitch padding and the other plane stay.
    u8 p010_luma[2 * 12];
    std::memset(p010_luma, 0xA5, sizeof(p010_luma));
    for (u32 i = 0; i < 8; ++i) p010_luma[i] = static_cast<u8>(0x80 + i);
    for (u32 i = 0; i < 8; ++i) p010_luma[12 + i] = static_cast<u8>(0x90 + i);
    const u8 p010_row[12] = {0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44,
                             0xA5, 0xA5, 0xA5, 0xA5};
    for (const u32 format : {DXGI_FORMAT_P010, DXGI_FORMAT_P016}) {
        const GuestAddr tex = h->cpu_texture(dev, format, 4, 2, 0);
        ASSERT_NE(tex, 0u) << format;
        put(src, p010_luma, sizeof(p010_luma));
        ASSERT_EQ(write_sub(tex, 0, 0, 12), S_OK) << format;
        put(src, p010_row, sizeof(p010_row));
        ASSERT_EQ(write_sub(tex, 1, 0, 12), S_OK) << format;
        put(dst, std::vector<u8>(sizeof(p010_luma), 0xEE).data(), sizeof(p010_luma));
        ASSERT_EQ(read_sub(tex, 0, 0, 12), S_OK) << format;
        const std::vector<u8> luma_plane = take(dst, sizeof(p010_luma));
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_EQ(luma_plane[i], p010_luma[i]) << format << " row0 " << i;
            EXPECT_EQ(luma_plane[12 + i], p010_luma[12 + i]) << format << " row1 " << i;
        }
        for (u32 i = 8; i < 12; ++i) {
            EXPECT_EQ(luma_plane[i], 0xEE) << format;
            EXPECT_EQ(luma_plane[12 + i], 0xEE) << format;
        }
        put(dst, std::vector<u8>(12, 0xEE).data(), 12);
        ASSERT_EQ(read_sub(tex, 1, 0, 12), S_OK) << format;
        const std::vector<u8> back = take(dst, 12);
        for (usize i = 0; i < 8; ++i) EXPECT_EQ(back[i], p010_row[i]) << format << " " << i;
        for (usize i = 8; i < 12; ++i) EXPECT_EQ(back[i], 0xEE) << format;
        const u8 luma_texel[2] = {0xAB, 0xCD};
        put(src, luma_texel, 2);
        write_box(1, 0, 2, 1);
        ASSERT_EQ(write_sub(tex, 0, box_at, 2), S_OK) << format;
        put(dst, std::vector<u8>(12, 0).data(), 12);
        ASSERT_EQ(read_sub(tex, 1, 0, 12), S_OK) << format;
        const std::vector<u8> chroma_kept = take(dst, 8);
        for (usize i = 0; i < 8; ++i) EXPECT_EQ(chroma_kept[i], p010_row[i]) << format;
        put(dst, std::vector<u8>(2, 0).data(), 2);
        write_box(0, 0, 1, 1);
        ASSERT_EQ(read_sub(tex, 0, box_at, 2), S_OK) << format;
        EXPECT_EQ(take(dst, 2)[0], 0x80) << format;
        EXPECT_EQ(take(dst, 2)[1], 0x81) << format;
        write_box(1, 0, 2, 1);
        ASSERT_EQ(read_sub(tex, 0, box_at, 2), S_OK) << format;
        EXPECT_EQ(take(dst, 2)[0], 0xAB) << format;
        EXPECT_EQ(take(dst, 2)[1], 0xCD) << format;
    }

    // CopyTextureRegion of plane 0 moves luma and leaves chroma. Plane 1
    // does the opposite. One mip, one slice: subresource 0 is luma, 1 is chroma.
    const GuestAddr queue = h->queue(dev);
    const GuestAddr copy_src = h->cpu_texture(dev, DXGI_FORMAT_NV12, 4, 4, 0);
    const GuestAddr copy_dst = h->cpu_texture(dev, DXGI_FORMAT_NV12, 4, 4, 0);
    ASSERT_NE(copy_src, 0u);
    ASSERT_NE(copy_dst, 0u);
    u8 src_luma[16];
    u8 dst_luma[16];
    for (u32 i = 0; i < 16; ++i) {
        src_luma[i] = static_cast<u8>(0xA0 + i);
        dst_luma[i] = 0x44;
    }
    put(src, src_luma, sizeof(src_luma));
    ASSERT_EQ(write_sub(copy_src, 0, 0, 4), S_OK);
    put(src, dst_luma, sizeof(dst_luma));
    ASSERT_EQ(write_sub(copy_dst, 0, 0, 4), S_OK);
    const u8 src_chroma[8] = {0x22, 0x33, 0x22, 0x33, 0x22, 0x33, 0x22, 0x33};
    const u8 dst_chroma[8] = {0x55, 0x66, 0x55, 0x66, 0x55, 0x66, 0x55, 0x66};
    put(src, src_chroma, sizeof(src_chroma));
    ASSERT_EQ(write_sub(copy_src, 1, 0, 4), S_OK);
    put(src, dst_chroma, sizeof(dst_chroma));
    ASSERT_EQ(write_sub(copy_dst, 1, 0, 4), S_OK);
    const GuestAddr list = h->list(dev);
    h->transition(list, copy_src, D3D12_RESOURCE_STATE_COMMON,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->transition(list, copy_dst, D3D12_RESOURCE_STATE_COMMON,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    h->copy_subresource(list, copy_dst, 0, copy_src, 0);
    h->transition(list, copy_dst, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    const GuestAddr luma_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr chroma_out = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    h->copy_to_footprint(list, luma_out, copy_dst, DXGI_FORMAT_R8_TYPELESS, 4, 4, 0);
    h->copy_to_footprint(list, chroma_out, copy_dst, DXGI_FORMAT_R8G8_TYPELESS, 2, 2, 1);
    h->run(dev, queue, list);
    const std::vector<u8> copied_luma = h->read_back(luma_out, 1024);
    const std::vector<u8> copied_chroma = h->read_back(chroma_out, 1024);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            EXPECT_EQ(copied_luma[y * 256u + x], src_luma[y * 4 + x]) << x << "," << y;
        }
    }
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(copied_chroma[i], dst_chroma[i]) << i;
        EXPECT_EQ(copied_chroma[256u + i], dst_chroma[4 + i]) << i;
    }
    put(dst, std::vector<u8>(16, 0).data(), 16);
    ASSERT_EQ(read_sub(copy_dst, 0, 0, 4), S_OK);
    EXPECT_EQ(take(dst, 16), std::vector<u8>(src_luma, src_luma + 16));
    put(dst, std::vector<u8>(8, 0).data(), 8);
    ASSERT_EQ(read_sub(copy_dst, 1, 0, 4), S_OK);
    EXPECT_EQ(take(dst, 8), std::vector<u8>(dst_chroma, dst_chroma + 8));

    const GuestAddr list2 = h->list(dev);
    h->transition(list2, copy_dst, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    h->copy_subresource(list2, copy_dst, 1, copy_src, 1);
    h->transition(list2, copy_dst, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    const GuestAddr luma_out2 = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    const GuestAddr chroma_out2 = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                            D3D12_RESOURCE_STATE_COPY_DEST, 4096);
    h->copy_to_footprint(list2, luma_out2, copy_dst, DXGI_FORMAT_R8_TYPELESS, 4, 4, 0);
    h->copy_to_footprint(list2, chroma_out2, copy_dst, DXGI_FORMAT_R8G8_TYPELESS, 2, 2, 1);
    h->run(dev, queue, list2);
    const std::vector<u8> luma_after = h->read_back(luma_out2, 1024);
    const std::vector<u8> chroma_after = h->read_back(chroma_out2, 1024);
    EXPECT_EQ(luma_after[0], src_luma[0]);
    EXPECT_EQ(luma_after[3 * 256u + 3], src_luma[15]);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(chroma_after[i], src_chroma[i]) << i;
        EXPECT_EQ(chroma_after[256u + i], src_chroma[4 + i]) << i;
    }
}

// Learn (D3D12_SHADER_COMPONENT_MAPPING) on a Texture3D SRV. The volume and
// the mappings are d3d12_a03_descriptors_probe.c s_tex3d; the expected words
// are that probe's tex3d lines on WARP (tests/oracle_data/
// d3d12_a03_descriptors_probe.expected), not a Vitreo run.
// Slice 0 is (1, 2, 4, 8) and slice 1 is (16, 32, 64, 128), RGBA32F.
// wzyx is memory components 3,2,1,0. gr01 is component 1, component 0,
// force 0, force 1 (1.0f on a float format). Dimensions are not remapped.
TEST_P(D3D12MetalTest, Texture3DShaderResourceViewAppliesComponentMapping) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv_range;
    srv_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.num_descriptors = 1;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.offset_in_descriptors_from_table_start =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    table.ranges = {srv_range, uav_range};
    rs.parameters = {table};
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

    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_tex3d, sizeof(g_sh_cs_tex3d))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_tex3d));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc, hp.size);
    h->zero(kDesc2, rd.size);
    h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_CUSTOM);
    h->w32(kDesc + hp.CPUPageProperty, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
    h->w32(kDesc + hp.MemoryPoolPreference, D3D12_MEMORY_POOL_L0);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    h->w64(kDesc2 + rd.Width, 1);
    h->w32(kDesc2 + rd.Height, 1);
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 2).is_ok());
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
    const GuestAddr volume =
        h->made(h->com(dev, DeviceSlot::CreateCommittedResource,
                       {{kDesc},
                        {0},
                        {kDesc2},
                        {D3D12_RESOURCE_STATE_COMMON},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    const float texels[8] = {1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f, 128.f};
    const GuestAddr src = kData + 0x18000;
    ASSERT_TRUE(h->memory
                    .write_bytes(src, reinterpret_cast<const u8*>(texels),
                                 sizeof(texels))
                    .is_ok());
    ASSERT_EQ(h->com(volume, ResourceSlot::WriteToSubresource,
                     {{0}, {0}, {src}, {16}, {16}}),
              S_OK);

    const GuestAddr data =
        h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 srv_cpu = h->cpu_start(heap);
    const u64 uav_cpu = srv_cpu + increment;

    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 16);
    h->w32(kDesc + ul.Buffer_StructureByteStride, 16);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {uav_cpu}});

    const auto encode = [](u32 s0, u32 s1, u32 s2, u32 s3) {
        return (s0 & D3D12_SHADER_COMPONENT_MAPPING_MASK) |
               ((s1 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << D3D12_SHADER_COMPONENT_MAPPING_SHIFT) |
               ((s2 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 2)) |
               ((s3 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 3)) |
               D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    };
    struct Case {
        const char* name;
        bool null_desc;
        u32 mapping;
        u32 dims[4];
        u32 slice0[4];
        u32 slice1[4];
    };
    const Case cases[] = {
        {"NULL-desc", true, 0, {1, 1, 2, 1},
         {0x3f800000u, 0x40000000u, 0x40800000u, 0x41000000u},
         {0x41800000u, 0x42000000u, 0x42800000u, 0x43000000u}},
        {"identity", false, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
         {1, 1, 2, 1},
         {0x3f800000u, 0x40000000u, 0x40800000u, 0x41000000u},
         {0x41800000u, 0x42000000u, 0x42800000u, 0x43000000u}},
        {"wzyx", false, encode(3, 2, 1, 0), {1, 1, 2, 1},
         {0x41000000u, 0x40800000u, 0x40000000u, 0x3f800000u},
         {0x43000000u, 0x42800000u, 0x42000000u, 0x41800000u}},
        {"gr01", false, encode(1, 0, 4, 5), {1, 1, 2, 1},
         {0x40000000u, 0x3f800000u, 0x00000000u, 0x3f800000u},
         {0x42000000u, 0x41800000u, 0x00000000u, 0x3f800000u}},
    };

    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    bool uav_is_copy_source = false;
    for (const Case& c : cases) {
        if (c.null_desc) {
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {0}, {srv_cpu}});
        } else {
            h->zero(kDesc, sl.size);
            h->w32(kDesc + sl.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
            h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE3D);
            h->w32(kDesc + sl.Shader4ComponentMapping, c.mapping);
            h->w32(kDesc + sl.Texture3D_MostDetailedMip, 0);
            h->w32(kDesc + sl.Texture3D_MipLevels, 1);
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {kDesc}, {srv_cpu}});
        }
        const GuestAddr list = h->list(dev, pso);
        if (!uav_is_copy_source) {
            h->transition(list, volume, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            h->transition(list, data, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature,
                     {{root}});
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{0}, Harness::wide(h->gpu_start(heap))});
        (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
        h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                     {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                      Harness::wide(48)});
        h->run(dev, queue, list);
        uav_is_copy_source = true;

        const std::vector<u8> out = h->read_back(readback, 48);
        u32 words[12] = {};
        for (int i = 0; i < 12; ++i) {
            std::memcpy(words + i, out.data() + static_cast<usize>(i) * 4u, 4);
        }
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(words[i], c.dims[i]) << c.name << " dim " << i;
            EXPECT_EQ(words[4 + i], c.slice0[i]) << c.name << " slice0 " << i;
            EXPECT_EQ(words[8 + i], c.slice1[i]) << c.name << " slice1 " << i;
        }
    }
}

// D3D12_FORMAT_SUPPORT1_TEXTURE3D of B8G8R8X8 (d3d12_a41_format_sweep, WARP
// and RTX: s1 bit 0x40): a volume of it reads alpha 1 (DXGI_FORMAT "X8"),
// also through a component mapping that selects alpha.
TEST_P(D3D12MetalTest, Texture3DOfB8G8R8X8ReadsOpaqueAlpha) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv_range;
    srv_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.num_descriptors = 1;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.offset_in_descriptors_from_table_start =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    table.ranges = {srv_range, uav_range};
    rs.parameters = {table};
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

    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_tex3d, sizeof(g_sh_cs_tex3d))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_tex3d));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc, hp.size);
    h->zero(kDesc2, rd.size);
    h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_CUSTOM);
    h->w32(kDesc + hp.CPUPageProperty, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
    h->w32(kDesc + hp.MemoryPoolPreference, D3D12_MEMORY_POOL_L0);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    h->w64(kDesc2 + rd.Width, 1);
    h->w32(kDesc2 + rd.Height, 1);
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 2).is_ok());
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_B8G8R8X8_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
    const GuestAddr volume =
        h->made(h->com(dev, DeviceSlot::CreateCommittedResource,
                       {{kDesc},
                        {0},
                        {kDesc2},
                        {D3D12_RESOURCE_STATE_COMMON},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    // B, G, R, X per texel; X is never read.
    const u8 texels[8] = {0x10, 0x20, 0x30, 0x00, 0x40, 0x50, 0x60, 0x7f};
    const GuestAddr src = kData + 0x18000;
    ASSERT_TRUE(h->memory.write_bytes(src, texels, sizeof(texels)).is_ok());
    ASSERT_EQ(h->com(volume, ResourceSlot::WriteToSubresource,
                     {{0}, {0}, {src}, {4}, {4}}),
              S_OK);

    const GuestAddr data =
        h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 srv_cpu = h->cpu_start(heap);
    const u64 uav_cpu = srv_cpu + increment;

    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 16);
    h->w32(kDesc + ul.Buffer_StructureByteStride, 16);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {uav_cpu}});

    const auto encode = [](u32 s0, u32 s1, u32 s2, u32 s3) {
        return (s0 & D3D12_SHADER_COMPONENT_MAPPING_MASK) |
               ((s1 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << D3D12_SHADER_COMPONENT_MAPPING_SHIFT) |
               ((s2 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 2)) |
               ((s3 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 3)) |
               D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    };
    struct Case {
        const char* name;
        bool null_desc;
        u32 mapping;
        u32 dims[4];
        u32 slice0[4];
        u32 slice1[4];
    };
    const u32 one = 0x3f800000u;
    const Case cases[] = {
        {"NULL-desc", true, 0, {1, 1, 2, 1},
         {std::bit_cast<u32>(48.0f / 255.0f), std::bit_cast<u32>(32.0f / 255.0f), std::bit_cast<u32>(16.0f / 255.0f), one}, {std::bit_cast<u32>(96.0f / 255.0f), std::bit_cast<u32>(80.0f / 255.0f), std::bit_cast<u32>(64.0f / 255.0f), one}},
        {"wzyx", false, encode(3, 2, 1, 0), {1, 1, 2, 1},
         {one, std::bit_cast<u32>(16.0f / 255.0f), std::bit_cast<u32>(32.0f / 255.0f), std::bit_cast<u32>(48.0f / 255.0f)}, {one, std::bit_cast<u32>(64.0f / 255.0f), std::bit_cast<u32>(80.0f / 255.0f), std::bit_cast<u32>(96.0f / 255.0f)}},
    };

    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    bool uav_is_copy_source = false;
    for (const Case& c : cases) {
        if (c.null_desc) {
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {0}, {srv_cpu}});
        } else {
            h->zero(kDesc, sl.size);
            h->w32(kDesc + sl.Format, DXGI_FORMAT_B8G8R8X8_UNORM);
            h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE3D);
            h->w32(kDesc + sl.Shader4ComponentMapping, c.mapping);
            h->w32(kDesc + sl.Texture3D_MostDetailedMip, 0);
            h->w32(kDesc + sl.Texture3D_MipLevels, 1);
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {kDesc}, {srv_cpu}});
        }
        const GuestAddr list = h->list(dev, pso);
        if (!uav_is_copy_source) {
            h->transition(list, volume, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            h->transition(list, data, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature,
                     {{root}});
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{0}, Harness::wide(h->gpu_start(heap))});
        (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
        h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                     {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                      Harness::wide(48)});
        h->run(dev, queue, list);
        uav_is_copy_source = true;

        const std::vector<u8> out = h->read_back(readback, 48);
        u32 words[12] = {};
        for (int i = 0; i < 12; ++i) {
            std::memcpy(words + i, out.data() + static_cast<usize>(i) * 4u, 4);
        }
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(words[i], c.dims[i]) << c.name << " dim " << i;
            EXPECT_EQ(words[4 + i], c.slice0[i]) << c.name << " slice0 " << i;
            EXPECT_EQ(words[8 + i], c.slice1[i]) << c.name << " slice1 " << i;
        }
    }
}

// D3D12_FORMAT_SUPPORT1_TEXTURE3D of B4G4R4A4 (d3d12_a41_format_sweep, WARP
// and RTX: s1 bit 0x40): a volume of it reads R, G, B, A from bits 8-11,
// 4-7, 0-3, 12-15 (Learn DXGI_FORMAT: components named from the lowest
// bits, B first), although its storage is Metal ABGR4, also through a
// component mapping.
TEST_P(D3D12MetalTest, Texture3DOfB4G4R4A4ReadsDxgiComponents) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv_range;
    srv_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.num_descriptors = 1;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.offset_in_descriptors_from_table_start =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    table.ranges = {srv_range, uav_range};
    rs.parameters = {table};
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

    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_tex3d, sizeof(g_sh_cs_tex3d))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_tex3d));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc, hp.size);
    h->zero(kDesc2, rd.size);
    h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_CUSTOM);
    h->w32(kDesc + hp.CPUPageProperty, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
    h->w32(kDesc + hp.MemoryPoolPreference, D3D12_MEMORY_POOL_L0);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    h->w64(kDesc2 + rd.Width, 1);
    h->w32(kDesc2 + rd.Height, 1);
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 2).is_ok());
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_B4G4R4A4_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
    const GuestAddr volume =
        h->made(h->com(dev, DeviceSlot::CreateCommittedResource,
                       {{kDesc},
                        {0},
                        {kDesc2},
                        {D3D12_RESOURCE_STATE_COMMON},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    // 0xARGB nibbles per texel, B lowest: (R, G, B, A) = (3, 2, 1, 4) and
    // (7, 6, 5, 8).
    const u8 texels[4] = {0x21, 0x43, 0x65, 0x87};
    const GuestAddr src = kData + 0x18000;
    ASSERT_TRUE(h->memory.write_bytes(src, texels, sizeof(texels)).is_ok());
    ASSERT_EQ(h->com(volume, ResourceSlot::WriteToSubresource,
                     {{0}, {0}, {src}, {2}, {2}}),
              S_OK);

    const GuestAddr data =
        h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 srv_cpu = h->cpu_start(heap);
    const u64 uav_cpu = srv_cpu + increment;

    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 16);
    h->w32(kDesc + ul.Buffer_StructureByteStride, 16);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {uav_cpu}});

    const auto encode = [](u32 s0, u32 s1, u32 s2, u32 s3) {
        return (s0 & D3D12_SHADER_COMPONENT_MAPPING_MASK) |
               ((s1 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << D3D12_SHADER_COMPONENT_MAPPING_SHIFT) |
               ((s2 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 2)) |
               ((s3 & D3D12_SHADER_COMPONENT_MAPPING_MASK)
                << (D3D12_SHADER_COMPONENT_MAPPING_SHIFT * 3)) |
               D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    };
    struct Case {
        const char* name;
        bool null_desc;
        u32 mapping;
        u32 dims[4];
        u32 slice0[4];
        u32 slice1[4];
    };
    const u32 one = 0x3f800000u;
    const auto n = [](u32 v) { return std::bit_cast<u32>(static_cast<float>(v) / 15.0f); };
    const Case cases[] = {
        {"NULL-desc", true, 0, {1, 1, 2, 1}, {n(3), n(2), n(1), n(4)}, {n(7), n(6), n(5), n(8)}},
        {"wzyx", false, encode(3, 2, 1, 0), {1, 1, 2, 1},
         {n(4), n(1), n(2), n(3)}, {n(8), n(5), n(6), n(7)}},
        {"x01w", false, encode(0, 4, 5, 3), {1, 1, 2, 1},
         {n(3), 0u, one, n(4)}, {n(7), 0u, one, n(8)}},
    };

    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    bool uav_is_copy_source = false;
    for (const Case& c : cases) {
        if (c.null_desc) {
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {0}, {srv_cpu}});
        } else {
            h->zero(kDesc, sl.size);
            h->w32(kDesc + sl.Format, DXGI_FORMAT_B4G4R4A4_UNORM);
            h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE3D);
            h->w32(kDesc + sl.Shader4ComponentMapping, c.mapping);
            h->w32(kDesc + sl.Texture3D_MostDetailedMip, 0);
            h->w32(kDesc + sl.Texture3D_MipLevels, 1);
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{volume}, {kDesc}, {srv_cpu}});
        }
        const GuestAddr list = h->list(dev, pso);
        if (!uav_is_copy_source) {
            h->transition(list, volume, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            h->transition(list, data, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature,
                     {{root}});
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{0}, Harness::wide(h->gpu_start(heap))});
        (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
        h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                     {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                      Harness::wide(48)});
        h->run(dev, queue, list);
        uav_is_copy_source = true;

        const std::vector<u8> out = h->read_back(readback, 48);
        u32 words[12] = {};
        for (int i = 0; i < 12; ++i) {
            std::memcpy(words + i, out.data() + static_cast<usize>(i) * 4u, 4);
        }
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(words[i], c.dims[i]) << c.name << " dim " << i;
            EXPECT_EQ(words[4 + i], c.slice0[i]) << c.name << " slice0 " << i;
            EXPECT_EQ(words[8 + i], c.slice1[i]) << c.name << " slice1 " << i;
        }
    }
}

// Learn (D3D12_BUFFER_UAV): FirstElement is a UINT64 element index. A raw
// view's element is 4 bytes (D3D12_BUFFER_UAV_FLAG_RAW, the same stride
// bind_view uses; a structured view uses StructureByteStride in that same
// product). Byte offset = placement + FirstElement * 4. Here the resource
// is a reserved buffer (no 4 GiB allocation: one mapped 64 KiB tile) and
// FirstElement is the element at 4 GiB plus 4, so the view starts at
// 4 GiB + 16, which is 16-byte aligned. cs_madd is v * 3 + 7 over that
// view (the same shader and constants as the table-UAV test). A truncated
// 32-bit offset would transform byte 16, and binding 0 would transform
// the start; the dwords at 4 GiB + 16 would stay the uploaded values.
TEST_P(D3D12MetalTest, RawViewAtFourGiBPlusAlignedElement) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse buffers";
    }
    constexpr u64 kFourGiB = 0x100000000ull;
    constexpr u64 kTile = 65536ull;
    constexpr u64 kExtraElements = 4ull;  // 16 bytes past 4 GiB
    constexpr u64 kByteOffset = kFourGiB + kExtraElements * 4ull;
    constexpr u64 kFirstElement = kFourGiB / 4ull + kExtraElements;
    constexpr u32 kDwords = 64u;
    constexpr u64 kWidth = kFourGiB + kTile;
    static_assert(kByteOffset == kFirstElement * 4ull);
    static_assert(kByteOffset + kDwords * 4ull <= kWidth);
    if (device.max_buffer_length() < kWidth) {
        GTEST_SKIP() << "Metal cannot address a buffer past 4 GiB";
    }

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

    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, kWidth);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    h->w32(kDesc2 + rd.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr data =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));

    constexpr GuestAddr kCoord = kData + 0x16000;
    constexpr GuestAddr kRegion = kData + 0x16040;
    constexpr GuestAddr kFlag = kData + 0x16080;
    constexpr GuestAddr kHeapOff = kData + 0x160C0;
    h->w32(kCoord + 0, static_cast<u32>(kFourGiB / kTile));
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 0);
    h->w32(kRegion + 8, 0);
    h->w32(kRegion + 12, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{data},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap_mem},
                      {1},
                      {kFlag},
                      {kHeapOff},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);

    const GuestAddr upload = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        kDwords * 4u);
    const GuestAddr readback = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kDwords * 4u);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < kDwords; ++i) h->w32(mapped + i * 4u, i);
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    const GuestAddr descriptors =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w64(kDesc + ul.Buffer_FirstElement, kFirstElement);
    h->w32(kDesc + ul.Buffer_NumElements, kDwords);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {h->cpu_start(descriptors)}});

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(kByteOffset), {upload}, Harness::wide(0),
                  Harness::wide(kDwords * 4u)});
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, descriptors);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    h->w32(kOut2, 3);
    h->w32(kOut2 + 4, 7);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRoot32BitConstants,
                 {{0}, {2}, {kOut2}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(descriptors))});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(kByteOffset),
                  Harness::wide(kDwords * 4u)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, kDwords * 4u);
    for (u32 i = 0; i < kDwords; ++i) {
        u32 v = 0;
        std::memcpy(&v, out.data() + i * 4u, 4);
        EXPECT_EQ(v, i * 3u + 7u) << "dword " << i;
    }
}

// Reserved Texture2D on Metal placement sparse memory, packed tail. Spec
// (D3D11.3 5.9.2.8.5, Learn D3D12_TILED_RESOURCES_TIER_2): mips of at least
// one standard tile in every dimension are never packed, the packed mips
// follow them, and once a slice's packed tiles are mapped its packed mips
// hold data. A NULL tile reads back zeros through CopyTiles (5.9.4.1). A
// 256x256 RGBA8 chain (128 tile): mips 0 and 1 are standard; mip 2 (64x64)
// is written and read back through the tail.
TEST_P(D3D12MetalTest, ReservedTexturePackedTailHoldsItsMips) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse textures";
    }
    constexpr u64 kTile = 65536ull;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    h->w64(kDesc2 + rd.Width, 256);
    h->w32(kDesc2 + rd.Height, 256);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 0).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    const GuestAddr tiled =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));

    constexpr GuestAddr kTiles = kData + 0x16100;
    constexpr GuestAddr kPacked = kData + 0x16110;
    constexpr GuestAddr kShape = kData + 0x16120;
    h->com(dev, DeviceSlot::GetResourceTiling,
           {{tiled}, {kTiles}, {kPacked}, {kShape}, {0}, {0}, {0}});
    const u32 total = h->r32(kTiles);
    const u32 standard = h->memory.read_u8(kPacked).value();
    const u32 packed = h->memory.read_u8(kPacked + 1).value();
    const u32 packed_tiles = h->r32(kPacked + 4);
    const u32 packed_start = h->r32(kPacked + 8);
    EXPECT_EQ(h->r32(kShape), 128u);
    EXPECT_EQ(h->r32(kShape + 4), 128u);
    EXPECT_GE(standard, 2u);  // 256 and 128 fill the tile
    EXPECT_EQ(standard + packed, 9u);
    ASSERT_LE(standard, 2u) << "the test writes mip 2 through the packed tail";
    ASSERT_GE(packed_tiles, 1u);
    EXPECT_EQ(packed_start, 5u);  // 2x2 + 1 standard tiles
    EXPECT_EQ(total, packed_start + packed_tiles);

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, total * kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));
    // Learn UpdateTileMappings: NULL coordinates and sizes are the whole
    // resource; one range of `total` heap tiles from 0.
    constexpr GuestAddr kFlag = kData + 0x16080;
    constexpr GuestAddr kHeapOff = kData + 0x160C0;
    constexpr GuestAddr kCount = kData + 0x160D0;
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    h->w32(kCount, total);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled}, {1}, {0}, {0}, {heap_mem}, {1}, {kFlag}, {kHeapOff},
                      {kCount}, {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);

    constexpr u32 kMip = 64;
    constexpr u32 kPitch = kMip * 4;
    const GuestAddr upload = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, kPitch * kMip);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, kTile);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr src = h->read_ptr(kOut2);
    for (u32 i = 0; i < kMip * kMip; ++i) h->w32(src + i * 4u, 0x5A000000u | i);
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    GuestAddr list = h->list(dev);
    {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        h->write_ptr(kDesc + l.pResource, tiled);
        h->w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h->w32(kDesc + l.SubresourceIndex, 2);
        h->write_ptr(kDesc2 + l.pResource, upload);
        h->w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Width, kMip);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Height, kMip);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, 1);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, kPitch);
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }
    h->transition(list, tiled, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, readback, tiled, DXGI_FORMAT_R8G8B8A8_UNORM, kMip, kMip, 2);
    h->run(dev, queue, list);
    {
        const std::vector<u8> out = h->read_back(readback, kPitch * kMip);
        u32 wrong = 0;
        for (u32 i = 0; i < kMip * kMip; ++i) {
            u32 v = 0;
            std::memcpy(&v, out.data() + i * 4u, 4);
            if (v != (0x5A000000u | i) && wrong++ < 4) {
                ADD_FAILURE() << "mip 2 texel " << i << " = 0x" << std::hex << v;
            }
        }
        EXPECT_EQ(wrong, 0u);
    }

    // NULL mip 0's tile (1, 0), then CopyTiles it to a buffer that holds a
    // pattern: the tile reads as zeros.
    constexpr GuestAddr kCoord = kData + 0x16000;
    constexpr GuestAddr kRegion = kData + 0x16040;
    h->w32(kCoord + 0, 1);
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 0);
    h->w32(kRegion + 8, 0);
    h->w32(kRegion + 12, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NULL);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled}, {1}, {kCoord}, {kRegion}, {0}, {1}, {kFlag}, {0}, {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->com(readback, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr host = h->read_ptr(kOut2);
    for (u32 i = 0; i < kTile / 4; ++i) h->w32(host + i * 4u, 0xABABABABu);
    (void)h->com(readback, ResourceSlot::Unmap, {{0}, {0}});
    list = h->list(dev);
    (void)h->com(list, GraphicsCommandListSlot::CopyTiles,
                 {{tiled},
                  {kCoord},
                  {kRegion},
                  {readback},
                  Harness::wide(0),
                  {D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER}});
    h->run(dev, queue, list);
    const std::vector<u8> zeros = h->read_back(readback, kTile);
    u32 nonzero = 0;
    for (u32 i = 0; i < kTile; ++i) nonzero += zeros[i] != 0 ? 1u : 0u;
    EXPECT_EQ(nonzero, 0u);
    EXPECT_EQ(h->com(dev, DeviceSlot::GetDeviceRemovedReason, {}), S_OK);
}

void append_dword(std::vector<u8>& out, u32 word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        out.push_back(static_cast<u8>((word >> (byte * 8)) & 0xFFu));
    }
}

/// cs_5_0 Buffer<float> t0 / RWByteAddressBuffer u0. One group of 64 threads
/// stores the bits of t0[SV_DispatchThreadID.x] at byte id*4. The tokens are
/// the SM5 forms the translator already decodes (dcl_resource dimension
/// BUFFER, ld, ishl, store_raw). R32_FLOAT's element is those bits.
std::vector<u8> compute_dxbc(std::vector<u32> words);

std::vector<u8> typed_r32_copy_shader() {
    std::vector<u32> words{
        0x00050050u, 0u,
        0x0100086Au,
        0x04000858u, 0x00107000u, 0u, 0x00005555u,
        0x0300009Du, 0x0011E000u, 0u,
        0x02000068u, 2u,
        0x0400009Bu, 64u, 1u, 1u,
        0x0600002Du, 0x00100012u, 0u, 0x00020E46u, 0x001070E4u, 0u,
        0x06000029u, 0x00100012u, 1u, 0x0002000Au, 0x00004001u, 2u,
        0x070000A6u, 0x0011E012u, 0u, 0x0010000Au, 1u, 0x00100006u, 0u,
        0x0100003Eu,
    };
    return compute_dxbc(words);
}

/// A DXBC container around shader-model 5 compute tokens @p words (word 1
/// is set to the token count), with empty ISGN / OSGN chunks.
std::vector<u8> compute_dxbc(std::vector<u32> words) {
    words[1] = static_cast<u32>(words.size());
    std::vector<u8> code;
    for (const u32 word : words) append_dword(code, word);
    // Empty ISGN / OSGN: count 0, name-table offset 8.
    const std::vector<u8> sig{0, 0, 0, 0, 8, 0, 0, 0};
    const std::array<std::pair<u32, std::vector<u8>>, 3> chunks{{
        {0x4E475349u, sig},
        {0x4E47534Fu, sig},
        {0x58454853u, code},
    }};
    u32 at = 32u + 4u * static_cast<u32>(chunks.size());
    std::vector<u32> offsets;
    for (const auto& chunk : chunks) {
        offsets.push_back(at);
        at += 8u + static_cast<u32>(chunk.second.size());
    }
    std::vector<u8> blob;
    append_dword(blob, 0x43425844u);
    for (int i = 0; i < 4; ++i) append_dword(blob, 0u);
    append_dword(blob, 1u);
    append_dword(blob, at);
    append_dword(blob, static_cast<u32>(chunks.size()));
    for (const u32 offset : offsets) append_dword(blob, offset);
    for (const auto& chunk : chunks) {
        append_dword(blob, chunk.first);
        append_dword(blob, static_cast<u32>(chunk.second.size()));
        blob.insert(blob.end(), chunk.second.begin(), chunk.second.end());
    }
    u8 hash[16]{};
    graphics::dxbc_container_checksum(blob.data(), at, hash);
    std::memcpy(blob.data() + 4, hash, sizeof(hash));
    return blob;
}

// Learn (D3D12_BUFFER_SRV): FirstElement is a UINT64 element index.
// StructureByteStride 0 and DXGI_FORMAT_R32_FLOAT make a typed view whose
// element is 4 bytes, so element 0x40000000 is byte 4 GiB. The resource is
// a reserved buffer (one mapped 64 KiB tile, not a 4 GiB allocation). The
// compute shader loads that view and stores the element bits. A 32-bit
// product of the element index and the stride is 0, which would read the
// unmapped base and return zeros instead of the dwords written into the tile.
TEST_P(D3D12MetalTest, TypedViewAtFourGiB) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse buffers";
    }
    constexpr u64 kFourGiB = 0x100000000ull;
    constexpr u64 kTile = 65536ull;
    constexpr u64 kFirstElement = 0x40000000ull;
    constexpr u32 kElements = 64u;
    constexpr u64 kByteOffset = kFirstElement * 4ull;
    constexpr u64 kWidth = kFourGiB + kTile;
    static_assert(kByteOffset == kFourGiB);
    static_assert(kByteOffset + kElements * 4ull <= kWidth);
    if (device.max_buffer_length() < kWidth) {
        GTEST_SKIP() << "Metal cannot address a buffer past 4 GiB";
    }

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter srvs;
    srvs.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv_range;
    srv_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.num_descriptors = 1;
    srv_range.base_shader_register = 0;
    srvs.ranges.push_back(srv_range);
    RootParameter uavs;
    uavs.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.base_shader_register = 0;
    uavs.ranges.push_back(uav_range);
    rs.parameters = {srvs, uavs};
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

    const std::vector<u8> shader = typed_r32_copy_shader();
    ASSERT_TRUE(h->memory.write_bytes(kShader, shader.data(), shader.size()).is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, shader.size());
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, kWidth);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    const GuestAddr data =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));

    constexpr GuestAddr kCoord = kData + 0x18000;
    constexpr GuestAddr kRegion = kData + 0x18040;
    constexpr GuestAddr kFlag = kData + 0x18080;
    constexpr GuestAddr kHeapOff = kData + 0x180C0;
    h->w32(kCoord + 0, static_cast<u32>(kFourGiB / kTile));
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 0);
    h->w32(kRegion + 8, 0);
    h->w32(kRegion + 12, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{data},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap_mem},
                      {1},
                      {kFlag},
                      {kHeapOff},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);

    std::array<u32, kElements> written{};
    for (u32 i = 0; i < kElements; ++i) {
        const float value = static_cast<float>(i + 1u);
        std::memcpy(&written[i], &value, 4);
    }
    const GuestAddr upload = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        kElements * 4u);
    const GuestAddr output = h->buffer(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        kElements * 4u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kElements * 4u);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < kElements; ++i) h->w32(mapped + i * 4u, written[i]);
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    const GuestAddr descriptors =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 srv_cpu = h->cpu_start(descriptors);
    const u64 uav_cpu = srv_cpu + increment;

    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    h->zero(kDesc, sl.size);
    h->w32(kDesc + sl.Format, DXGI_FORMAT_R32_FLOAT);
    h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_BUFFER);
    h->w32(kDesc + sl.Shader4ComponentMapping,
           D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    h->w64(kDesc + sl.Buffer_FirstElement, kFirstElement);
    h->w32(kDesc + sl.Buffer_NumElements, kElements);
    (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                 {{data}, {kDesc}, {srv_cpu}});

    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w64(kDesc + ul.Buffer_FirstElement, 0);
    h->w32(kDesc + ul.Buffer_NumElements, kElements);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{output}, {0}, {kDesc}, {uav_cpu}});

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(kByteOffset), {upload}, Harness::wide(0),
                  Harness::wide(kElements * 4u)});
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    h->transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, descriptors);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(descriptors))});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(descriptors) + increment)});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {output}, Harness::wide(0),
                  Harness::wide(kElements * 4u)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, kElements * 4u);
    for (u32 i = 0; i < kElements; ++i) {
        u32 v = 0;
        std::memcpy(&v, out.data() + i * 4u, 4);
        EXPECT_EQ(v, written[i]) << "element " << i;
    }
}

/// cs_5_0 Buffer<float4> t0 / RWBuffer<uint> u0 / RWByteAddressBuffer u1,
/// 64 threads: InterlockedAdd(u0[id], 5, original); u1 at id*32 gets
/// original, and at id*32 + 16 the float4 t0[id].
std::vector<u8> typed_raw_atomic_mapping_shader() {
    return compute_dxbc({
        0x00050050u, 0u,
        0x0100086Au,
        0x04000858u, 0x00107000u, 0u, 0x00005555u,  // dcl_resource_buffer t0 float4
        0x0400089Cu, 0x0011E000u, 0u, 0x00004444u,  // dcl_uav_typed_buffer u0 uint
        0x0300009Du, 0x0011E000u, 1u,               // dcl_uav_raw u1
        0x02000068u, 2u,
        0x0400009Bu, 64u, 1u, 1u,
        // imm_atomic_iadd r0.x, u0, vThreadID.xxxx, l(5)
        0x080000B4u, 0x00100012u, 0u, 0x0011E012u, 0u, 0x00020006u,
        0x00004001u, 5u,
        // ld r1.xyzw, vThreadID.xxxx, t0.xyzw
        0x0600002Du, 0x001000F2u, 1u, 0x00020006u, 0x00107E46u, 0u,
        // ishl r0.y, vThreadID.xxxx, l(5)
        0x06000029u, 0x00100022u, 0u, 0x00020006u, 0x00004001u, 5u,
        // store_raw u1.x, r0.y, r0.x
        0x070000A6u, 0x0011E012u, 1u, 0x00100556u, 0u, 0x00100006u, 0u,
        // iadd r0.z, r0.y, l(16)
        0x0700001Eu, 0x00100042u, 0u, 0x00100556u, 0u, 0x00004001u, 16u,
        // store_raw u1.xyzw, r0.z, r1.xyzw
        0x070000A6u, 0x0011E0F2u, 1u, 0x00100AA6u, 0u, 0x00100E46u, 1u,
        0x0100003Eu,
    });
}

// A typed view of a reserved buffer has no texture here (Metal makes none
// of a placement sparse buffer), so both views below are read as raw
// memory.
// Learn (InterlockedAdd on RWBuffer<uint>, R32_UINT): the element gains the
// value and the original returns. Learn (D3D12_SHADER_COMPONENT_MAPPING):
// each returned component is the view's R, G, B, A or a forced 0 / 1;
// D3D11.3 3.2.3.3 decodes an n-bit UNORM code c as c / (2^n - 1).
TEST_P(D3D12MetalTest, RawTypedViewAtomicAndComponentMapping) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse buffers";
    }
    constexpr u64 kTile = 65536ull;
    constexpr u32 kElements = 64u;
    constexpr u32 kColorByte = 512u;
    constexpr u32 kUploadBytes = kColorByte + kElements * 4u;
    constexpr u32 kOutBytes = kElements * 32u;
    // (A, 1, R, 0).
    constexpr u32 kMapping = 3u | (5u << 3) | (0u << 6) | (4u << 9) | (1u << 12);

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter srvs;
    srvs.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv_range;
    srv_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.num_descriptors = 1;
    srvs.ranges.push_back(srv_range);
    RootParameter uavs;
    uavs.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 2;
    uavs.ranges.push_back(uav_range);
    rs.parameters = {srvs, uavs};
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

    const std::vector<u8> shader = typed_raw_atomic_mapping_shader();
    ASSERT_TRUE(h->memory.write_bytes(kShader, shader.data(), shader.size()).is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, shader.size());
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, kTile);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    h->w32(kDesc2 + rd.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr data =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));
    constexpr GuestAddr kCoord = kData + 0x18000;
    constexpr GuestAddr kRegion = kData + 0x18040;
    constexpr GuestAddr kFlag = kData + 0x18080;
    constexpr GuestAddr kHeapOff = kData + 0x180C0;
    for (u32 i = 0; i < 4; ++i) h->w32(kCoord + i * 4u, 0);
    h->w32(kRegion + 0, 1);
    for (u32 i = 1; i < 4; ++i) h->w32(kRegion + i * 4u, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    (void)h->com(queue, CommandQueueSlot::UpdateTileMappings,
                 {{data}, {1}, {kCoord}, {kRegion}, {heap_mem}, {1}, {kFlag},
                  {kHeapOff}, {0}, {D3D12_TILE_MAPPING_FLAG_NONE}});

    const GuestAddr upload = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        kUploadBytes);
    const GuestAddr output = h->buffer(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        kOutBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kOutBytes + kElements * 4u);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < kElements; ++i) {
        h->w32(mapped + i * 4u, i * 10u);
        // R = i, G = 255 - i, B = 0x80, A = 0xFF (R in the low byte).
        h->w32(mapped + kColorByte + i * 4u,
               i | ((255u - i) << 8) | (0x80u << 16) | (0xFFu << 24));
    }
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    const GuestAddr descriptors =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 srv_cpu = h->cpu_start(descriptors);

    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    h->zero(kDesc, sl.size);
    h->w32(kDesc + sl.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_BUFFER);
    h->w32(kDesc + sl.Shader4ComponentMapping, kMapping);
    h->w64(kDesc + sl.Buffer_FirstElement, kColorByte / 4u);
    h->w32(kDesc + sl.Buffer_NumElements, kElements);
    (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                 {{data}, {kDesc}, {srv_cpu}});

    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_UINT);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w64(kDesc + ul.Buffer_FirstElement, 0);
    h->w32(kDesc + ul.Buffer_NumElements, kElements);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {srv_cpu + increment}});
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w64(kDesc + ul.Buffer_FirstElement, 0);
    h->w32(kDesc + ul.Buffer_NumElements, kOutBytes / 4u);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{output}, {0}, {kDesc}, {srv_cpu + 2u * increment}});

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(0), {upload}, Harness::wide(0),
                  Harness::wide(kUploadBytes)});
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h->transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, descriptors);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(descriptors))});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(descriptors) + increment)});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {output}, Harness::wide(0),
                  Harness::wide(kOutBytes)});
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(kOutBytes), {data}, Harness::wide(0),
                  Harness::wide(kElements * 4u)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, kOutBytes + kElements * 4u);
    const auto word = [&](u32 byte) {
        u32 v = 0;
        std::memcpy(&v, out.data() + byte, 4);
        return v;
    };
    const auto bits = [](float f) {
        u32 v = 0;
        std::memcpy(&v, &f, 4);
        return v;
    };
    for (u32 i = 0; i < kElements; ++i) {
        EXPECT_EQ(word(i * 32u), i * 10u) << "original " << i;
        EXPECT_EQ(word(kOutBytes + i * 4u), i * 10u + 5u) << "counter " << i;
        EXPECT_EQ(word(i * 32u + 16u), bits(1.0f)) << "A " << i;
        EXPECT_EQ(word(i * 32u + 20u), bits(1.0f)) << "forced 1 " << i;
        EXPECT_EQ(word(i * 32u + 24u),
                  bits(static_cast<float>(i) / 255.0f)) << "R " << i;
        EXPECT_EQ(word(i * 32u + 28u), 0u) << "forced 0 " << i;
    }
}

// Learn: D3D12_GPU_VIRTUAL_ADDRESS is a UINT64. A root CBV is that address
// (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT is 256). ps_rootconst writes
// cb0[0] to SV_Target, so the covered pixel is the float4 at the view.
//
// The triangle is the clip-space triangle that covers NDC [-1, 1] x [-1, 1]
// (the same 4x4 R8G8B8A8_UNORM target and 256-byte footprint the clear
// test reads). A 4x4 viewport's pixel centers sit at NDC
// x = (i + 0.5) / 2 - 1 and y = 1 - (j + 0.5) / 2, all strictly inside that
// triangle. A 1x1 target whose edge misses the pixel center is not a proof.
//
// The float4 at byte 0 of the same buffer is a different value. Binding the
// resource base, or dropping the byte offset, stores that sentinel (or the
// clear) instead of the view.
TEST_P(D3D12MetalTest, RootCbvAtFourGiBPlus256) {
    constexpr u64 kFourGiB = 0x100000000ull;
    constexpr u64 kTile = 65536ull;
    constexpr u64 kInto = 256ull;
    // {1, 0.5, 0.25, 1}. The clear test's UNORM rule (0.5 -> 128, 0.25 -> 64)
    // stores that as 0xFF4080FF. The float4 at byte 0 is {0, 1, 0, 1}
    // (0xFF00FF00). The clear is {0, 0, 0, 1} (0xFF000000).
    constexpr u32 kColor[4] = {0x3f800000u, 0x3f000000u, 0x3e800000u,
                               0x3f800000u};
    constexpr u32 kSentinel[4] = {0u, 0x3f800000u, 0u, 0x3f800000u};
    constexpr u32 kPixel = 0xFF4080FFu;
    constexpr u32 kRt = 4u;
    constexpr u32 kRow = 256u;
    static_assert(kInto == D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    static_assert((kFourGiB + kInto) %
                      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT ==
                  0u);

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    RootParameter cbv;
    cbv.parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbv.shader_visibility = D3D12_SHADER_VISIBILITY_PIXEL;
    cbv.shader_register = 0;
    rs.parameters = {cbv};
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

    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x1000;
    constexpr GuestAddr kPosName = kData + 0x17000;
    constexpr GuestAddr kColName = kData + 0x17020;
    constexpr GuestAddr kElements = kData + 0x17100;
    constexpr GuestAddr kView = kData + 0x17200;
    constexpr GuestAddr kScissor = kData + 0x17240;
    constexpr GuestAddr kVbView = kData + 0x17280;
    constexpr GuestAddr kRtvSlot = kData + 0x17300;
    constexpr GuestAddr kClear = kData + 0x17320;
    ASSERT_TRUE(h->memory.write_bytes(kVs, g_sh_vs_color, sizeof(g_sh_vs_color))
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPs, g_sh_ps_rootconst, sizeof(g_sh_ps_rootconst))
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPosName,
                                 reinterpret_cast<const u8*>("POSITION"), 9)
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kColName, reinterpret_cast<const u8*>("COLOR"), 6)
                    .is_ok());
    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    h->zero(kElements, static_cast<usize>(il.size) * 2u);
    h->write_ptr(kElements + il.SemanticName, kPosName);
    h->w32(kElements + il.Format, DXGI_FORMAT_R32G32_FLOAT);
    h->w32(kElements + il.AlignedByteOffset, 0);
    h->write_ptr(kElements + il.size + il.SemanticName, kColName);
    h->w32(kElements + il.size + il.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
    h->w32(kElements + il.size + il.AlignedByteOffset, 8);

    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[GetParam()];
    h->zero(kDesc, gl.size);
    h->write_ptr(kDesc + gl.pRootSignature, root);
    h->write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
    h->write_ptr(kDesc + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
    h->write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
    h->write_ptr(kDesc + gl.PS_BytecodeLength, sizeof(g_sh_ps_rootconst));
    const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
    h->w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
    h->w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
    h->w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
    h->w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
    h->w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
    h->w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
    h->w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
    h->w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h->w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h->write_ptr(kDesc + gl.InputLayout_pInputElementDescs, kElements);
    h->w32(kDesc + gl.InputLayout_NumElements, 2);
    h->w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    h->w32(kDesc + gl.NumRenderTargets, 1);
    h->w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc + gl.SampleDesc_Count, 1);
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr color = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr color_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kRow * kRt);
    const GuestAddr rtvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h->cpu_start(rtvs);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h->write_ptr(kRtvSlot, rtv);

    const float clear_rgba[4] = {0.f, 0.f, 0.f, 1.f};
    ASSERT_TRUE(h->memory
                    .write_bytes(kClear, reinterpret_cast<const u8*>(clear_rgba),
                                 sizeof(clear_rgba))
                    .is_ok());
    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[GetParam()];
    h->w32(kScissor + rect.left, 0);
    h->w32(kScissor + rect.top, 0);
    h->w32(kScissor + rect.right, kRt);
    h->w32(kScissor + rect.bottom, kRt);

    const GuestAddr vertices = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(vertices, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr vmap = h->read_ptr(kOut2);
    const float tri[3][6] = {
        {-1.f, -1.f, 1.f, 0.f, 1.f, 1.f},
        {-1.f, 3.f, 1.f, 0.f, 1.f, 1.f},
        {3.f, -1.f, 1.f, 0.f, 1.f, 1.f},
    };
    ASSERT_TRUE(h->memory
                    .write_bytes(vmap, reinterpret_cast<const u8*>(tri), sizeof(tri))
                    .is_ok());
    (void)h->com(vertices, ResourceSlot::Unmap, {{0}, {0}});
    const u64 vertex_va = h->gpu_va(vertices);
    ASSERT_NE(vertex_va, 0u);
    ASSERT_GT(vertex_va, 0xFFFFFFFFull);
    const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[GetParam()];
    h->w64(kVbView + vb.BufferLocation, vertex_va);
    h->w32(kVbView + vb.SizeInBytes, sizeof(tri));
    h->w32(kVbView + vb.StrideInBytes, 24);

    const GuestAddr constants = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 512);
    ASSERT_EQ(h->com(constants, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr cmap = h->read_ptr(kOut2);
    for (u32 c = 0; c < 4; ++c) {
        h->w32(cmap + c * 4u, kSentinel[c]);
        h->w32(cmap + kInto + c * 4u, kColor[c]);
    }
    (void)h->com(constants, ResourceSlot::Unmap, {{0}, {0}});
    const u64 constants_va = h->gpu_va(constants);
    ASSERT_NE(constants_va, 0u);
    ASSERT_GT(constants_va, 0xFFFFFFFFull);

    u32 color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const auto draw = [&](GuestAddr cb, u64 byte_offset, const char* where) {
        const u64 base = h->gpu_va(cb);
        ASSERT_NE(base, 0u) << where;
        ASSERT_GT(base, 0xFFFFFFFFull) << where;
        const u64 view = base + byte_offset;
        ASSERT_NE(byte_offset, 0u) << where;
        ASSERT_EQ(view % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, 0u)
            << where;
        const GuestAddr list = h->list(dev, pso);
        if (color_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            h->transition(list, color, color_state,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                     {{rtv}, {kClear}, {0}, {0}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetViewports,
                     {{1}, {kView}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetScissorRects,
                     {{1}, {kScissor}});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
        (void)h->com(list, GraphicsCommandListSlot::IASetVertexBuffers,
                     {{0}, {1}, {kVbView}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                     {{root}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView,
                     {{0}, Harness::wide(view)});
        (void)h->com(list, GraphicsCommandListSlot::OMSetRenderTargets,
                     {{1}, {kRtvSlot}, {0}, {0}});
        (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                     {{3}, {1}, {0}, {0}});
        h->transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        h->copy_to_footprint(list, color_out, color, DXGI_FORMAT_R8G8B8A8_UNORM,
                             kRt, kRt);
        h->run(dev, queue, list);
        color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const std::vector<u8> pixels = h->read_back(color_out, kRow * kRt);
        for (u32 y = 0; y < kRt; ++y) {
            for (u32 x = 0; x < kRt; ++x) {
                u32 got = 0;
                std::memcpy(&got, pixels.data() + y * kRow + x * 4u, 4);
                EXPECT_EQ(got, kPixel)
                    << where << " pixel " << x << "," << y
                    << " (clear is 0xFF000000; byte 0 of the buffer is green)";
            }
        }
    };

    draw(constants, kInto, "upload+256");

    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse buffers";
    }
    constexpr u64 kWidth = kFourGiB + kTile;
    if (device.max_buffer_length() < kWidth) {
        GTEST_SKIP() << "Metal cannot address a buffer past 4 GiB";
    }
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, kWidth);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    const GuestAddr reserved =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    const u64 reserved_va = h->gpu_va(reserved);
    ASSERT_NE(reserved_va, 0u) << "x86 GetGPUVirtualAddress of a reserved buffer";
    ASSERT_GT(reserved_va, 0xFFFFFFFFull);
    ASSERT_EQ(reserved_va % kTile, 0u);

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));
    constexpr GuestAddr kCoord = kData + 0x17400;
    constexpr GuestAddr kRegion = kData + 0x17440;
    constexpr GuestAddr kFlag = kData + 0x17480;
    constexpr GuestAddr kHeapOff = kData + 0x174C0;
    h->w32(kCoord + 0, static_cast<u32>(kFourGiB / kTile));
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 0);
    h->w32(kRegion + 8, 0);
    h->w32(kRegion + 12, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{reserved},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap_mem},
                      {1},
                      {kFlag},
                      {kHeapOff},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{reserved}, Harness::wide(kFourGiB), {constants},
                  Harness::wide(0), Harness::wide(512)});
    h->transition(list, reserved, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (color_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        h->transition(list, color, color_state,
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    const GuestAddr fence =
        h->made(h->com(dev, DeviceSlot::CreateFence,
                       {Harness::wide(0), {0}, {h->iid(IID_ID3D12FENCE)}, {kOut}}));
    ASSERT_EQ(h->com(queue, CommandQueueSlot::Signal, {{fence}, Harness::wide(1)}),
              S_OK);
    ASSERT_EQ(h->com(fence, FenceSlot::SetEventOnCompletion,
                     {Harness::wide(1), {0}}),
              S_OK);

    draw(reserved, kFourGiB + kInto, "4GiB+256");
}

namespace {

GraphicsState uint_targets(bool independent, u32 count) {
    GraphicsState state{};
    state.independent_blend = independent;
    state.num_render_targets = count;
    for (u32 index = 0; index < count; ++index) {
        state.rtv_formats[index] = DXGI_FORMAT_R8G8B8A8_UINT;
    }
    return state;
}

void enable_logic(RenderTargetBlend& blend, u32 op) {
    blend.logic_op_enable = true;
    blend.blend_enable = false;
    blend.logic_op = op;
}

}  // namespace

// D3D12_BLEND_DESC::IndependentBlendEnable false uses RenderTarget[0].
// True gives each UINT target its D3D12_RENDER_TARGET_BLEND_DESC::LogicOp.
// Both bitnesses: the decision is on the already-decoded blend state.
TEST_P(D3D12MetalTest, IndependentBlendGivesEachUintTargetItsLogicOp) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_framebuffer_fetch()) {
        GTEST_SKIP() << "no framebuffer fetch: OutputMergerLogicOp is FALSE";
    }
    Frontend& frontend = h->manager->frontend();
    const char* api = "ID3D12Device::CreateGraphicsPipelineState";

    GraphicsState different = uint_targets(true, 2);
    enable_logic(different.blend[0], D3D12_LOGIC_OP_XOR);
    enable_logic(different.blend[1], D3D12_LOGIC_OP_AND);
    Frontend::LogicOpVariant varied{};
    EXPECT_EQ(frontend.logic_op_variant(different, varied, api), S_OK);
    EXPECT_TRUE(varied.per_target);
    EXPECT_EQ(varied.targets, 0x3u);
    EXPECT_EQ(varied.ops[0], D3D12_LOGIC_OP_XOR);
    EXPECT_EQ(varied.ops[1], D3D12_LOGIC_OP_AND);
    EXPECT_EQ(varied.component_masks[0], (std::array<u32, 4>{0xFFu, 0xFFu, 0xFFu, 0xFFu}));
    EXPECT_EQ(varied.component_masks[1], (std::array<u32, 4>{0xFFu, 0xFFu, 0xFFu, 0xFFu}));

    GraphicsState r32_and_rgba8 = uint_targets(true, 2);
    r32_and_rgba8.rtv_formats[0] = DXGI_FORMAT_R32_UINT;
    enable_logic(r32_and_rgba8.blend[0], D3D12_LOGIC_OP_XOR);
    enable_logic(r32_and_rgba8.blend[1], D3D12_LOGIC_OP_AND);
    Frontend::LogicOpVariant widths{};
    EXPECT_EQ(frontend.logic_op_variant(r32_and_rgba8, widths, api), S_OK);
    EXPECT_TRUE(widths.per_target);
    EXPECT_EQ(widths.component_masks[0], (std::array<u32, 4>{~0u, 0u, 0u, 0u}));
    EXPECT_EQ(widths.component_masks[1], (std::array<u32, 4>{0xFFu, 0xFFu, 0xFFu, 0xFFu}));

    GraphicsState shared = uint_targets(false, 2);
    enable_logic(shared.blend[0], D3D12_LOGIC_OP_XOR);
    enable_logic(shared.blend[1], D3D12_LOGIC_OP_AND);  // ignored
    Frontend::LogicOpVariant one{};
    EXPECT_EQ(frontend.logic_op_variant(shared, one, api), S_OK);
    EXPECT_FALSE(one.per_target);
    EXPECT_EQ(one.op, D3D12_LOGIC_OP_XOR);
    EXPECT_EQ(one.targets, 0x3u);
    EXPECT_EQ(one.ops[0], D3D12_LOGIC_OP_XOR);
    EXPECT_EQ(one.ops[1], D3D12_LOGIC_OP_XOR);

    GraphicsState same = uint_targets(true, 2);
    enable_logic(same.blend[0], D3D12_LOGIC_OP_XOR);
    enable_logic(same.blend[1], D3D12_LOGIC_OP_XOR);
    Frontend::LogicOpVariant equal{};
    EXPECT_EQ(frontend.logic_op_variant(same, equal, api), S_OK);
    EXPECT_FALSE(equal.per_target);
    EXPECT_EQ(equal.op, D3D12_LOGIC_OP_XOR);

    // The shader the pipeline compiles names each target's op. A shared
    // XOR and a per-target XOR/AND are different programs.
    const auto compile = [&](const Frontend::LogicOpVariant& logic) {
        std::vector<u8> bytes(fl111::g_ps_logic2,
                              fl111::g_ps_logic2 + sizeof(fl111::g_ps_logic2));
        CompiledShader shader{};
        EXPECT_EQ(frontend.compile_shader(
                      std::move(bytes), graphics::ShaderStage::Fragment, shader,
                      api, nullptr, D3D12_SHADER_VISIBILITY_PIXEL, &logic),
                  S_OK);
        return shader.msl;
    };
    const std::string shared_msl = compile(one);
    const std::string split_msl = compile(varied);
    EXPECT_FALSE(shared_msl.empty());
    EXPECT_FALSE(split_msl.empty());
    EXPECT_NE(shared_msl, split_msl);
}

TEST_P(D3D12MetalTest, LogicOpFailuresStayInvalid) {
    Frontend& frontend = h->manager->frontend();
    const char* api = "ID3D12Device::CreateGraphicsPipelineState";

    GraphicsState both = uint_targets(true, 1);
    enable_logic(both.blend[0], D3D12_LOGIC_OP_XOR);
    both.blend[0].blend_enable = true;
    Frontend::LogicOpVariant out{};
    EXPECT_EQ(frontend.logic_op_variant(both, out, api), E_INVALIDARG);

    GraphicsState unorm = uint_targets(true, 1);
    unorm.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    enable_logic(unorm.blend[0], D3D12_LOGIC_OP_XOR);
    EXPECT_EQ(frontend.logic_op_variant(unorm, out, api), E_INVALIDARG);

    GraphicsState range = uint_targets(true, 1);
    enable_logic(range.blend[0], D3D12_LOGIC_OP_OR_INVERTED + 1u);
    EXPECT_EQ(frontend.logic_op_variant(range, out, api), E_INVALIDARG);

    auto forced = std::make_unique<CapsContext>();
    forced->logic_ops = false;
    frontend.m_caps = std::move(forced);
    GraphicsState fetched = uint_targets(true, 1);
    enable_logic(fetched.blend[0], D3D12_LOGIC_OP_XOR);
    EXPECT_EQ(frontend.logic_op_variant(fetched, out, api), E_INVALIDARG);
}

// Learn (CopyTextureRegion, D3D12_BOX): texel coordinates, left/top/front
// inclusive and right/bottom/back exclusive. A Texture3D subresource is the
// mip (Learn, Subresources: ArraySize is 1). The words below are the bytes
// this test uploads. A box whose front is not 0 must leave z = 0 alone.
// One committed volume and one placed volume.
TEST_P(D3D12MetalTest, Texture3DBoxCopyLeavesTexelsOutsideTheBox) {
    constexpr u32 kExtent = 4;
    constexpr u32 kPitch = 256;
    constexpr u32 kSlice = kPitch * kExtent;
    constexpr u32 kSentinel = 0xA5A5A5A5u;
    const auto pattern = [](u32 x, u32 y, u32 z) {
        return 0xFF000000u | (z << 16) | (y << 8) | (x + 1u);
    };
    const auto inside = [](u32 y, u32 z) {
        return y >= 1u && y < 3u && z >= 1u && z < 3u;
    };

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const auto make_volume = [&](u32 flags) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
        const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
        h->zero(kDesc, hp.size);
        h->zero(kDesc2, rd.size);
        h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
        h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
        h->w64(kDesc2 + rd.Width, kExtent);
        h->w32(kDesc2 + rd.Height, kExtent);
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, kExtent)
                        .is_ok());
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc2 + rd.SampleDesc_Count, 1);
        h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
        h->w32(kDesc2 + rd.Flags, flags);
        return h->made(h->com(dev, DeviceSlot::CreateCommittedResource,
                              {{kDesc},
                               {0},
                               {kDesc2},
                               {D3D12_RESOURCE_STATE_COPY_DEST},
                               {0},
                               {h->iid(IID_ID3D12RESOURCE)},
                               {kOut}}));
    };
    const GuestAddr committed = make_volume(0);
    const GuestAddr source = make_volume(0);
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, 1u << 20);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    h->w64(kDesc2 + rd.Width, kExtent);
    h->w32(kDesc2 + rd.Height, kExtent);
    EXPECT_TRUE(
        h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, kExtent).is_ok());
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
    const GuestAddr placed = h->made(h->com(
        dev, DeviceSlot::CreatePlacedResource,
        {{heap},
         Harness::wide(0),
         {kDesc2},
         {D3D12_RESOURCE_STATE_COPY_DEST},
         {0},
         {h->iid(IID_ID3D12RESOURCE)},
         {kOut}}));

    const GuestAddr sentinel_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 65536);
    const GuestAddr pattern_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 65536);
    const GuestAddr committed_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 65536);
    const GuestAddr placed_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 65536);
    const auto fill = [&](GuestAddr buffer, bool use_pattern) {
        ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
        const GuestAddr host = h->read_ptr(kOut2);
        for (u32 z = 0; z < kExtent; ++z) {
            for (u32 y = 0; y < kExtent; ++y) {
                for (u32 x = 0; x < kExtent; ++x) {
                    const u32 word =
                        use_pattern ? pattern(x, y, z) : kSentinel;
                    h->w32(host + z * kSlice + y * kPitch + x * 4u, word);
                }
            }
        }
    };
    fill(sentinel_buf, false);
    fill(pattern_buf, true);

    const auto footprint_to = [&](GuestAddr list, GuestAddr buffer,
                                  GuestAddr texture, u32 dst_x, u32 dst_y,
                                  u32 dst_z, const u32* box) {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        h->write_ptr(kDesc + l.pResource, texture);
        h->w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h->w32(kDesc + l.SubresourceIndex, 0);
        h->write_ptr(kDesc2 + l.pResource, buffer);
        h->w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h->w64(kDesc2 + l.PlacedFootprint_Offset, 0);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Format,
               DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Width, kExtent);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Height, kExtent);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, kExtent);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, kPitch);
        GuestAddr box_at = 0;
        if (box != nullptr) {
            box_at = kData + 0x1800;
            for (u32 i = 0; i < 6; ++i) h->w32(box_at + i * 4u, box[i]);
        }
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {dst_x}, {dst_y}, {dst_z}, {kDesc2}, {box_at}});
    };
    const auto volume_to_buffer = [&](GuestAddr list, GuestAddr buffer,
                                      GuestAddr texture) {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        h->write_ptr(kDesc + l.pResource, buffer);
        h->w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h->w64(kDesc + l.PlacedFootprint_Offset, 0);
        h->w32(kDesc + l.PlacedFootprint_Footprint_Format,
               DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc + l.PlacedFootprint_Footprint_Width, kExtent);
        h->w32(kDesc + l.PlacedFootprint_Footprint_Height, kExtent);
        h->w32(kDesc + l.PlacedFootprint_Footprint_Depth, kExtent);
        h->w32(kDesc + l.PlacedFootprint_Footprint_RowPitch, kPitch);
        h->write_ptr(kDesc2 + l.pResource, texture);
        h->w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h->w32(kDesc2 + l.SubresourceIndex, 0);
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    };
    const u32 box[6] = {0, 1, 1, kExtent, 3, 3};
    const GuestAddr list = h->list(dev);
    footprint_to(list, sentinel_buf, committed, 0, 0, 0, nullptr);
    footprint_to(list, pattern_buf, committed, 0, 1, 1, box);
    footprint_to(list, pattern_buf, source, 0, 0, 0, nullptr);
    footprint_to(list, sentinel_buf, placed, 0, 0, 0, nullptr);
    {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        h->write_ptr(kDesc + l.pResource, placed);
        h->w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h->write_ptr(kDesc2 + l.pResource, source);
        h->w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        const GuestAddr box_at = kData + 0x1900;
        for (u32 i = 0; i < 6; ++i) h->w32(box_at + i * 4u, box[i]);
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {0}, {1}, {1}, {kDesc2}, {box_at}});
    }
    volume_to_buffer(list, committed_out, committed);
    volume_to_buffer(list, placed_out, placed);
    h->run(dev, queue, list);

    const auto check = [&](const std::vector<u8>& bytes, const char* name) {
        ASSERT_GE(bytes.size(), static_cast<usize>(kSlice) * kExtent) << name;
        for (u32 z = 0; z < kExtent; ++z) {
            for (u32 y = 0; y < kExtent; ++y) {
                for (u32 x = 0; x < kExtent; ++x) {
                    u32 word = 0;
                    std::memcpy(&word,
                                bytes.data() + z * kSlice + y * kPitch + x * 4u,
                                4);
                    const u32 want =
                        inside(y, z) ? pattern(x, y, z) : kSentinel;
                    EXPECT_EQ(word, want)
                        << name << " z=" << z << " y=" << y << " x=" << x;
                }
            }
        }
    };
    check(h->read_back(committed_out, static_cast<usize>(kSlice) * kExtent),
          "committed footprint box");
    check(h->read_back(placed_out, static_cast<usize>(kSlice) * kExtent),
          "placed texture box");
}

// Learn (BeginRenderPass, D3D12_RENDER_PASS_TIER_0): tier 0 records an
// ordinary copy. SUSPENDING_PASS / RESUMING_PASS in one ExecuteCommandLists
// keep the bound mip and W range of a Texture3D render target. The restored
// words are the bytes uploaded before the suspend; the other W slices keep
// the bytes uploaded across the suspend.
TEST_P(D3D12MetalTest, Texture3DSuspendRestoresTheBoundWRange) {
    constexpr u32 kExtent = 4;
    constexpr u32 kPitch = 256;
    constexpr u32 kSlice = kPitch * kExtent;
    constexpr u32 kSentinel = 0xA5A5A5A5u;
    const auto pattern = [](u32 x, u32 y, u32 z) {
        return 0xFF000000u | (z << 16) | (y << 8) | (x + 1u);
    };

    const GuestAddr dev = h->device();
    for (u32 i = 0; i < 12; i += 4) h->w32(kDesc + i, 0xFFFFFFFFu);
    ASSERT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport,
                     {{D3D12_FEATURE_D3D12_OPTIONS5}, {kDesc}, {12}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + 4), 0u);

    const GuestAddr queue = h->queue(dev);
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc, hp.size);
    h->zero(kDesc2, rd.size);
    h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    h->w64(kDesc2 + rd.Width, kExtent);
    h->w32(kDesc2 + rd.Height, kExtent);
    EXPECT_TRUE(
        h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, kExtent).is_ok());
    EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
    h->w32(kDesc2 + rd.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr volume = h->made(h->com(
        dev, DeviceSlot::CreateCommittedResource,
        {{kDesc},
         {0},
         {kDesc2},
         {D3D12_RESOURCE_STATE_COPY_DEST},
         {0},
         {h->iid(IID_ID3D12RESOURCE)},
         {kOut}}));
    const GuestAddr pattern_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 65536);
    const GuestAddr sentinel_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 65536);
    const GuestAddr readback = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 65536);
    const auto fill = [&](GuestAddr buffer, bool use_pattern) {
        ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
        const GuestAddr host = h->read_ptr(kOut2);
        for (u32 z = 0; z < kExtent; ++z) {
            for (u32 y = 0; y < kExtent; ++y) {
                for (u32 x = 0; x < kExtent; ++x) {
                    h->w32(host + z * kSlice + y * kPitch + x * 4u,
                           use_pattern ? pattern(x, y, z) : kSentinel);
                }
            }
        }
    };
    fill(pattern_buf, true);
    fill(sentinel_buf, false);

    const GuestAddr rtvs =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h->cpu_start(rtvs);
    const auto& rv = layout::D3D12_RENDER_TARGET_VIEW_DESC[GetParam()];
    h->zero(kDesc, rv.size);
    h->w32(kDesc + rv.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc + rv.ViewDimension, D3D12_RTV_DIMENSION_TEXTURE3D);
    h->w32(kDesc + rv.Texture3D_MipSlice, 0);
    h->w32(kDesc + rv.Texture3D_FirstWSlice, 1);
    h->w32(kDesc + rv.Texture3D_WSize, 2);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView,
                 {{volume}, {kDesc}, {rtv}});

    const auto copy_full = [&](GuestAddr list, GuestAddr buffer, bool to_texture) {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        GuestAddr footprint = to_texture ? kDesc2 : kDesc;
        GuestAddr image = to_texture ? kDesc : kDesc2;
        h->write_ptr(footprint + l.pResource, buffer);
        h->w32(footprint + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h->w64(footprint + l.PlacedFootprint_Offset, 0);
        h->w32(footprint + l.PlacedFootprint_Footprint_Format,
               DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(footprint + l.PlacedFootprint_Footprint_Width, kExtent);
        h->w32(footprint + l.PlacedFootprint_Footprint_Height, kExtent);
        h->w32(footprint + l.PlacedFootprint_Footprint_Depth, kExtent);
        h->w32(footprint + l.PlacedFootprint_Footprint_RowPitch, kPitch);
        h->write_ptr(image + l.pResource, volume);
        h->w32(image + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    };
    const auto& pass = layout::D3D12_RENDER_PASS_RENDER_TARGET_DESC[GetParam()];
    const GuestAddr kRt = kData + 0x7000;
    h->zero(kRt, pass.size);
    h->write_ptr(kRt + pass.cpuDescriptor_ptr, rtv);
    h->w32(kRt + pass.BeginningAccess_Type,
           D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE);
    h->w32(kRt + pass.EndingAccess_Type,
           D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);

    const GuestAddr first = h->list(dev);
    copy_full(first, pattern_buf, true);
    h->transition(first, volume, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    (void)h->com(first, GraphicsCommandListSlot::BeginRenderPass,
                 {{1}, {kRt}, {0}, {D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS}});
    (void)h->com(first, GraphicsCommandListSlot::EndRenderPass, {});
    h->transition(first, volume, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    copy_full(first, sentinel_buf, true);
    ASSERT_EQ(h->com(first, GraphicsCommandListSlot::Close, {}), S_OK);

    const GuestAddr second = h->list(dev);
    (void)h->com(second, GraphicsCommandListSlot::BeginRenderPass,
                 {{1}, {kRt}, {0}, {D3D12_RENDER_PASS_FLAG_RESUMING_PASS}});
    (void)h->com(second, GraphicsCommandListSlot::EndRenderPass, {});
    h->transition(second, volume, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_full(second, readback, false);
    ASSERT_EQ(h->com(second, GraphicsCommandListSlot::Close, {}), S_OK);

    h->write_ptr(kArray, first);
    h->write_ptr(kArray + h->ptr_size(), second);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{2}, {kArray}});
    const GuestAddr fence =
        h->made(h->com(dev, DeviceSlot::CreateFence,
                       {Harness::wide(0), {0}, {h->iid(IID_ID3D12FENCE)}, {kOut}}));
    ASSERT_EQ(h->com(queue, CommandQueueSlot::Signal, {{fence}, Harness::wide(1)}),
              S_OK);
    ASSERT_EQ(h->com(fence, FenceSlot::SetEventOnCompletion,
                     {Harness::wide(1), {0}}),
              S_OK);

    const std::vector<u8> out =
        h->read_back(readback, static_cast<usize>(kSlice) * kExtent);
    for (u32 z = 0; z < kExtent; ++z) {
        for (u32 y = 0; y < kExtent; ++y) {
            for (u32 x = 0; x < kExtent; ++x) {
                u32 word = 0;
                std::memcpy(&word,
                            out.data() + z * kSlice + y * kPitch + x * 4u, 4);
                const bool kept = z == 1u || z == 2u;
                EXPECT_EQ(word, kept ? pattern(x, y, z) : kSentinel)
                    << "z=" << z << " y=" << y << " x=" << x;
            }
        }
    }
}

void push_u32(std::vector<u8>& out, u32 word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        out.push_back(static_cast<u8>((word >> (byte * 8)) & 0xFFu));
    }
}

struct SoSigEl {
    const char* name;
    u32 index;
    u32 system_value;
    u32 component_type;
    u32 reg;
    u32 mask;
};

std::vector<u8> so_signature_chunk(const std::vector<SoSigEl>& elements) {
    std::vector<u8> data;
    push_u32(data, static_cast<u32>(elements.size()));
    push_u32(data, 8u);
    std::vector<u8> strings;
    const u32 strings_at = 8u + 24u * static_cast<u32>(elements.size());
    for (const auto& element : elements) {
        push_u32(data, strings_at + static_cast<u32>(strings.size()));
        push_u32(data, element.index);
        push_u32(data, element.system_value);
        push_u32(data, element.component_type);
        push_u32(data, element.reg);
        push_u32(data, element.mask | (element.mask << 8));
        for (const char* c = element.name; *c != 0; ++c) strings.push_back(*c);
        strings.push_back(0);
    }
    while (strings.size() % 4 != 0) strings.push_back(0);
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

/// A DXBC container of shader tokens and its two signatures.
std::vector<u8> so_dxbc(const std::vector<u32>& tokens,
                        const std::vector<SoSigEl>& inputs,
                        const std::vector<SoSigEl>& outputs) {
    std::vector<u8> code;
    for (const u32 token : tokens) push_u32(code, token);
    const std::array<std::pair<u32, std::vector<u8>>, 3> chunks{{
        {0x4E475349u, so_signature_chunk(inputs)},
        {0x4E47534Fu, so_signature_chunk(outputs)},
        {0x58454853u, code},
    }};
    u32 at = 32u + 4u * static_cast<u32>(chunks.size());
    std::vector<u32> offsets;
    for (const auto& chunk : chunks) {
        offsets.push_back(at);
        at += 8u + static_cast<u32>(chunk.second.size());
    }
    std::vector<u8> blob;
    push_u32(blob, 0x43425844u);
    for (int i = 0; i < 4; ++i) push_u32(blob, 0u);
    push_u32(blob, 1u);
    push_u32(blob, at);
    push_u32(blob, static_cast<u32>(chunks.size()));
    for (const u32 offset : offsets) push_u32(blob, offset);
    for (const auto& chunk : chunks) {
        push_u32(blob, chunk.first);
        push_u32(blob, static_cast<u32>(chunk.second.size()));
        blob.insert(blob.end(), chunk.second.begin(), chunk.second.end());
    }
    u8 hash[16]{};
    graphics::dxbc_container_checksum(blob.data(), at, hash);
    std::memcpy(blob.data() + 4, hash, sizeof(hash));
    return blob;
}

/// vs_4_0: o0 = v0 (SV_Position), o1.x = v1.x (SV_CullDistance0).
std::vector<u8> cull_vertex_shader() {
    const std::vector<u32> tokens{
        0x00010040u, 27u,
        0x0300005Fu, 0x001010F2u, 0u,              // dcl_input v0.xyzw
        0x0300005Fu, 0x00101012u, 1u,              // dcl_input v1.x
        0x04000067u, 0x001020F2u, 0u, 1u,          // dcl_output_siv o0, position
        0x04000067u, 0x00102012u, 1u, 3u,          // dcl_output_siv o1.x, cull
        0x05000036u, 0x001020F2u, 0u, 0x00101E46u, 0u,  // mov o0, v0
        0x05000036u, 0x00102012u, 1u, 0x0010100Au, 1u,  // mov o1.x, v1.x
        0x0100003Eu,
    };
    return so_dxbc(tokens,
                   {{"POSITION", 0, 0, 3, 0, 0xF}, {"CULLIN", 0, 0, 3, 1, 0x1}},
                   {{"SV_Position", 0, 1, 3, 0, 0xF},
                    {"SV_CullDistance", 0, 3, 3, 1, 0x1}});
}

/// o0 = (0,0,0,1), o1 = SV_VertexID.xxxx. The same vs_4_0 the stream-output
/// contract test streams: each TEXCOORD0 component is that vertex id.
std::vector<u8> so_point_vertex_shader() {
    const std::vector<u32> tokens{
        0x00010040u, 31u,       0x04000060u, 0x00101012u, 0u, 6u,         0x04000067u,
        0x001020F2u, 0u,        1u,          0x03000065u, 0x001020F2u,    1u,
        0x03000065u, 0x00102032u, 2u,        0x08000036u, 0x001020F2u,    0u,
        0x00004002u, 0u,        0u,          0u,          0x3F800000u,    0x05000036u,
        0x001020F2u, 1u,        0x00101006u, 0u,          0x05000036u,    0x00102032u,
        2u,          0x00101006u, 0u,        0x0100003Eu,
    };
    return so_dxbc(tokens, {{"SV_VertexID", 0, 6, 1, 0, 0x1}},
                   {{"SV_Position", 0, 1, 3, 0, 0xF},
                    {"TEXCOORD", 0, 0, 3, 1, 0xF},
                    {"TEXCOORD", 1, 0, 3, 2, 0x3}});
}

// d3d12.h: D3D12_STREAM_OUTPUT_BUFFER_VIEW::SizeInBytes is UINT64. Learn
// ("Stream Output Counters"): BufferFilledSize is a 32-bit byte count, and a
// primitive that does not fit is not written. The buffer is reserved, 8 GiB
// plus one tile, with that single tile mapped at 4 GiB. The view starts on
// the tile and its capacity is 4 GiB + 32. Those low 32 bits are 32, which
// cannot hold one 48-byte triangle; the UINT64 capacity can. The filled-size
// word written for the triangle is 48.
TEST_P(D3D12MetalTest, StreamOutputRecordsABufferLargerThanFourGiB) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.supports_tiled_resources()) {
        GTEST_SKIP() << "no placement sparse buffers";
    }
    constexpr u64 kFourGiB = 0x100000000ull;
    constexpr u64 kTile = 65536ull;
    constexpr u64 kWidth = kFourGiB * 2ull + kTile;
    constexpr u64 kAt = kFourGiB;
    constexpr u64 kCapacity = kFourGiB + 32ull;
    constexpr u32 kStride = 16u;
    constexpr u32 kVertices = 3u;
    constexpr u32 kBytes = kVertices * kStride;
    static_assert(kCapacity > 0xFFFFFFFFull);
    static_assert(static_cast<u32>(kCapacity) < kBytes);
    static_assert(kAt + kCapacity <= kWidth);
    if (device.max_buffer_length() < kWidth) {
        GTEST_SKIP() << "Metal cannot address an 8 GiB buffer";
    }

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const std::vector<u8> vs = so_point_vertex_shader();
    ASSERT_TRUE(h->memory.write_bytes(kShader, vs.data(), vs.size()).is_ok());

    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
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

    constexpr GuestAddr kName = kData + 0x18000;
    constexpr GuestAddr kEntry = kData + 0x18100;
    constexpr GuestAddr kStrideAt = kData + 0x18200;
    constexpr GuestAddr kView = kData + 0x18300;
    ASSERT_TRUE(h->memory.write_bytes(kName, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    const auto& entry = layout::D3D12_SO_DECLARATION_ENTRY[GetParam()];
    h->zero(kEntry, entry.size);
    h->w32(kEntry + entry.Stream, 0);
    h->write_ptr(kEntry + entry.SemanticName, kName);
    h->w32(kEntry + entry.SemanticIndex, 0);
    ASSERT_TRUE(h->memory.write_u8(kEntry + entry.StartComponent, 0).is_ok());
    ASSERT_TRUE(h->memory.write_u8(kEntry + entry.ComponentCount, 4).is_ok());
    ASSERT_TRUE(h->memory.write_u8(kEntry + entry.OutputSlot, 0).is_ok());
    h->w32(kStrideAt, kStride);

    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, gl.size);
    h->write_ptr(kDesc + gl.pRootSignature, root);
    h->write_ptr(kDesc + gl.VS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + gl.VS_BytecodeLength, vs.size());
    h->write_ptr(kDesc + gl.StreamOutput_pSODeclaration, kEntry);
    h->w32(kDesc + gl.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + gl.StreamOutput_pBufferStrides, kStrideAt);
    h->w32(kDesc + gl.StreamOutput_NumStrides, 1);
    h->w32(kDesc + gl.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h->w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    h->w32(kDesc + gl.SampleDesc_Count, 1);
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    h->zero(kDesc2, rd.size);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, kWidth);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    const GuestAddr data =
        h->made(h->com(dev, DeviceSlot::CreateReservedResource,
                       {{kDesc2},
                        {D3D12_RESOURCE_STATE_COPY_DEST},
                        {0},
                        {h->iid(IID_ID3D12RESOURCE)},
                        {kOut}}));
    const u64 data_va = h->gpu_va(data);
    ASSERT_NE(data_va, 0u);
    ASSERT_EQ(data_va % kTile, 0u);

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    h->zero(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    const GuestAddr heap_mem =
        h->made(h->com(dev, DeviceSlot::CreateHeap,
                       {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}));
    constexpr GuestAddr kCoord = kData + 0x18400;
    constexpr GuestAddr kRegion = kData + 0x18440;
    constexpr GuestAddr kFlag = kData + 0x18480;
    constexpr GuestAddr kHeapOff = kData + 0x184C0;
    h->w32(kCoord + 0, static_cast<u32>(kAt / kTile));
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 0);
    h->w32(kRegion + 8, 0);
    h->w32(kRegion + 12, 0);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kHeapOff, 0);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{data},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap_mem},
                      {1},
                      {kFlag},
                      {kHeapOff},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);

    const GuestAddr upload = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 128);
    const GuestAddr filled = h->buffer(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr readback = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 128);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    h->w32(mapped, 0);
    for (u32 i = 0; i < kTile && i < 16u; ++i) {
        h->w32(mapped + 64u + i * 4u, 0xA5A5A5A5u);
    }
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});

    const auto& view = layout::D3D12_STREAM_OUTPUT_BUFFER_VIEW[GetParam()];
    h->zero(kView, view.size);
    h->w64(kView + view.BufferLocation, data_va + kAt);
    h->w64(kView + view.SizeInBytes, kCapacity);
    h->w64(kView + view.BufferFilledSizeLocation, h->gpu_va(filled));

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{filled}, Harness::wide(0), {upload}, Harness::wide(0),
                  Harness::wide(4)});
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(kAt), {upload}, Harness::wide(64),
                  Harness::wide(64)});
    h->transition(list, filled, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_STREAM_OUT);
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_STREAM_OUT);
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SOSetTargets,
                     {{0}, {1}, {kView}}),
              S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::DrawInstanced,
                     {{kVertices}, {1}, {0}, {0}}),
              S_OK);
    h->transition(list, filled, D3D12_RESOURCE_STATE_STREAM_OUT,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->transition(list, data, D3D12_RESOURCE_STATE_STREAM_OUT,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(kAt),
                  Harness::wide(64)});
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(64), {filled}, Harness::wide(0),
                  Harness::wide(4)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, 68);
    for (u32 vertex = 0; vertex < kVertices; ++vertex) {
        for (u32 component = 0; component < 4u; ++component) {
            u32 word = 0;
            std::memcpy(&word, out.data() + (vertex * 4u + component) * 4u, 4);
            EXPECT_EQ(word, vertex) << vertex << " " << component;
        }
    }
    for (u32 i = kBytes / 4u; i < 16u; ++i) {
        u32 word = 0;
        std::memcpy(&word, out.data() + i * 4u, 4);
        EXPECT_EQ(word, 0xA5A5A5A5u) << "byte " << i * 4u;
    }
    u32 filled_bytes = 0;
    std::memcpy(&filled_bytes, out.data() + 64, 4);
    EXPECT_EQ(filled_bytes, kBytes);
}

// Slots 0 through 8 are nine distinct input-assembler slots. Slot 8 is above
// the old 8-stream ceiling, and its buffer holds a colour that slot 0 does
// not. The covered pixels are that colour's UNORM8 encoding (DXGI: round half
// away from zero of the uploaded float times 255). A tenth distinct slot has
// no Metal buffer index and CreateGraphicsPipelineState fails.
TEST_P(D3D12MetalTest, DrawReadsAVertexFromInputSlotAboveSeven) {
    for (const u32 slots : {9u, 10u}) {
    SCOPED_TRACE(slots);
    const auto GetParamSlots = [slots] { return slots; };
    constexpr u32 kRt = 4;
    constexpr u32 kRow = 256;
    constexpr float kColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const auto unorm8 = [](float value) {
        const double scaled = static_cast<double>(value) * 255.0;
        return static_cast<u32>(scaled + 0.5);
    };
    const u32 pixel = unorm8(kColor[0]) | (unorm8(kColor[1]) << 8) |
                      (unorm8(kColor[2]) << 16) | (unorm8(kColor[3]) << 24);
    ASSERT_NE(pixel, 0xFF000000u);

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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

    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x1000;
    constexpr GuestAddr kPosName = kData + 0x1A000;
    constexpr GuestAddr kColName = kData + 0x1A020;
    constexpr GuestAddr kTexName = kData + 0x1A040;
    constexpr GuestAddr kElements = kData + 0x1A100;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kVb0 = kData + 0x1A580;
    constexpr GuestAddr kVb8 = kData + 0x1A5A0;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kClear = kData + 0x1A5E0;
    ASSERT_TRUE(h->memory
                    .write_bytes(kVs, g_sh_vs_color, sizeof(g_sh_vs_color))
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPs, g_sh_ps_color, sizeof(g_sh_ps_color))
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPosName, reinterpret_cast<const u8*>("POSITION"), 9)
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kColName, reinterpret_cast<const u8*>("COLOR"), 6)
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kTexName, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());

    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    const auto write_elements = [&](u32 count) {
        h->zero(kElements, static_cast<usize>(il.size) * count);
        for (u32 index = 0; index < count; ++index) {
            const GuestAddr at = kElements + static_cast<GuestAddr>(il.size) * index;
            const GuestAddr name = index == 0 ? kPosName
                                   : index == 8 ? kColName
                                                : kTexName;
            const u32 format = index == 8 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                          : DXGI_FORMAT_R32G32_FLOAT;
            h->write_ptr(at + il.SemanticName, name);
            h->w32(at + il.SemanticIndex, index == 8 || index == 0 ? 0u : index);
            h->w32(at + il.Format, format);
            h->w32(at + il.InputSlot, index);
            h->w32(at + il.AlignedByteOffset, 0);
            h->w32(at + il.InputSlotClass,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
            h->w32(at + il.InstanceDataStepRate, 0);
        }
    };

    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[GetParam()];
    const auto fill_pso = [&](u32 elements) {
        h->zero(kDesc, gl.size);
        h->write_ptr(kDesc + gl.pRootSignature, root);
        h->write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
        h->write_ptr(kDesc + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
        h->write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
        h->write_ptr(kDesc + gl.PS_BytecodeLength, sizeof(g_sh_ps_color));
        const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
        h->w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
        h->w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
        h->w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
        h->w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
        h->w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
        h->w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
        h->w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
        h->w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
        h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
        h->w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
        h->write_ptr(kDesc + gl.InputLayout_pInputElementDescs, kElements);
        h->w32(kDesc + gl.InputLayout_NumElements, elements);
        h->w32(kDesc + gl.PrimitiveTopologyType,
               D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        h->w32(kDesc + gl.NumRenderTargets, 1);
        h->w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc + gl.SampleDesc_Count, 1);
    };

    // Ten distinct slots: more than the nine Metal vertex buffer indices,
    // so the vertex shader pulls its inputs (dxbc_to_spirv_vertex_pull.cpp)
    // and the draw still reads slot 8.
    write_elements(GetParamSlots());
    fill_pso(GetParamSlots());
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr color = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr color_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kRow * kRt);
    const GuestAddr rtvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h->cpu_start(rtvs);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h->write_ptr(kRtvSlot, rtv);
    const float clear_rgba[4] = {0.f, 0.f, 0.f, 1.f};
    ASSERT_TRUE(h->memory
                    .write_bytes(kClear, reinterpret_cast<const u8*>(clear_rgba),
                                 sizeof(clear_rgba))
                    .is_ok());

    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[GetParam()];
    h->w32(kScissor + rect.left, 0);
    h->w32(kScissor + rect.top, 0);
    h->w32(kScissor + rect.right, kRt);
    h->w32(kScissor + rect.bottom, kRt);

    const float positions[3][2] = {{-1.f, -1.f}, {-1.f, 3.f}, {3.f, -1.f}};
    const GuestAddr position_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(position_buf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2),
                                 reinterpret_cast<const u8*>(positions),
                                 sizeof(positions))
                    .is_ok());
    (void)h->com(position_buf, ResourceSlot::Unmap, {{0}, {0}});
    float colors[3][4];
    for (auto& vertex : colors) {
        std::memcpy(vertex, kColor, sizeof(kColor));
    }
    const GuestAddr color_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(color_buf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2),
                                 reinterpret_cast<const u8*>(colors),
                                 sizeof(colors))
                    .is_ok());
    (void)h->com(color_buf, ResourceSlot::Unmap, {{0}, {0}});

    const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[GetParam()];
    h->w64(kVb0 + vb.BufferLocation, h->gpu_va(position_buf));
    h->w32(kVb0 + vb.SizeInBytes, sizeof(positions));
    h->w32(kVb0 + vb.StrideInBytes, 8);
    h->w64(kVb8 + vb.BufferLocation, h->gpu_va(color_buf));
    h->w32(kVb8 + vb.SizeInBytes, sizeof(colors));
    h->w32(kVb8 + vb.StrideInBytes, 16);

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                 {{rtv}, {kClear}, {0}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h->com(list, GraphicsCommandListSlot::RSSetScissorRects,
                 {{1}, {kScissor}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
    (void)h->com(list, GraphicsCommandListSlot::IASetVertexBuffers,
                 {{0}, {1}, {kVb0}});
    (void)h->com(list, GraphicsCommandListSlot::IASetVertexBuffers,
                 {{8}, {1}, {kVb8}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::OMSetRenderTargets,
                 {{1}, {kRtvSlot}, {0}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{3}, {1}, {0}, {0}});
    h->transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    h->copy_to_footprint(list, color_out, color, DXGI_FORMAT_R8G8B8A8_UNORM,
                         kRt, kRt);
    h->run(dev, queue, list);

    const std::vector<u8> pixels = h->read_back(color_out, kRow * kRt);
    for (u32 y = 0; y < kRt; ++y) {
        for (u32 x = 0; x < kRt; ++x) {
            u32 got = 0;
            std::memcpy(&got, pixels.data() + y * kRow + x * 4u, 4);
            EXPECT_EQ(got, pixel) << "pixel " << x << "," << y
                                  << " (slot 8 colour; clear is 0xFF000000)";
        }
    }
    }
}

// IA_VERTEX_BUFFER of B5G6R5, B5G5R5A1, B4G4R4A4 and B8G8R8X8
// (d3d12_a41_format_sweep, WARP and RTX: s1 bit 0x2). Metal has no vertex
// format of them, so the vertex shader fetches and unpacks the element
// (vertex pulling). Learn DXGI_FORMAT: components are named from the lowest
// bits, B first; X8 is unused (alpha reads 1, D3D11.3 19.1.3.3). A covered
// pixel of the R8G8B8A8_UNORM target is each channel's UNORM8 encoding.
TEST_P(D3D12MetalTest, DrawPullsPackedVertexFormats) {
    struct Case {
        u32 format;
        u32 stride;
        u8 bytes[4];
        u32 pixel;  // R | G << 8 | B << 16 | A << 24
    };
    const auto px = [](u32 r, u32 g, u32 b, u32 a) {
        return r | (g << 8) | (b << 16) | (a << 24);
    };
    // B5G6R5 0x8408: R 16, G 32, B 8 -> 132, 130, 66 (n / (2^k - 1) * 255).
    // B5G5R5A1 0x4104: R 16, G 8, B 4, A 0. B4G4R4A4 0xC359: R 3, G 5, B 9,
    // A 12 (x 17). B8G8R8X8: bytes B, G, R, X.
    const Case cases[] = {
        {DXGI_FORMAT_B5G6R5_UNORM, 2, {0x08, 0x84, 0, 0}, px(132, 130, 66, 255)},
        {DXGI_FORMAT_B5G5R5A1_UNORM, 2, {0x04, 0x41, 0, 0}, px(132, 66, 33, 0)},
        {DXGI_FORMAT_B4G4R4A4_UNORM, 2, {0x59, 0xC3, 0, 0}, px(51, 85, 153, 204)},
        {DXGI_FORMAT_B8G8R8X8_UNORM, 4, {0x10, 0x20, 0x30, 0x00}, px(0x30, 0x20, 0x10, 255)},
    };
    constexpr u32 kRt = 4;
    constexpr u32 kRow = 256;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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
    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x1000;
    constexpr GuestAddr kPosName = kData + 0x1A000;
    constexpr GuestAddr kColName = kData + 0x1A020;
    constexpr GuestAddr kElements = kData + 0x1A100;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kVbs = kData + 0x1A580;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kClear = kData + 0x1A5E0;
    ASSERT_TRUE(h->memory.write_bytes(kVs, g_sh_vs_color, sizeof(g_sh_vs_color)).is_ok());
    ASSERT_TRUE(h->memory.write_bytes(kPs, g_sh_ps_color, sizeof(g_sh_ps_color)).is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPosName, reinterpret_cast<const u8*>("POSITION"), 9)
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kColName, reinterpret_cast<const u8*>("COLOR"), 6)
                    .is_ok());
    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[GetParam()];
    const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[GetParam()];

    const GuestAddr color = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr color_out = h->buffer(
        dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kRow * kRt);
    const GuestAddr rtvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h->cpu_start(rtvs);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h->write_ptr(kRtvSlot, rtv);
    const float clear_rgba[4] = {0.f, 0.f, 0.f, 1.f};
    ASSERT_TRUE(h->memory
                    .write_bytes(kClear, reinterpret_cast<const u8*>(clear_rgba),
                                 sizeof(clear_rgba))
                    .is_ok());
    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[GetParam()];
    h->w32(kScissor + rect.left, 0);
    h->w32(kScissor + rect.top, 0);
    h->w32(kScissor + rect.right, kRt);
    h->w32(kScissor + rect.bottom, kRt);
    const float positions[3][2] = {{-1.f, -1.f}, {-1.f, 3.f}, {3.f, -1.f}};
    const GuestAddr position_buf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(position_buf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2), reinterpret_cast<const u8*>(positions),
                                 sizeof(positions))
                    .is_ok());
    (void)h->com(position_buf, ResourceSlot::Unmap, {{0}, {0}});

    u32 color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.format);
        h->zero(kElements, static_cast<usize>(il.size) * 2u);
        h->write_ptr(kElements + il.SemanticName, kPosName);
        h->w32(kElements + il.Format, DXGI_FORMAT_R32G32_FLOAT);
        h->write_ptr(kElements + il.size + il.SemanticName, kColName);
        h->w32(kElements + il.size + il.Format, c.format);
        h->w32(kElements + il.size + il.InputSlot, 1);
        h->zero(kDesc, gl.size);
        h->write_ptr(kDesc + gl.pRootSignature, root);
        h->write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
        h->write_ptr(kDesc + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
        h->write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
        h->write_ptr(kDesc + gl.PS_BytecodeLength, sizeof(g_sh_ps_color));
        const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
        h->w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
        h->w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
        h->w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
        h->w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
        h->w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
        h->w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
        h->w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
        h->w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
        h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
        h->w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
        h->write_ptr(kDesc + gl.InputLayout_pInputElementDescs, kElements);
        h->w32(kDesc + gl.InputLayout_NumElements, 2);
        h->w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        h->w32(kDesc + gl.NumRenderTargets, 1);
        h->w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
        h->w32(kDesc + gl.SampleDesc_Count, 1);
        const GuestAddr pso =
            h->made(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                           {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));
        u8 colors[12] = {};
        for (u32 v = 0; v < 3; ++v) std::memcpy(colors + v * c.stride, c.bytes, c.stride);
        const GuestAddr color_buf = h->buffer(
            dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
        ASSERT_EQ(h->com(color_buf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
        ASSERT_TRUE(h->memory.write_bytes(h->read_ptr(kOut2), colors, sizeof(colors)).is_ok());
        (void)h->com(color_buf, ResourceSlot::Unmap, {{0}, {0}});
        h->w64(kVbs + vb.BufferLocation, h->gpu_va(position_buf));
        h->w32(kVbs + vb.SizeInBytes, sizeof(positions));
        h->w32(kVbs + vb.StrideInBytes, 8);
        h->w64(kVbs + vb.size + vb.BufferLocation, h->gpu_va(color_buf));
        h->w32(kVbs + vb.size + vb.SizeInBytes, 3 * c.stride);
        h->w32(kVbs + vb.size + vb.StrideInBytes, c.stride);
        const GuestAddr list = h->list(dev, pso);
        if (color_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            h->transition(list, color, color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                     {{rtv}, {kClear}, {0}, {0}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
        (void)h->com(list, GraphicsCommandListSlot::IASetVertexBuffers, {{0}, {2}, {kVbs}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
        (void)h->com(list, GraphicsCommandListSlot::OMSetRenderTargets,
                     {{1}, {kRtvSlot}, {0}, {0}});
        (void)h->com(list, GraphicsCommandListSlot::DrawInstanced, {{3}, {1}, {0}, {0}});
        h->transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        h->copy_to_footprint(list, color_out, color, DXGI_FORMAT_R8G8B8A8_UNORM, kRt, kRt);
        h->run(dev, queue, list);
        color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const std::vector<u8> pixels = h->read_back(color_out, kRow * kRt);
        u32 got = 0;
        std::memcpy(&got, pixels.data() + kRow + 4u, 4);
        EXPECT_EQ(got, c.pixel) << std::hex << got;
    }
}

static const unsigned char g_sh_cs_float_xchg[752] = {
    0x44, 0x58, 0x42, 0x43, 0x12, 0xb4, 0x43, 0x50, 0xb7, 0xe9, 0x70, 0x0b,
    0x46, 0xf8, 0x3e, 0xef, 0x6b, 0x6e, 0x39, 0x09, 0x01, 0x00, 0x00, 0x00,
    0xf0, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x00, 0x18, 0x01, 0x00, 0x00, 0x28, 0x01, 0x00, 0x00,
    0x54, 0x02, 0x00, 0x00, 0x52, 0x44, 0x45, 0x46, 0xcc, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x3c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x53, 0x43, 0x00, 0x01, 0x00, 0x00,
    0xa2, 0x00, 0x00, 0x00, 0x52, 0x44, 0x31, 0x31, 0x3c, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x9e, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x74, 0x00, 0x62, 0x00, 0x6f, 0x00, 0x4d, 0x69, 0x63, 0x72, 0x6f, 0x73,
    0x6f, 0x66, 0x74, 0x20, 0x28, 0x52, 0x29, 0x20, 0x48, 0x4c, 0x53, 0x4c,
    0x20, 0x53, 0x68, 0x61, 0x64, 0x65, 0x72, 0x20, 0x43, 0x6f, 0x6d, 0x70,
    0x69, 0x6c, 0x65, 0x72, 0x20, 0x31, 0x30, 0x2e, 0x31, 0x00, 0xab, 0xab,
    0x49, 0x53, 0x47, 0x4e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x4f, 0x53, 0x47, 0x4e, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x53, 0x48, 0x45, 0x58,
    0x24, 0x01, 0x00, 0x00, 0x50, 0x00, 0x05, 0x00, 0x49, 0x00, 0x00, 0x00,
    0x6a, 0x08, 0x00, 0x01, 0x9c, 0x18, 0x00, 0x04, 0x00, 0xe0, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x00, 0x00, 0x9c, 0x08, 0x00, 0x04,
    0x00, 0xe0, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x55, 0x55, 0x00, 0x00,
    0x9d, 0x00, 0x00, 0x03, 0x00, 0xe0, 0x11, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x68, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x9b, 0x00, 0x00, 0x04,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xb8, 0x00, 0x00, 0x0c, 0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xe0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40,
    0xb8, 0x00, 0x00, 0x09, 0x22, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xe0, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x40, 0xc0,
    0xa3, 0x00, 0x00, 0x8c, 0xc2, 0x00, 0x00, 0x80, 0x43, 0x55, 0x15, 0x00,
    0x42, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x96, 0xec, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa3, 0x00, 0x00, 0x8c, 0x42, 0x00, 0x00, 0x80, 0x43, 0x55, 0x15, 0x00,
    0x82, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x96, 0xe3, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xa6, 0x00, 0x00, 0x07, 0xf2, 0xe0, 0x11, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x0e, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x01, 0x53, 0x54, 0x41, 0x54,
    0x94, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

// D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE of R32_FLOAT (WARP and RTX:
// format 41 s2 0x2c8). fxc cs_5_0 of
//   RWTexture2D<float> t : register(u0); RWBuffer<float> b : register(u1);
//   RWByteAddressBuffer o : register(u2);
//   InterlockedExchange(t[uint2(1, 0)], 2.5f, old_t);
//   InterlockedExchange(b[1], -3.0f, old_b);
//   o.Store4(0, asuint(float4(old_t, old_b, t[uint2(1, 0)], b[1])));
// is imm_atomic_exch on u0 / u1 declared (float) and ld_uav_typed of them
// (tools/oracle_probe-style blob, compiled on winoracle-gpu). Learn
// (InterlockedExchange): the original value comes back and the new one is
// stored, bit for bit.
TEST_P(D3D12MetalTest, InterlockedExchangeOnR32FloatTypedUavs) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 3;
    table.ranges = {uav_range};
    rs.parameters = {table};
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
    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_float_xchg, sizeof(g_sh_cs_float_xchg))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_float_xchg));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    // u0: a 4x1 R32_FLOAT texture holding 6, 7, 8, 9.
    const GuestAddr texture = h->cpu_texture(dev, DXGI_FORMAT_R32_FLOAT, 4, 1,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ASSERT_NE(texture, 0u);
    const float texels[4] = {6.f, 7.f, 8.f, 9.f};
    const GuestAddr src = kData + 0x18000;
    ASSERT_TRUE(h->memory
                    .write_bytes(src, reinterpret_cast<const u8*>(texels), sizeof(texels))
                    .is_ok());
    ASSERT_EQ(h->com(texture, ResourceSlot::WriteToSubresource,
                     {{0}, {0}, {src}, {16}, {16}}),
              S_OK);
    // u1: four R32_FLOAT elements 10, 11, 12, 13, uploaded by a copy.
    const GuestAddr typed = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COPY_DEST, 256,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr upload = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const float elements[4] = {10.f, 11.f, 12.f, 13.f};
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2), reinterpret_cast<const u8*>(elements),
                                 sizeof(elements))
                    .is_ok());
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});
    const GuestAddr out = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);

    const GuestAddr heap = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    const u64 cpu = h->cpu_start(heap);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_FLOAT);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_TEXTURE2D);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{texture}, {0}, {kDesc}, {cpu}});
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_FLOAT);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 4);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{typed}, {0}, {kDesc}, {cpu + increment}});
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{out}, {0}, {kDesc}, {cpu + 2u * increment}});

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{typed}, Harness::wide(0), {upload}, Harness::wide(0), Harness::wide(16)});
    h->transition(list, typed, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h->transition(list, texture, D3D12_RESOURCE_STATE_COMMON,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(heap))});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {out}, Harness::wide(0), Harness::wide(16)});
    h->run(dev, queue, list);
    const std::vector<u8> got = h->read_back(readback, 16);
    float words[4] = {};
    std::memcpy(words, got.data(), 16);
    EXPECT_EQ(words[0], 7.f) << "original texel";
    EXPECT_EQ(words[1], 11.f) << "original element";
    EXPECT_EQ(words[2], 2.5f) << "texel after the exchange";
    EXPECT_EQ(words[3], -3.f) << "element after the exchange";
}

// D3D11.3 15.4.2: a primitive is discarded when one cull distance is
// negative at all of its vertices; Metal has no cull distances, so a plain
// vertex shader writing SV_CullDistance draws through its vertex capture and
// a culling pass C. Left triangle: distances (-1, -1, +1), kept (not every
// vertex is out). Right triangle: (-1, -1, -1), culled. Drawn as a list and,
// the same two triangles, as a strip-free indexed list.
TEST_P(D3D12MetalTest, VertexShaderCullDistanceDiscardsWholePrimitives) {
    constexpr u32 kRt = 4;
    constexpr u32 kRow = 256;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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

    const std::vector<u8> vs = cull_vertex_shader();
    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x1000;
    constexpr GuestAddr kPosName = kData + 0x1A000;
    constexpr GuestAddr kCullName = kData + 0x1A020;
    constexpr GuestAddr kElements = kData + 0x1A100;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kVb = kData + 0x1A580;
    constexpr GuestAddr kIb = kData + 0x1A5A0;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kClear = kData + 0x1A5E0;
    ASSERT_TRUE(h->memory.write_bytes(kVs, vs.data(), vs.size()).is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPs, sysval::sv_ps_green, sizeof(sysval::sv_ps_green))
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kPosName, reinterpret_cast<const u8*>("POSITION"), 9)
                    .is_ok());
    ASSERT_TRUE(h->memory
                    .write_bytes(kCullName, reinterpret_cast<const u8*>("CULLIN"), 7)
                    .is_ok());
    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    h->zero(kElements, static_cast<usize>(il.size) * 2);
    for (u32 index = 0; index < 2; ++index) {
        const GuestAddr at = kElements + static_cast<GuestAddr>(il.size) * index;
        h->write_ptr(at + il.SemanticName, index == 0 ? kPosName : kCullName);
        h->w32(at + il.Format, index == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                          : DXGI_FORMAT_R32_FLOAT);
        h->w32(at + il.AlignedByteOffset, index == 0 ? 0u : 16u);
        h->w32(at + il.InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
    }
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[GetParam()];
    h->zero(kDesc, gl.size);
    h->write_ptr(kDesc + gl.pRootSignature, root);
    h->write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
    h->write_ptr(kDesc + gl.VS_BytecodeLength, vs.size());
    h->write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
    h->write_ptr(kDesc + gl.PS_BytecodeLength, sizeof(sysval::sv_ps_green));
    const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
    h->w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
    h->w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
    h->w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
    h->w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
    h->w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
    h->w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
    h->w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
    h->w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h->w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h->write_ptr(kDesc + gl.InputLayout_pInputElementDescs, kElements);
    h->w32(kDesc + gl.InputLayout_NumElements, 2);
    h->w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    h->w32(kDesc + gl.NumRenderTargets, 1);
    h->w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc + gl.SampleDesc_Count, 1);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK)
        << "a vertex shader writing SV_CullDistance";
    const GuestAddr pso = h->read_ptr(kOut);

    // {x, y, z, w, cull} per vertex.
    const float vertices[6][5] = {
        {-1.f, -3.f, 0.f, 1.f, -1.f}, {-1.f, 3.f, 0.f, 1.f, -1.f},
        {0.f, 0.f, 0.f, 1.f, 1.f},    {1.f, -3.f, 0.f, 1.f, -1.f},
        {1.f, 3.f, 0.f, 1.f, -1.f},   {0.f, 0.f, 0.f, 1.f, -1.f},
    };
    const GuestAddr vbuf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(vbuf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2),
                                 reinterpret_cast<const u8*>(vertices),
                                 sizeof(vertices))
                    .is_ok());
    const u16 indices[6] = {0, 1, 2, 3, 4, 5};
    const GuestAddr ibuf = h->buffer(
        dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    ASSERT_EQ(h->com(ibuf, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kOut2),
                                 reinterpret_cast<const u8*>(indices),
                                 sizeof(indices))
                    .is_ok());
    const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[GetParam()];
    h->w64(kVb + vb.BufferLocation, h->gpu_va(vbuf));
    h->w32(kVb + vb.SizeInBytes, sizeof(vertices));
    h->w32(kVb + vb.StrideInBytes, 20);
    const auto& ib = layout::D3D12_INDEX_BUFFER_VIEW[GetParam()];
    h->w64(kIb + ib.BufferLocation, h->gpu_va(ibuf));
    h->w32(kIb + ib.SizeInBytes, sizeof(indices));
    h->w32(kIb + ib.Format, DXGI_FORMAT_R16_UINT);

    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h->w32(at, bits);
    };
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[GetParam()];
    h->w32(kScissor + rect.right, kRt);
    h->w32(kScissor + rect.bottom, kRt);
    const float clear_rgba[4] = {0.f, 0.f, 0.f, 1.f};
    ASSERT_TRUE(h->memory
                    .write_bytes(kClear, reinterpret_cast<const u8*>(clear_rgba),
                                 sizeof(clear_rgba))
                    .is_ok());

    for (const bool indexed : {false, true}) {
        SCOPED_TRACE(indexed ? "indexed" : "non-indexed");
        const GuestAddr color = h->resource(
            dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const GuestAddr color_out = h->buffer(
            dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            kRow * kRt);
        const GuestAddr rtvs = h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        const u64 rtv = h->cpu_start(rtvs);
        (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
        h->write_ptr(kRtvSlot, rtv);
        const GuestAddr list = h->list(dev, pso);
        (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                     {{rtv}, {kClear}, {0}, {0}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetScissorRects,
                     {{1}, {kScissor}});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
        (void)h->com(list, GraphicsCommandListSlot::IASetVertexBuffers,
                     {{0}, {1}, {kVb}});
        (void)h->com(list, GraphicsCommandListSlot::IASetIndexBuffer, {{kIb}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                     {{root}});
        (void)h->com(list, GraphicsCommandListSlot::OMSetRenderTargets,
                     {{1}, {kRtvSlot}, {0}, {0}});
        if (indexed) {
            (void)h->com(list, GraphicsCommandListSlot::DrawIndexedInstanced,
                         {{6}, {1}, {0}, {0}, {0}});
        } else {
            (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                         {{6}, {1}, {0}, {0}});
        }
        h->transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        h->copy_to_footprint(list, color_out, color, DXGI_FORMAT_R8G8B8A8_UNORM,
                             kRt, kRt);
        h->run(dev, queue, list);
        const std::vector<u8> pixels = h->read_back(color_out, kRow * kRt);
        const auto at = [&](u32 x, u32 y) {
            u32 got = 0;
            std::memcpy(&got, pixels.data() + y * kRow + x * 4u, 4);
            return got;
        };
        // Row 1 (NDC y 0.25): x 0, 1 inside the kept triangle; 2, 3 inside
        // the culled one.
        EXPECT_EQ(at(0, 1), 0xFF00FF00u);
        EXPECT_EQ(at(1, 1), 0xFF00FF00u);
        EXPECT_EQ(at(2, 1), 0xFF000000u);
        EXPECT_EQ(at(3, 1), 0xFF000000u);
    }
}

// Learn ("Hardware Tiers", resource binding): D3D12 has UAVs in every
// stage at every feature level ("Max UAVs in all stages": 8 at tier 1, from
// FL 11_0). The D3D11.0 rule ("u# declared outside the pixel and compute
// stages") must not refuse a D3D12 vertex shader. fl111 vs_uav
// (tools/oracle_probe/d3d11_fl111.hlsl): InterlockedAdd(0, 1), then
// Store(4 + id * 4, id * 3 + 1), at u1 through a descriptor table.
TEST_P(D3D12MetalTest, VertexShaderWritesAUavThroughATable) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav.base_shader_register = 1;
    table.ranges.push_back(uav);
    rs.parameters = {table};
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

    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, fl111::g_vs_uav, sizeof(fl111::g_vs_uav))
                    .is_ok());
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, gl.size);
    h->write_ptr(kDesc + gl.pRootSignature, root);
    h->write_ptr(kDesc + gl.VS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + gl.VS_BytecodeLength, sizeof(fl111::g_vs_uav));
    h->w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h->w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h->w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h->w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h->w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    h->w32(kDesc + gl.SampleDesc_Count, 1);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const GuestAddr upload =
        h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 256);
    const GuestAddr data = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_COPY_DEST, 256,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    h->zero(h->read_ptr(kOut2), 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {h->cpu_start(heap)}});

    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, 16.f);
    put_f(kView + vp.Height, 1.f);
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[GetParam()];
    h->w32(kScissor + rect.left, 0);
    h->w32(kScissor + rect.top, 0);
    h->w32(kScissor + rect.right, 16);
    h->w32(kScissor + rect.bottom, 1);

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{data}, Harness::wide(0), {upload}, Harness::wide(0),
                  Harness::wide(256)});
    h->transition(list, data, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    h->write_ptr(kArray, heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(heap))});
    (void)h->com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h->com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced, {{4}, {1}, {0}, {0}});
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, 256);
    const auto word = [&](u32 i) {
        u32 v = 0;
        std::memcpy(&v, out.data() + i * 4u, 4);
        return v;
    };
    // The count only bounds below (an implementation may shade a vertex
    // more than once); each vertex's store is exact.
    EXPECT_GE(word(0), 4u);
    for (u32 id = 0; id < 4; ++id) EXPECT_EQ(word(1 + id), id * 3u + 1u) << id;
    EXPECT_EQ(word(5), 0u);
}

// a09 "big" (tools/oracle_probe/d3d12_a09_binding_probe.c; 64/64 on WARP and
// an RTX 4080 SUPER): resource binding tier 2 reads every SRV of an
// unbounded table of 3000 typed-buffer descriptors, the heap read in place.
// Then the descriptors are rewritten and a second list reads the new ones:
// a descriptor is read when the list executes, not when it was recorded.
TEST_P(D3D12MetalTest, UnboundedSrvTableReadsTheWholeHeap) {
    constexpr u32 kViews = 3000;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter uav_table;
    uav_table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav_table.ranges.push_back(uav);
    RootParameter srv_table;
    srv_table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv;
    srv.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.num_descriptors = 0xFFFFFFFFu;
    srv_table.ranges.push_back(srv);
    rs.parameters = {uav_table, srv_table};
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
    ASSERT_TRUE(
        h->memory.write_bytes(kShader, binding_cs_big, sizeof(binding_cs_big))
            .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(binding_cs_big));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr upload = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       kViews * 4u);
    const GuestAddr src = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST, kViews * 4u);
    const GuestAddr out = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < kViews; ++i) h->w32(mapped + i * 4u, 0x10000u + i * 7u);

    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews + 1, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    ASSERT_NE(increment, 0u);
    const u64 cpu = h->cpu_start(heap);
    const u64 gpu = h->gpu_start(heap);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{out}, {0}, {kDesc}, {cpu}});
    // Descriptor k views element first(k) of the buffer.
    const auto write_views = [&](auto first) {
        const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
        for (u32 k = 0; k < kViews; ++k) {
            h->zero(kDesc, sl.size);
            h->w32(kDesc + sl.Format, DXGI_FORMAT_R32_UINT);
            h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_BUFFER);
            h->w32(kDesc + sl.Shader4ComponentMapping,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
            h->w64(kDesc + sl.Buffer_FirstElement, first(k));
            h->w32(kDesc + sl.Buffer_NumElements, 1);
            (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                         {{src}, {kDesc}, {cpu + (1u + k) * u64{increment}}});
        }
    };
    const auto dispatch = [&](bool copy_in) {
        const GuestAddr list = h->list(dev, pso);
        if (copy_in) {
            (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                         {{src}, Harness::wide(0), {upload}, Harness::wide(0),
                          Harness::wide(kViews * 4u)});
            h->transition(list, src, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature,
                     {{root}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{0}, Harness::wide(gpu)});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{1}, Harness::wide(gpu + increment)});
        (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
        h->transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                     {{readback}, Harness::wide(0), {out}, Harness::wide(0),
                      Harness::wide(256)});
        h->transition(list, out, D3D12_RESOURCE_STATE_COPY_SOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        h->run(dev, queue, list);
        return h->read_back(readback, 256);
    };

    write_views([](u32 k) { return u64{k}; });
    std::vector<u8> got = dispatch(true);
    for (u32 t = 0; t < 64; ++t) {
        const u32 i = (t * 37u + 5u) % kViews;
        u32 v = 0;
        std::memcpy(&v, got.data() + t * 4u, 4);
        EXPECT_EQ(v, 0x10000u + i * 7u) << "thread " << t << " descriptor " << i;
    }

    write_views([](u32 k) { return u64{kViews - 1u - k}; });
    got = dispatch(false);
    for (u32 t = 0; t < 64; ++t) {
        const u32 i = (t * 37u + 5u) % kViews;
        u32 v = 0;
        std::memcpy(&v, got.data() + t * 4u, 4);
        EXPECT_EQ(v, 0x10000u + (kViews - 1u - i) * 7u)
            << "rewritten: thread " << t << " descriptor " << i;
    }
}

// SM 6.6 dynamic resources (ResourceDescriptorHeap[i]) as the DXIL lowering
// writes them: an ordinary 5.1 declaration in register space
// graphics::kDxbcResourceHeapSpace, element i = heap slot i. The shader is
// a09's cs_big (fxc) with T0's space DWORD patched to that space and the
// container checksum recomputed -- a hand-made extension-token shader. The
// root signature has only the u0 table and the
// D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag; without
// the flag the PSO is refused.
TEST_P(D3D12MetalTest, HeapIndexedSrvsReadTheBoundHeapFromSlotZero) {
    constexpr u32 kViews = 3000;
    // dcl_resource_buffer (uint) T0[0:*], space 0 (cs_big.asm): the 5.1 token
    // run ends with the space DWORD.
    static const u8 kDecl[] = {0x58, 0x08, 0x00, 0x07, 0x46, 0x7e, 0x30, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0xff, 0xff, 0xff, 0xff, 0x44, 0x44, 0x00, 0x00};
    std::vector<u8> shader(binding_cs_big, binding_cs_big + sizeof(binding_cs_big));
    usize at = 0;
    for (; at + sizeof(kDecl) + 4 <= shader.size(); ++at) {
        if (std::memcmp(shader.data() + at, kDecl, sizeof(kDecl)) == 0) break;
    }
    ASSERT_LT(at + sizeof(kDecl) + 4, shader.size()) << "T0 declaration";
    const u32 space = graphics::kDxbcResourceHeapSpace;
    std::memcpy(shader.data() + at + sizeof(kDecl), &space, 4);
    graphics::dxbc_container_checksum(shader.data(),
                                      static_cast<u32>(shader.size()),
                                      shader.data() + 4);
    ASSERT_TRUE(graphics::dxbc_container_verified(shader.data(), shader.size()));

    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const auto make_root = [&](u32 flags) {
        RootSignatureDesc rs;
        rs.flags = flags;
        RootParameter uav_table;
        uav_table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootDescriptorRange uav;
        uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav.num_descriptors = 1;
        uav_table.ranges.push_back(uav);
        rs.parameters = {uav_table};
        std::vector<u8> blob;
        std::string error;
        EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                           error),
                  S_OK)
            << error;
        EXPECT_TRUE(h->memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
        return h->made(h->com(dev, DeviceSlot::CreateRootSignature,
                              {{0}, {kBlob}, {blob.size()},
                               {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    };
    ASSERT_TRUE(h->memory.write_bytes(kShader, shader.data(), shader.size()).is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    const auto make_pso = [&](GuestAddr root) {
        h->zero(kDesc, cl.size);
        h->write_ptr(kDesc + cl.pRootSignature, root);
        h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
        h->write_ptr(kDesc + cl.CS_BytecodeLength, shader.size());
        return h->com(dev, DeviceSlot::CreateComputePipelineState,
                      {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    };
    EXPECT_EQ(make_pso(make_root(0)), static_cast<u64>(E_INVALIDARG))
        << "a heap-indexed shader needs CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED";
    const GuestAddr root =
        make_root(D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
    const GuestAddr pso = h->made(make_pso(root));

    const GuestAddr upload = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       kViews * 4u);
    const GuestAddr src = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST, kViews * 4u);
    const GuestAddr out = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 i = 0; i < kViews; ++i) h->w32(mapped + i * 4u, 0x10000u + i * 7u);

    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews + 1, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    const u64 cpu = h->cpu_start(heap);
    const u64 gpu = h->gpu_start(heap);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{out}, {0}, {kDesc}, {cpu}});
    // Heap slot 1 + k views element k.
    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[GetParam()];
    for (u32 k = 0; k < kViews; ++k) {
        h->zero(kDesc, sl.size);
        h->w32(kDesc + sl.Format, DXGI_FORMAT_R32_UINT);
        h->w32(kDesc + sl.ViewDimension, D3D12_SRV_DIMENSION_BUFFER);
        h->w32(kDesc + sl.Shader4ComponentMapping,
               D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        h->w64(kDesc + sl.Buffer_FirstElement, k);
        h->w32(kDesc + sl.Buffer_NumElements, 1);
        (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                     {{src}, {kDesc}, {cpu + (1u + k) * u64{increment}}});
    }
    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{src}, Harness::wide(0), {upload}, Harness::wide(0),
                  Harness::wide(kViews * 4u)});
    h->transition(list, src, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    h->write_ptr(kArray, heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(gpu)});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {out}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);
    const std::vector<u8> got = h->read_back(readback, 256);
    for (u32 t = 0; t < 64; ++t) {
        // cs_big reads element (t * 37 + 5) % 3000: heap slot i, which
        // views element i - 1 (slot 0 is the UAV; i is never 0 here).
        const u32 i = (t * 37u + 5u) % kViews;
        u32 v = 0;
        std::memcpy(&v, got.data() + t * 4u, 4);
        EXPECT_EQ(v, 0x10000u + (i - 1u) * 7u) << "thread " << t << " slot " << i;
    }
}

// a22 "cbv" (tools/oracle_probe/d3d12_a22_heapwin_probe.c; reads 64/64 on
// WARP and an RTX 4080 SUPER): resource binding tier 3 "full heap" CBVs. The
// shader's last unbounded range is a ConstantBuffer array and it has a raw
// UAV, whose view-size side buffer used to land inside a runtime CBV array
// (Metal refused the argument buffer: "id set location ... minimum is ...").
// 2000 CBVs, each 256 bytes of one upload buffer; thread t reads
// cbs[(t * 37 + 5) % 2000].v.y.
TEST_P(D3D12MetalTest, UnboundedCbvTableReadsTheWholeHeap) {
    constexpr u32 kViews = 2000;
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    RootSignatureDesc rs;
    RootParameter uav_table;
    uav_table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav_table.ranges.push_back(uav);
    RootParameter cbv_table;
    cbv_table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange cbv;
    cbv.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbv.num_descriptors = 0xFFFFFFFFu;
    cbv.register_space = 1;
    cbv_table.ranges.push_back(cbv);
    rs.parameters = {uav_table, cbv_table};
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
    ASSERT_TRUE(h->memory.write_bytes(kShader, heapwin_cs_cbv, sizeof(heapwin_cs_cbv))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(heapwin_cs_cbv));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr blocks = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       kViews * 256u);
    const GuestAddr out = h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    ASSERT_EQ(h->com(blocks, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    for (u32 k = 0; k < kViews; ++k) {
        h->w32(mapped + k * 256u, k);
        h->w32(mapped + k * 256u + 4u, 0x20000u + k * 3u);
    }
    const u64 va = h->gpu_va(blocks);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews + 1, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    const u64 cpu = h->cpu_start(heap);
    const u64 gpu = h->gpu_start(heap);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 64);
    h->w32(kDesc + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{out}, {0}, {kDesc}, {cpu}});
    for (u32 k = 0; k < kViews; ++k) {
        // D3D12_CONSTANT_BUFFER_VIEW_DESC: BufferLocation (UINT64),
        // SizeInBytes (UINT) -- the same layout in x86 and x64.
        h->w64(kDesc, va + k * 256u);
        h->w32(kDesc + 8, 256);
        h->w32(kDesc + 12, 0);
        (void)h->com(dev, DeviceSlot::CreateConstantBufferView,
                     {{kDesc}, {cpu + (1u + k) * u64{increment}}});
    }
    const GuestAddr list = h->list(dev, pso);
    h->write_ptr(kArray, heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(gpu)});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(gpu + increment)});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {out}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);
    const std::vector<u8> got = h->read_back(readback, 256);
    for (u32 t = 0; t < 64; ++t) {
        const u32 i = (t * 37u + 5u) % kViews;
        u32 v = 0;
        std::memcpy(&v, got.data() + t * 4u, 4);
        EXPECT_EQ(v, 0x20000u + i * 3u) << "thread " << t << " CBV " << i;
    }
}


// a03 [samplers] on the GPU: a shader model 5.0 compute shader (cs_samplers)
// samples through descriptor-table samplers whose BorderColor is none of
// Metal's three colours. Windows (WARP and the RTX 4080) returns the colour
// itself, (0.25, 0.5, 0.75, 1), for the border sample at (1.5, 0.25); the
// translation's border twin carries it.
TEST_P(D3D12MetalTest, ShaderModel50DescriptorSamplerKeepsACustomBorderColour) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter views;
    views.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv;
    srv.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.num_descriptors = 1;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav.offset_in_descriptors_from_table_start = 1;
    views.ranges = {srv, uav};
    RootParameter samplers;
    samplers.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange sampler_range;
    sampler_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.num_descriptors = 4;
    samplers.ranges = {sampler_range};
    rs.parameters = {views, samplers};
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
    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_samplers, sizeof(g_sh_cs_samplers))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_samplers));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const GuestAddr tex = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 2, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    const GuestAddr data =
        h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const GuestAddr sampler_heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    const u32 sampler_increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER}}));
    ASSERT_NE(increment, 0u);
    ASSERT_NE(sampler_increment, 0u);
    (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                 {{tex}, {0}, {h->cpu_start(heap)}});
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 16);
    h->w32(kDesc + ul.Buffer_StructureByteStride, 16);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {h->cpu_start(heap) + increment}});
    // Every sampler: POINT, BORDER, colour (0.25, 0.5, 0.75, 1).
    const auto& sd = layout::D3D12_SAMPLER_DESC[GetParam()];
    for (u32 i = 0; i < 4; ++i) {
        h->zero(kDesc, sd.size);
        h->w32(kDesc + sd.AddressU, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        h->w32(kDesc + sd.AddressV, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        h->w32(kDesc + sd.AddressW, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        h->w32(kDesc + sd.ComparisonFunc, D3D12_COMPARISON_FUNC_NEVER);
        const float border[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        for (u32 c = 0; c < 4; ++c) {
            u32 bits = 0;
            std::memcpy(&bits, &border[c], 4);
            h->w32(kDesc + sd.BorderColor + c * 4u, bits);
        }
        h->w32(kDesc + sd.MaxLOD, 0x7f7fffffu);  // D3D12_FLOAT32_MAX
        (void)h->com(dev, DeviceSlot::CreateSampler,
                     {{kDesc}, {h->cpu_start(sampler_heap) + i * sampler_increment}});
    }

    const GuestAddr list = h->list(dev, pso);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, heap);
    h->write_ptr(kArray + (GetParam() ? 8u : 4u), sampler_heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{2}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(heap))});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(sampler_heap))});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, 256);
    // o[6]: s3 at (1.5, 0.25), all border.
    u32 got[4] = {};
    std::memcpy(got, out.data() + 6 * 16, 16);
    EXPECT_EQ(got[0], 0x3e800000u);
    EXPECT_EQ(got[1], 0x3f000000u);
    EXPECT_EQ(got[2], 0x3f400000u);
    EXPECT_EQ(got[3], 0x3f800000u);
}

// MINIMUM_ / MAXIMUM_ filters through descriptor-table samplers created
// after the pipeline (the translation cannot know them; the shader's
// out-of-line reduction runs on the sampler-state record's flag). The
// expected texels follow D3D11.3 5.9.4.5.6 ("the min() or max() of the
// texels fetched, on a per-component basis", a zero-weight texel not
// counting) with 7.18.7/7.18.8 addressing on cs_samplers' 2x2 texture:
//   T00 (1,6,3,.5)  T10 (5,2,7,.25)  T01 (3,4,1,.75)  T11 (2,8,5,1)
// s0 MAXIMUM_MIN_MAG_MIP_LINEAR, s1 MINIMUM_MIN_MAG_MIP_LINEAR, s2
// MIN_MAG_MIP_LINEAR (no reduction), s3 MAXIMUM_MIN_MAG_MIP_POINT; CLAMP.
TEST_P(D3D12MetalTest, DescriptorTableMinMaxSamplersCreatedAfterThePipelineReduce) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    RootSignatureDesc rs;
    RootParameter views;
    views.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srv;
    srv.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.num_descriptors = 1;
    RootDescriptorRange uav;
    uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.num_descriptors = 1;
    uav.offset_in_descriptors_from_table_start = 1;
    views.ranges = {srv, uav};
    RootParameter samplers;
    samplers.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange sampler_range;
    sampler_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.num_descriptors = 4;
    samplers.ranges = {sampler_range};
    rs.parameters = {views, samplers};
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
    ASSERT_TRUE(h->memory
                    .write_bytes(kShader, g_sh_cs_samplers, sizeof(g_sh_cs_samplers))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    h->zero(kDesc, cl.size);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(g_sh_cs_samplers));
    const GuestAddr pso =
        h->made(h->com(dev, DeviceSlot::CreateComputePipelineState,
                       {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));

    const float texels[4][4] = {
        {1.0f, 6.0f, 3.0f, 0.5f}, {5.0f, 2.0f, 7.0f, 0.25f},   // row 0
        {3.0f, 4.0f, 1.0f, 0.75f}, {2.0f, 8.0f, 5.0f, 1.0f}};  // row 1
    const GuestAddr tex = h->resource(
        dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 2, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    const GuestAddr upload = h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, 512);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr src = h->read_ptr(kOut2);
    ASSERT_TRUE(h->memory.write_bytes(src, reinterpret_cast<const u8*>(texels[0]), 32).is_ok());
    ASSERT_TRUE(h->memory.write_bytes(src + 256, reinterpret_cast<const u8*>(texels[2]), 32).is_ok());
    (void)h->com(upload, ResourceSlot::Unmap, {{0}, {0}});
    const GuestAddr data =
        h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr readback = h->buffer(dev, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, 256);
    const GuestAddr heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    const GuestAddr sampler_heap =
        h->heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, true);
    const u32 increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}));
    const u32 sampler_increment = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER}}));
    (void)h->com(dev, DeviceSlot::CreateShaderResourceView,
                 {{tex}, {0}, {h->cpu_start(heap)}});
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    h->zero(kDesc, ul.size);
    h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
    h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h->w32(kDesc + ul.Buffer_NumElements, 16);
    h->w32(kDesc + ul.Buffer_StructureByteStride, 16);
    (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                 {{data}, {0}, {kDesc}, {h->cpu_start(heap) + increment}});
    // D3D12_FILTER: MAXIMUM_ 0x180, MINIMUM_ 0x100, + MIN_MAG_MIP_LINEAR 0x15.
    const u32 filters[4] = {0x195u, 0x115u, 0x15u, 0x180u};
    const auto& sd = layout::D3D12_SAMPLER_DESC[GetParam()];
    for (u32 i = 0; i < 4; ++i) {
        h->zero(kDesc, sd.size);
        h->w32(kDesc + sd.Filter, filters[i]);
        h->w32(kDesc + sd.AddressU, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        h->w32(kDesc + sd.AddressV, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        h->w32(kDesc + sd.AddressW, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        h->w32(kDesc + sd.ComparisonFunc, D3D12_COMPARISON_FUNC_NEVER);
        h->w32(kDesc + sd.MaxLOD, 0x7f7fffffu);  // D3D12_FLOAT32_MAX
        (void)h->com(dev, DeviceSlot::CreateSampler,
                     {{kDesc}, {h->cpu_start(sampler_heap) + i * sampler_increment}});
    }

    const GuestAddr list = h->list(dev, pso);
    {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[GetParam()];
        h->zero(kDesc, l.size);
        h->zero(kDesc2, l.size);
        h->write_ptr(kDesc + l.pResource, tex);
        h->w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h->w32(kDesc + l.SubresourceIndex, 0);
        h->write_ptr(kDesc2 + l.pResource, upload);
        h->w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Format,
               DXGI_FORMAT_R32G32B32A32_FLOAT);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Width, 2);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Height, 2);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, 1);
        h->w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, 256);
        (void)h->com(list, GraphicsCommandListSlot::CopyTextureRegion,
                     {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }
    h->transition(list, tex, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    h->write_ptr(kArray, heap);
    h->write_ptr(kArray + (GetParam() ? 8u : 4u), sampler_heap);
    (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{2}, {kArray}});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{0}, Harness::wide(h->gpu_start(heap))});
    (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                 {{1}, Harness::wide(h->gpu_start(sampler_heap))});
    (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h->transition(list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {data}, Harness::wide(0),
                  Harness::wide(256)});
    h->run(dev, queue, list);

    const std::vector<u8> out = h->read_back(readback, 256);
    const float expected[8][4] = {
        {1, 6, 3, 0.5f},     // o0 s0 MAX at T00's centre: T00 alone
        {2, 8, 5, 1},        // o1 s0 MAX at (1.25, .75), clamped: T11
        {1, 2, 3, 0.25f},    // o2 s1 MIN between T00 and T10
        {1, 2, 1, 0.25f},    // o3 s1 MIN at the centre: all four
        {5, 2, 7, 0.25f},    // o4 s2 LINEAR at (1.25, .25), clamped: T10
        {3, 4, 1, 0.75f},    // o5 s2 LINEAR at (-.25, .75), clamped: T01
        {5, 2, 7, 0.25f},    // o6 s3 MAX POINT at (1.5, .25): T10
        {2, 8, 5, 1}};       // o7 s3 MAX POINT at (.75, .75): T11
    for (u32 o = 0; o < 8; ++o) {
        float got[4] = {};
        std::memcpy(got, out.data() + o * 16, 16);
        for (u32 c = 0; c < 4; ++c) {
            EXPECT_EQ(got[c], expected[o][c]) << "o[" << o << "] component " << c;
        }
    }
}


namespace {
std::shared_ptr<graphics::BackendBuffer> record_test_buffer(const std::vector<u32>& words) {
    graphics::BackendBufferDesc desc{};
    desc.byte_width = words.size() * sizeof(u32);
    desc.shader_resource = true;
    desc.constant_buffer = true;
    return graphics::MetalDevice::instance().create_buffer(desc, words.data());
}
void expect_record_bytes(const std::shared_ptr<graphics::BackendBuffer>& buffer,
                          const std::vector<u32>& expected) {
    ASSERT_TRUE(buffer && buffer->valid());
    std::vector<u32> actual(expected.size());
    ASSERT_TRUE(buffer->read(0, actual.data(), actual.size() * sizeof(u32)));
    EXPECT_EQ(actual, expected);
}
}

TEST(SamplerRecordBufferCacheMetal, ExactWordsReuseImmutableStorage) {
    if (!graphics::MetalDevice::instance().available()) GTEST_SKIP();
    SamplerRecordBufferCache cache;
    std::vector<u32> words(graphics::kSamplerStateRecordWords, 0u);
    const auto original = words;
    auto first = record_test_buffer(words);
    ASSERT_TRUE(cache.remember(words, first));
    EXPECT_EQ(cache.find(words).get(), first.get());
    words[0] = std::bit_cast<u32>(1.0f);
    EXPECT_FALSE(cache.find(words));
    auto second = record_test_buffer(words);
    ASSERT_TRUE(cache.remember(words, second));
    EXPECT_EQ(cache.find(words).get(), second.get());
    EXPECT_EQ(cache.find(original).get(), first.get());
    expect_record_bytes(first, original);
    expect_record_bytes(second, words);
    words.push_back(0u);
    EXPECT_FALSE(cache.find(words));
}

TEST(SamplerRecordBufferCacheMetal, EvictionPreservesCallerReferences) {
    if (!graphics::MetalDevice::instance().available()) GTEST_SKIP();
    std::vector<u32> a(graphics::kSamplerStateRecordWords, 0u), b = a, c = a;
    b[0] = std::bit_cast<u32>(1.0f);
    c[0] = std::bit_cast<u32>(2.0f);
    const usize budget = a.size() * sizeof(u32) * 4u;
    SamplerRecordBufferCache cache(2, budget);
    auto first = record_test_buffer(a);
    auto second = record_test_buffer(b);
    auto third = record_test_buffer(c);
    ASSERT_TRUE(cache.remember(a, first));
    ASSERT_TRUE(cache.remember(b, second));
    ASSERT_EQ(cache.find(a).get(), first.get());
    ASSERT_TRUE(cache.remember(c, third));
    EXPECT_FALSE(cache.find(b));
    EXPECT_EQ(cache.find(a).get(), first.get());
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_LE(cache.retained_bytes(), budget);
    expect_record_bytes(second, b);
}

TEST(SamplerRecordBufferCacheMetal, OversizedAndInvalidEntriesAreNotRetained) {
    if (!graphics::MetalDevice::instance().available()) GTEST_SKIP();
    std::vector<u32> words(graphics::kSamplerStateRecordWords, 0u);
    auto buffer = record_test_buffer(words);
    SamplerRecordBufferCache too_small(2, words.size() * sizeof(u32));
    EXPECT_FALSE(too_small.remember(words, buffer));
    EXPECT_FALSE(too_small.find(words));
    expect_record_bytes(buffer, words);
    SamplerRecordBufferCache cache;
    EXPECT_FALSE(cache.remember(words, {}));
    EXPECT_FALSE(cache.remember({}, buffer));
    auto shorter = words;
    shorter.pop_back();
    EXPECT_FALSE(cache.remember(shorter, buffer));
    EXPECT_EQ(cache.size(), 0u);
}

TEST(SamplerRecordBufferCacheMetal, CacheOwnsAndReleasesItsReferences) {
    if (!graphics::MetalDevice::instance().available()) GTEST_SKIP();
    std::weak_ptr<const graphics::BackendBuffer> weak;
    {
        SamplerRecordBufferCache cache;
        std::vector<u32> words(graphics::kSamplerStateRecordWords, 0u);
        auto buffer = record_test_buffer(words);
        weak = buffer;
        ASSERT_TRUE(cache.remember(words, buffer));
        buffer.reset();
        EXPECT_FALSE(weak.expired());
    }
    EXPECT_TRUE(weak.expired());
}

}  // namespace
}  // namespace vitreo::winapi
