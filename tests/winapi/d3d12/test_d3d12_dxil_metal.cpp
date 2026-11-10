// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_dxil_metal.cpp
/// @brief Shader model 6 (DXIL) pipelines through d3d12.dll on Metal: the
/// dxc corpus (tests/oracle_data/d3d12_sm6) compiled by CreatePipelineState,
/// dispatched through the guest vtables, read back, and compared with WARP
/// line for line (tests/oracle_data/d3d12_sm6_exec_probe.expected, from
/// tools/oracle_probe/d3d12_sm6_exec_probe.c). The harness is
/// test_d3d12_metal.cpp's.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"

#include "../../oracle_data/d3d12_sm6/d3d12_sm6_shaders.h"
#include "../../oracle_data/d3d12_sm6x/d3d12_sm6x_shaders.h"
#include "../../oracle_data/d3d12_dxilc/d3d12_dxilc_shaders.h"
#include "../../oracle_data/d3d12_denorm/d3d12_denorm_shaders.h"
#include "../../oracle_data/d3d12_dynres/d3d12_dynres_shaders.h"
#include "../../oracle_data/d3d12_bary/d3d12_bary_shaders.h"
#include "../../oracle_data/d3d12_c4/d3d12_c4_shaders.h"
#include "../../oracle_data/d3d12_c6/d3d12_c6_shaders.h"


#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <array>
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <deque>
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


class D3D12DxilMetalTest : public ::testing::TestWithParam<bool> {
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

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12DxilMetalTest, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

const Sm6Blob& sm6_blob(const char* name) {
    for (const auto& b : kSm6Blobs) {
        if (std::string(b.name) == name) return b;
    }
    // The wave / 64-bit / 16-bit corpus (tests/oracle_data/d3d12_sm6x).
    static std::deque<Sm6Blob> extra;  // stable references across push_back
    for (const auto& b : kSm6xBlobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The DXIL completion corpus (tests/oracle_data/d3d12_dxilc).
    for (const auto& b : kDxilcBlobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The SM 6.6 dynamic-resources corpus (tests/oracle_data/d3d12_dynres).
    for (const auto& b : kDynresBlobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The C4 slice corpus (tests/oracle_data/d3d12_c4, dxc 1.9).
    for (const auto& b : kC4Blobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The C6 slice corpus (tests/oracle_data/d3d12_c6, dxc 1.8).
    for (const auto& b : kC6Blobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The SM 6.1 barycentrics corpus (tests/oracle_data/d3d12_bary).
    for (const auto& b : kBaryBlobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    // The fp32 denormal corpus (tests/oracle_data/d3d12_denorm).
    for (const auto& b : kDenormBlobs) {
        if (std::string(b.name) != name) continue;
        for (const auto& e : extra) {
            if (e.code == b.code) return e;
        }
        extra.push_back(Sm6Blob{b.name, b.code, b.size});
        return extra.back();
    }
    static const Sm6Blob none{"", nullptr, 0};
    return none;
}

/// The lines of d3d12_sm6_exec_probe.expected (WARP12) from the header
/// naming @p shader to the next header.
std::vector<std::string> expected_section(const std::string& shader,
                                          const std::string& file = "d3d12_sm6_exec_probe.expected",
                                          const std::string& adapter = "WARP12") {
    std::ifstream in(std::string(VITREO_SOURCE_DIR) + "/tests/oracle_data/" + file);
    std::vector<std::string> out;
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.rfind("d3d12_sm6_exec_probe: ", 0) == 0) {
            inside = line == "d3d12_sm6_exec_probe: " + shader + " on " + adapter;
            // Compared against Vitreo's header, which names WARP12.
            if (inside) line = "d3d12_sm6_exec_probe: " + shader + " on WARP12";
        }
        if (inside) out.push_back(line);
    }
    return out;
}

u32 gen_src_word(u32 w) { return w * 0x9E3779B1u ^ (w << 7); }

/// The generic compute harness of d3d12_sm6_exec_probe.c
/// (d3d12_sm6_exec_generic.h), through the D3D12 frontend: root signature
/// [0] 4 root constants b0 = (3, 5, 0x80000000, 7), [1] a table of SRV
/// t0-t3 then UAV u0-u2; inputs and the printout as the probe states them.
std::vector<std::string> run_generic(Harness& h, bool is_64bit, const char* name,
                                     bool dump_structured, u32& pso_hr, u32 root_flags = 0) {
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    RootParameter constants;
    constants.parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.num_32bit_values = 4;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srvs;
    srvs.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvs.num_descriptors = 4;
    RootDescriptorRange uavs;
    uavs.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavs.num_descriptors = 3;
    uavs.offset_in_descriptors_from_table_start = 4;
    table.ranges = {srvs, uavs};
    rs.parameters = {constants, table};
    rs.flags = root_flags;
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    const Sm6Blob& found = sm6_blob(name);
    std::vector<u8> code(found.code, found.code + found.size);
    // "!name": the same container with its digest zeroed (unsigned).
    if (name[0] == '!') {
        const Sm6Blob& base = sm6_blob(name + 1);
        code.assign(base.code, base.code + base.size);
        std::fill(code.begin() + 4, code.begin() + 20, u8{0});
    }
    const struct { const u8* code; usize size; } shader{code.data(), code.size()};
    EXPECT_TRUE(h.memory.write_bytes(kShader, shader.code, shader.size).is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[is_64bit];
    h.zero(kDesc, cl.size);
    h.write_ptr(kDesc + cl.pRootSignature, root);
    h.write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h.write_ptr(kDesc + cl.CS_BytecodeLength, shader.size);
    pso_hr = h.com(dev, DeviceSlot::CreateComputePipelineState,
                   {{kDesc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);

    constexpr u32 kSrcOff = 0, kStsOff = 1024, kTbfOff = 1536, kTexOff = 2048, kZeroOff = 8192;
    const GuestAddr upload =
        h.buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 16384);
    EXPECT_EQ(h.com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h.read_ptr(kOut2);
    h.zero(mapped, 16384);
    for (u32 e = 0; e < 256; ++e) h.w32(mapped + kSrcOff + e * 4, gen_src_word(e));
    const auto f = [](float v) {
        u32 b = 0;
        std::memcpy(&b, &v, 4);
        return b;
    };
    for (u32 e = 0; e < 16; ++e) {
        h.w32(mapped + kStsOff + e * 16, 3 * e + 1);
        h.w32(mapped + kStsOff + e * 16 + 4, f(0.5f * static_cast<float>(e)));
        h.w32(mapped + kStsOff + e * 16 + 8, 7 * e);
        h.w32(mapped + kStsOff + e * 16 + 12, e ^ 0x55u);
    }
    for (u32 e = 0; e < 8; ++e) {
        const float fe = static_cast<float>(e);
        h.w32(mapped + kTbfOff + e * 16, f(fe));
        h.w32(mapped + kTbfOff + e * 16 + 4, f(2.0f * fe + 1.0f));
        h.w32(mapped + kTbfOff + e * 16 + 8, f(0.25f * fe));
        h.w32(mapped + kTbfOff + e * 16 + 12, f(-fe));
    }
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const GuestAddr t = mapped + kTexOff + y * 256 + x * 16;
            h.w32(t, f(static_cast<float>(x + 4 * y)));
            h.w32(t + 4, f(0.5f * static_cast<float>(x)));
            h.w32(t + 8, f(0.25f * static_cast<float>(y)));
            h.w32(t + 12, f(1.0f));
        }
    }
    const GuestAddr tex = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4,
                                     DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    const u32 uflag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const GuestAddr u0 = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 4096, uflag);
    const GuestAddr u1 = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 256, uflag);
    const GuestAddr u2 = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 256, uflag);
    const GuestAddr readback =
        h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4096 + 512);

    const GuestAddr heap = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7, true);
    EXPECT_EQ(h.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize,
                    {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}}) != 0, true);
    const u32 inc = h.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize,
                          {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}});
    const u64 cpu = h.cpu_start(heap);
    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[is_64bit];
    const auto srv = [&](GuestAddr res, u32 slot, u32 format, u32 dim, u64 first, u32 count, u32 stride,
                         u32 flags) {
        h.zero(kDesc, sl.size);
        h.w32(kDesc + sl.Format, format);
        h.w32(kDesc + sl.ViewDimension, dim);
        h.w32(kDesc + sl.Shader4ComponentMapping, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        if (dim == D3D12_SRV_DIMENSION_BUFFER) {
            h.w64(kDesc + sl.Buffer_FirstElement, first);
            h.w32(kDesc + sl.Buffer_NumElements, count);
            h.w32(kDesc + sl.Buffer_StructureByteStride, stride);
            h.w32(kDesc + sl.Buffer_Flags, flags);
        } else {
            h.w32(kDesc + sl.Texture2D_MipLevels, 1);
        }
        (void)h.com(dev, DeviceSlot::CreateShaderResourceView, {{res}, {kDesc}, {cpu + u64{slot} * inc}});
    };
    srv(upload, 0, DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER, kSrcOff / 4, 256, 0,
        D3D12_BUFFER_SRV_FLAG_RAW);
    srv(upload, 1, DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, kStsOff / 16, 16, 16, 0);
    srv(upload, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, kTbfOff / 16, 8, 0, 0);
    srv(tex, 3, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, 0, 0, 0, 0);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[is_64bit];
    const auto uav = [&](GuestAddr res, u32 slot, u32 format, u32 count, u32 stride, u32 flags) {
        h.zero(kDesc, ul.size);
        h.w32(kDesc + ul.Format, format);
        h.w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
        h.w32(kDesc + ul.Buffer_NumElements, count);
        h.w32(kDesc + ul.Buffer_StructureByteStride, stride);
        h.w32(kDesc + ul.Buffer_Flags, flags);
        (void)h.com(dev, DeviceSlot::CreateUnorderedAccessView, {{res}, {0}, {kDesc}, {cpu + u64{slot} * inc}});
    };
    uav(u0, 4, DXGI_FORMAT_R32_TYPELESS, 1024, 0, D3D12_BUFFER_UAV_FLAG_RAW);
    uav(u1, 5, DXGI_FORMAT_UNKNOWN, 16, 16, 0);
    uav(u2, 6, DXGI_FORMAT_R32_UINT, 17, 0, 0);

    const GuestAddr list = h.list(dev, pso);
    {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[is_64bit];
        h.zero(kDesc, l.size);
        h.zero(kDesc2, l.size);
        h.write_ptr(kDesc + l.pResource, tex);
        h.w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h.write_ptr(kDesc2 + l.pResource, upload);
        h.w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h.w64(kDesc2 + l.PlacedFootprint_Offset, kTexOff);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Width, 4);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Height, 4);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, 1);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, 256);
        (void)h.com(list, GraphicsCommandListSlot::CopyTextureRegion,
                    {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }
    h.transition(list, tex, D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (const auto& [res, bytes] : {std::pair{u0, 4096u}, std::pair{u1, 256u}, std::pair{u2, 256u}}) {
        (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                    {{res}, Harness::wide(0), {upload}, Harness::wide(kZeroOff), Harness::wide(bytes)});
        h.transition(list, res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    h.write_ptr(kArray, heap);
    (void)h.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    const u32 roots[4] = {3u, 5u, 0x80000000u, 7u};
    EXPECT_TRUE(h.memory.write_bytes(kOut2, reinterpret_cast<const u8*>(roots), sizeof(roots)).is_ok());
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRoot32BitConstants, {{0}, {4}, {kOut2}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                {{1}, Harness::wide(h.gpu_start(heap))});
    (void)h.com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    u64 at = 0;
    for (const auto& [res, bytes] : {std::pair{u0, 4096u}, std::pair{u1, 256u}, std::pair{u2, 256u}}) {
        h.transition(list, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                    {{readback}, Harness::wide(at), {res}, Harness::wide(0), Harness::wide(bytes)});
        at += bytes;
    }
    h.run(dev, queue, list);
    const std::vector<u8> out = h.read_back(readback, 4096 + 512);
    const auto word = [&](usize i) {
        u32 v = 0;
        std::memcpy(&v, out.data() + i * 4, 4);
        return v;
    };
    std::vector<std::string> lines{std::string("d3d12_sm6_exec_probe: ") + name + " on WARP12"};
    char line[160];
    for (u32 e = 0; e < 1024; e += 8) {
        std::snprintf(line, sizeof(line), "u0 %04u: %08x %08x %08x %08x %08x %08x %08x %08x", e,
                      word(e), word(e + 1), word(e + 2), word(e + 3), word(e + 4), word(e + 5),
                      word(e + 6), word(e + 7));
        lines.push_back(line);
    }
    if (dump_structured) {
        for (u32 e = 0; e < 16; ++e) {
            std::snprintf(line, sizeof(line), "u1 %02u: %08x %08x %08x %08x", e, word(1024 + e * 4),
                          word(1024 + e * 4 + 1), word(1024 + e * 4 + 2), word(1024 + e * 4 + 3));
            lines.push_back(line);
        }
        for (u32 e = 0; e < 17; ++e) {
            std::snprintf(line, sizeof(line), "u2 %02u: %08x", e, word(1024 + 64 + e));
            lines.push_back(line);
        }
    }
    return lines;
}

/// Compares @p got with WARP line for line. Words whose u0 index has
/// (index % period) in @p inexact are results of operations whose precision
/// D3D leaves to the implementation (D3D11.3 3.1: division 2.5 ulp, rsq,
/// sqrt, exp, log); those -- WARP and the RTX 4080 oracle differ in exactly
/// these (d3d12_sm6_exec_probe.winoracle-gpu.expected) -- may differ by a
/// few units in the last place. Every other word is exact.
void expect_matches_warp(const std::vector<std::string>& got, const std::string& name,
                         u32 period = 0, std::vector<u32> inexact = {}) {
    const std::vector<std::string> want = expected_section(name);
    ASSERT_FALSE(want.empty()) << name;
    ASSERT_EQ(got.size(), want.size()) << name;
    for (usize i = 0; i < got.size(); ++i) {
        if (got[i] == want[i]) continue;
        bool ok = false;
        if (period != 0 && got[i].rfind("u0 ", 0) == 0) {
            ok = true;
            const u32 base = static_cast<u32>(std::stoul(got[i].substr(3, 4)));
            for (u32 k = 0; k < 8; ++k) {
                const u32 g = static_cast<u32>(std::stoul(got[i].substr(9 + k * 9, 8), nullptr, 16));
                const u32 w = static_cast<u32>(std::stoul(want[i].substr(9 + k * 9, 8), nullptr, 16));
                const u32 index = base + k;
                const bool loose = std::find(inexact.begin(), inexact.end(), index % period) != inexact.end();
                const u32 distance = g > w ? g - w : w - g;
                // cs_alu's three UAV atomics land on words 1021-1023, which
                // thread 63 also stores plainly: the HLSL leaves their order
                // to the implementation, so those words are not compared.
                if (period == 16 && index >= 1021) continue;
                if (g != w && (!loose || distance > 64)) ok = false;
            }
        }
        EXPECT_TRUE(ok) << name << " line " << i << "\n  got  " << got[i] << "\n  want " << want[i];
    }
}

TEST_P(D3D12DxilMetalTest, FlowShaderMatchesWarp) {
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_flow", false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_warp(lines, "cs_flow");
}

TEST_P(D3D12DxilMetalTest, ResourceShaderMatchesWarp) {
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_res", true, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_warp(lines, "cs_res");
}

TEST_P(D3D12DxilMetalTest, AluShaderMatchesWarp) {
    // cs_alu: thread t writes words 16t..16t+15; o[6] (fdiv), o[7] (sqrt,
    // rsqrt), o[9] (exp2, log2) carry implementation-defined precision.
    for (const char* name : {"cs_alu", "cs_alu_65"}) {
        u32 hr = 0;
        const auto lines = run_generic(*h, GetParam(), name, false, hr);
        ASSERT_EQ(hr, S_OK) << name;
        expect_matches_warp(lines, name, 16, {6, 7, 9});
    }
}

/// d3d12_sm6_exec_probe.c draw(): vs_tri/ps_tri or vs_quad/ps_tex into a
/// 16x16 R8G8B8A8_UNORM target cleared to (0, 0, 0, 1), printed as the
/// probe prints it.
std::vector<std::string> run_draw(Harness& h, bool is_64bit, bool quad, u32& pso_hr) {
    constexpr u32 kRt = 16, kRow = 256;
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (quad) {
        RootParameter table;
        table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootDescriptorRange srv;
        srv.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv.num_descriptors = 1;
        table.ranges = {srv};
        rs.parameters = {table};
        StaticSampler s;
        s.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.address_u = s.address_v = s.address_w = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.max_anisotropy = 1;
        s.comparison_func = D3D12_COMPARISON_FUNC_NEVER;
        s.max_lod = 3.402823466e+38f;
        rs.static_samplers = {s};
    }
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x2000;
    constexpr GuestAddr kPosName = kData + 0x1A000;
    constexpr GuestAddr kColName = kData + 0x1A020;
    constexpr GuestAddr kElements = kData + 0x1A100;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kVb = kData + 0x1A580;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kClear = kData + 0x1A5E0;
    const Sm6Blob& vs = sm6_blob(quad ? "vs_quad" : "vs_tri");
    const Sm6Blob& ps = sm6_blob(quad ? "ps_tex" : "ps_tri");
    EXPECT_TRUE(h.memory.write_bytes(kVs, vs.code, vs.size).is_ok());
    EXPECT_TRUE(h.memory.write_bytes(kPs, ps.code, ps.size).is_ok());
    EXPECT_TRUE(h.memory.write_bytes(kPosName, reinterpret_cast<const u8*>("POSITION"), 9).is_ok());
    EXPECT_TRUE(h.memory.write_bytes(kColName, reinterpret_cast<const u8*>("COLOR"), 6).is_ok());
    const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[is_64bit];
    h.zero(kElements, static_cast<usize>(il.size) * 2);
    h.write_ptr(kElements + il.SemanticName, kPosName);
    h.w32(kElements + il.Format, DXGI_FORMAT_R32G32B32_FLOAT);
    h.write_ptr(kElements + il.size + il.SemanticName, kColName);
    h.w32(kElements + il.size + il.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
    h.w32(kElements + il.size + il.AlignedByteOffset, 12);
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[is_64bit];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[is_64bit];
    h.zero(kDesc, gl.size);
    h.write_ptr(kDesc + gl.pRootSignature, root);
    h.write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
    h.write_ptr(kDesc + gl.VS_BytecodeLength, vs.size);
    h.write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
    h.write_ptr(kDesc + gl.PS_BytecodeLength, ps.size);
    const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
    h.w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
    h.w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h.w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h.w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h.w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h.write_ptr(kDesc + gl.InputLayout_pInputElementDescs, kElements);
    h.w32(kDesc + gl.InputLayout_NumElements, quad ? 0 : 2);
    h.w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    h.w32(kDesc + gl.NumRenderTargets, 1);
    h.w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
    h.w32(kDesc + gl.SampleDesc_Count, 1);
    pso_hr = h.com(dev, DeviceSlot::CreateGraphicsPipelineState,
                   {{kDesc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);

    const GuestAddr color = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr tex = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    const GuestAddr upload = h.buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 4096);
    const GuestAddr out = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kRow * kRt);
    EXPECT_EQ(h.com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h.read_ptr(kOut2);
    h.zero(mapped, 4096);
    const float verts[3][7] = {{-0.75f, -0.75f, 0.5f, 1.f, 0.f, 0.f, 1.f},
                               {0.f, 0.75f, 0.5f, 0.f, 1.f, 0.f, 1.f},
                               {0.75f, -0.75f, 0.5f, 0.f, 0.f, 1.f, 1.f}};
    EXPECT_TRUE(h.memory.write_bytes(mapped, reinterpret_cast<const u8*>(verts), sizeof(verts)).is_ok());
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 texel = (64 * x) | ((64 * y) << 8) | (128u << 16) | ((x == 3 && y == 3 ? 0u : 255u) << 24);
            h.w32(mapped + 1024 + y * 256 + x * 4, texel);
        }
    }
    const GuestAddr rtvs = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h.cpu_start(rtvs);
    (void)h.com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h.write_ptr(kRtvSlot, rtv);
    const GuestAddr srvs = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    (void)h.com(dev, DeviceSlot::CreateShaderResourceView, {{tex}, {0}, {h.cpu_start(srvs)}});
    const float clear_rgba[4] = {0.f, 0.f, 0.f, 1.f};
    EXPECT_TRUE(h.memory.write_bytes(kClear, reinterpret_cast<const u8*>(clear_rgba), sizeof(clear_rgba)).is_ok());
    const auto& vp = layout::D3D12_VIEWPORT[is_64bit];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h.w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[is_64bit];
    h.w32(kScissor + rect.left, 0);
    h.w32(kScissor + rect.top, 0);
    h.w32(kScissor + rect.right, kRt);
    h.w32(kScissor + rect.bottom, kRt);
    const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[is_64bit];
    h.w64(kVb + vb.BufferLocation, h.gpu_va(upload));
    h.w32(kVb + vb.SizeInBytes, 84);
    h.w32(kVb + vb.StrideInBytes, 28);

    const GuestAddr list = h.list(dev, pso);
    {
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[is_64bit];
        h.zero(kDesc, l.size);
        h.zero(kDesc2, l.size);
        h.write_ptr(kDesc + l.pResource, tex);
        h.w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h.write_ptr(kDesc2 + l.pResource, upload);
        h.w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h.w64(kDesc2 + l.PlacedFootprint_Offset, 1024);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Width, 4);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Height, 4);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, 1);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, 256);
        (void)h.com(list, GraphicsCommandListSlot::CopyTextureRegion, {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
    }
    h.transition(list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    (void)h.com(list, GraphicsCommandListSlot::ClearRenderTargetView, {{rtv}, {kClear}, {0}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
    (void)h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                {{quad ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
    (void)h.com(list, GraphicsCommandListSlot::IASetVertexBuffers, {{0}, {1}, {kVb}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    if (quad) {
        h.write_ptr(kArray, srvs);
        (void)h.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
        (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable,
                    {{0}, Harness::wide(h.gpu_start(srvs))});
    }
    (void)h.com(list, GraphicsCommandListSlot::OMSetRenderTargets, {{1}, {kRtvSlot}, {0}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::DrawInstanced, {{quad ? 4u : 3u}, {1}, {0}, {0}});
    h.transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    h.copy_to_footprint(list, out, color, DXGI_FORMAT_R8G8B8A8_UNORM, kRt, kRt);
    h.run(dev, queue, list);
    const std::vector<u8> pixels = h.read_back(out, kRow * kRt);
    std::vector<std::string> lines{std::string("d3d12_sm6_exec_probe: ") +
                                   (quad ? "draw_quad" : "draw_tri") + " on WARP12"};
    for (u32 y = 0; y < kRt; ++y) {
        std::string line;
        char cell[16];
        std::snprintf(cell, sizeof(cell), "y%02u:", y);
        line = cell;
        for (u32 x = 0; x < kRt; ++x) {
            u32 v = 0;
            std::memcpy(&v, pixels.data() + y * kRow + x * 4, 4);
            std::snprintf(cell, sizeof(cell), " %08x", v);
            line += cell;
        }
        lines.push_back(line);
    }
    return lines;
}

/// Pixels per channel within @p tolerance of WARP's.
void expect_pixels_match_warp(const std::vector<std::string>& got, const std::string& name,
                              u32 tolerance) {
    const std::vector<std::string> want = expected_section(name);
    ASSERT_EQ(got.size(), want.size()) << name;
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 g = static_cast<u32>(std::stoul(got[i].substr(5 + x * 9, 8), nullptr, 16));
            const u32 w = static_cast<u32>(std::stoul(want[i].substr(5 + x * 9, 8), nullptr, 16));
            u32 worst = 0;
            for (u32 s = 0; s < 32; s += 8) {
                const i32 d = static_cast<i32>((g >> s) & 255u) - static_cast<i32>((w >> s) & 255u);
                worst = std::max(worst, static_cast<u32>(d < 0 ? -d : d));
            }
            EXPECT_LE(worst, tolerance) << name << " pixel " << x << "," << (i - 1) << " got "
                                        << std::hex << g << " want " << w;
        }
    }
}

TEST_P(D3D12DxilMetalTest, TriangleMatchesWarp) {
    // WARP and the RTX 4080 agree on every pixel (coverage, interpolation,
    // UNORM conversion): exact.
    u32 hr = 0;
    const auto lines = run_draw(*h, GetParam(), false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_pixels_match_warp(lines, "draw_tri", 0);
}

TEST_P(D3D12DxilMetalTest, TexturedQuadMatchesWarp) {
    // Linear filtering's weight precision is the implementation's: WARP and
    // the RTX 4080 differ by up to 1 in a channel in 227 of 256 pixels.
    u32 hr = 0;
    const auto lines = run_draw(*h, GetParam(), true, hr);
    ASSERT_EQ(hr, S_OK);
    expect_pixels_match_warp(lines, "draw_quad", 2);
}

TEST_P(D3D12DxilMetalTest, WaveShaderMatchesWarp) {
    // cs_wave's words do not depend on the lane count: WARP (4 lanes), the
    // RTX 4080 and Apple GPUs (32) all write 1 everywhere.
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_wave", false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_warp(lines, "cs_wave");
}

TEST_P(D3D12DxilMetalTest, Int64ShaderMatchesWarp) {
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_int64", false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_warp(lines, "cs_int64");
}

TEST_P(D3D12DxilMetalTest, HalfShaderMatchesWarp) {
    // cs_half thread t writes words 4t..4t+3; word 0 is half(a * b + a / b),
    // whose division precision is the implementation's (WARP and the RTX
    // 4080 agree on every word).
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_half", false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_warp(lines, "cs_half", 4, {0, 3});
}

/// Compares @p got with the RTX 4080's section (d3d12_sm6x_probe.
/// winoracle-gpu.expected) word by word; words whose index modulo
/// @p period is in @p loose may differ by up to @p ulps.
void expect_matches_gpu_in(const std::vector<std::string>& got, const std::vector<std::string>& want,
                           const std::string& name, u32 period, std::vector<u32> loose, u32 ulps);

void expect_matches_gpu(const std::vector<std::string>& got, const std::string& name, u32 period = 0,
                        std::vector<u32> loose = {}, u32 ulps = 0) {
    const std::vector<std::string> want =
        expected_section(name, "d3d12_sm6x_probe.winoracle-gpu.expected", "GPU");
    expect_matches_gpu_in(got, want, name, period, std::move(loose), ulps);
}

void expect_matches_gpu_in(const std::vector<std::string>& got, const std::vector<std::string>& want,
                           const std::string& name, u32 period, std::vector<u32> loose, u32 ulps) {
    ASSERT_FALSE(want.empty()) << name;
    ASSERT_EQ(got.size(), want.size()) << name;
    for (usize i = 0; i < got.size(); ++i) {
        if (got[i] == want[i]) continue;
        bool ok = got[i].rfind("u0 ", 0) == 0 && period != 0;
        if (ok) {
            const u32 base = static_cast<u32>(std::stoul(got[i].substr(3, 4)));
            for (u32 k = 0; k < 8; ++k) {
                const u32 g = static_cast<u32>(std::stoul(got[i].substr(9 + k * 9, 8), nullptr, 16));
                const u32 w = static_cast<u32>(std::stoul(want[i].substr(9 + k * 9, 8), nullptr, 16));
                if (g == w) continue;
                const bool is_loose =
                    std::find(loose.begin(), loose.end(), (base + k) % period) != loose.end();
                const u32 distance = g > w ? g - w : w - g;
                if (!is_loose || distance > ulps) ok = false;
            }
        }
        EXPECT_TRUE(ok) << name << " line " << i << "\n  got  " << got[i] << "\n  want " << want[i];
    }
}

TEST_P(D3D12DxilMetalTest, WaveIntrinsicsMatchTheGpu) {
    // Every SM 6.0 wave intrinsic and the 6.5 WaveMatch / WaveMultiPrefix
    // ones, 32-, 64- and 16-bit, in uniform and divergent flow. Each word is
    // a flag against the spec's value (1), or 2 for a lane that did not
    // take part, which depends on the lane count: 32 here and on the RTX
    // 4080 (WARP's 4 lanes give a different pattern of 2s).
    if (graphics::MetalDevice::instance().simd_group_width() != 32) GTEST_SKIP() << "the GPU's SIMD width is not 32";
    for (const char* name : {"cs_wavex", "cs_wave16"}) {
        u32 hr = 0;
        const auto lines = run_generic(*h, GetParam(), name, false, hr);
        ASSERT_EQ(hr, S_OK) << name;
        expect_matches_gpu(lines, name);
    }
}

TEST_P(D3D12DxilMetalTest, Int64OpsMatchTheGpu) {
    // Thread t writes 8 64-bit words (16 words from 16t); r[5] and r[6]
    // (words 10-13) come from int64 <-> float conversions and float
    // arithmetic, whose rounding WARP and the RTX 4080 already differ in.
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_int64x", false, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_gpu(lines, "cs_int64x", 16, {10, 11, 12, 13}, ~0u);
}

TEST_P(D3D12DxilMetalTest, Native16BitOpsMatchTheGpu) {
    // Half results are IEEE binary16, rounded to nearest even, overflow to
    // infinity, denormals kept: the RTX 4080's (WARP clamps 65520 to 65504
    // and truncates some denormals). Each word packs up to two halves,
    // compared half by half: any NaN equals any NaN (D3D does not keep NaN
    // payloads; the RTX 4080 passes the input's through, Metal
    // canonicalises), and words 0 (sqrt), 12 (a sum of products, which the
    // fast-math flags dxc writes let an implementation fuse) and 13
    // (division) may differ by one half ulp.
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_halfx", false, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = expected_section("cs_halfx", "d3d12_sm6x_probe.winoracle-gpu.expected", "GPU");
    ASSERT_EQ(got.size(), want.size());
    const auto nan16 = [](u32 x) { return (x & 0x7C00u) == 0x7C00u && (x & 0x3FFu) != 0; };
    for (usize i = 1; i < got.size(); ++i) {
        const u32 base = static_cast<u32>(std::stoul(got[i].substr(3, 4)));
        for (u32 k = 0; k < 8; ++k) {
            const u32 g = static_cast<u32>(std::stoul(got[i].substr(9 + k * 9, 8), nullptr, 16));
            const u32 w = static_cast<u32>(std::stoul(want[i].substr(9 + k * 9, 8), nullptr, 16));
            const u32 word = (base + k) % 16;
            const bool loose = word == 0 || word == 12 || word == 13;
            bool ok = true;
            for (u32 shift = 0; shift < 32; shift += 16) {
                const u32 gh = (g >> shift) & 0xFFFFu, wh = (w >> shift) & 0xFFFFu;
                if (gh == wh || (nan16(gh) && nan16(wh))) continue;
                const u32 distance = gh > wh ? gh - wh : wh - gh;
                if (!loose || distance > 1) ok = false;
            }
            EXPECT_TRUE(ok) << "cs_halfx word " << (base + k) << std::hex << " got " << g << " want " << w;
        }
    }
}

TEST_P(D3D12DxilMetalTest, Typed16BitViewsMatchTheGpu) {
    // Buffer<half4> / Texture2D<half4> over float32 data (rounded to half)
    // and RWBuffer<uint16_t> over R32_UINT: the RTX 4080 stores the uint16
    // zero-extended (WARP sign-extends 0x801f to 0xffff801f, which the
    // element type, U16, does not allow).
    u32 hr = 0;
    const auto lines = run_generic(*h, GetParam(), "cs_res16", true, hr);
    ASSERT_EQ(hr, S_OK);
    expect_matches_gpu(lines, "cs_res16");
}

TEST_P(D3D12DxilMetalTest, DoublesMatchTheGpu) {
    // DXIL doubles (fadd .. fdiv, fma, min / max, abs, saturate, fcmp,
    // MakeDouble / SplitDouble, groupshared doubles, conversions to and from
    // i32 / u32 / i64 / u64 / float / half): IEEE binary64, correctly
    // rounded, so bit-exact with the RTX 4080 (d3d12_dxilc_probe.
    // winoracle-gpu.expected). cs_double16's fptrunc double -> half rounds
    // once (word 1 holds a half tie that rounding through float32 breaks).
    for (const char* name : {"cs_double", "cs_double2", "cs_double16"}) {
        u32 hr = 0;
        const auto lines = run_generic(*h, GetParam(), name, false, hr);
        ASSERT_EQ(hr, S_OK) << name;
        const std::vector<std::string> want =
            expected_section(name, "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU");
        ASSERT_FALSE(want.empty()) << name;
        ASSERT_EQ(lines.size(), want.size()) << name;
        if (std::string(name) != "cs_double") {
            for (usize i = 0; i < lines.size(); ++i) EXPECT_EQ(lines[i], want[i]) << name << " line " << i;
            continue;
        }
        // cs_double r[3] (words 6-7) is min * max + abs: dxc marks the
        // fmul / fadd fast, which lets an implementation contract them (the
        // RTX 4080 does; Vitreo rounds twice), so its low word may differ
        // by 1.
        expect_matches_gpu_in(lines, want, name, 16, {6}, 1);
    }
}

/// A binary16 value (exact in double).
double half_value(u32 bits) {
    const double sign = (bits & 0x8000u) != 0 ? -1.0 : 1.0;
    const u32 e = (bits >> 10) & 0x1Fu, m = bits & 0x3FFu;
    if (e == 0) return sign * std::ldexp(static_cast<double>(m), -24);
    return sign * std::ldexp(static_cast<double>(m | 0x400u), static_cast<int>(e) - 25);
}

TEST_P(D3D12DxilMetalTest, WaveOpsOnDoublesMatchTheGpu) {
    // Wave reductions, prefixes, reads, AllEqual, Match and quad reads of
    // doubles (d3d12_dxilc.hlsl cs_wavedbl): flags against the spec's
    // values, 2 where a lane took no part (depends on the 32-lane width).
    if (graphics::MetalDevice::instance().simd_group_width() != 32) GTEST_SKIP() << "the GPU's SIMD width is not 32";
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_wavedbl", false, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, expected_section("cs_wavedbl", "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU"));
}

TEST_P(D3D12DxilMetalTest, ResourceStatusMatchesWarp) {
    // Loads with a status (raw, structured, typed buffer, texture, typed
    // UAV) and CheckAccessFullyMapped: none of these resources is tiled, so
    // every status reads fully mapped (D3D11.2 tiled resources), as WARP12
    // reports (d3d12_dxilc_probe.expected cs_status word 5 = 0x1f). The RTX
    // 4080 reports raw and structured loads unmapped (0x1c), which the
    // contract does not allow, so WARP is the baseline here.
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_status", false, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, expected_section("cs_status", "d3d12_dxilc_probe.expected", "WARP12"));
}

TEST_P(D3D12DxilMetalTest, Fp32DenormModesMatchTheGpu) {
    // SM 6.2 "fp32-denorm-mode" (dxc -denorm): preserve keeps float32
    // denormal inputs and results, ftz flushes them to signed zero (DXIL.rst,
    // Functions; the DXC wiki "Denorm Mode"). WARP12 and the RTX 4080 agree
    // bit for bit (d3d12_dxilc.hlsl cs_denorm). The Apple GPU flushes, so
    // ftz runs on the hardware and preserve through VX_F32P.
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_denorm_ftz", false, hr);
    ASSERT_EQ(hr, S_OK);
    // Word 6 is max(flushed denormal, 0): IEEE 754-2008 maxNum leaves the
    // sign of max(-0, +0) open (the GPUs give +0, Metal -0).
    expect_matches_gpu_in(got, expected_section("cs_denorm_ftz", "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU"),
                          "cs_denorm_ftz", 16, {6}, 0x80000000u);
    const auto kept = run_generic(*h, GetParam(), "cs_denorm_preserve", false, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(kept, expected_section("cs_denorm_preserve", "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU"));
}

namespace {

/// How a word of the denormal corpus (d3d12_denorm.hlsl) is judged against
/// WARP12 (d3d12_denorm_probe.expected) and the RTX 4080
/// (d3d12_denorm_probe.winoracle-gpu.expected).
enum class DenormRule : u8 {
    Both,    ///< equal to both GPUs (which agree)
    Gpu,     ///< equal to the RTX 4080's (fused multiply-add; WARP's is not)
    Either,  ///< equal to one of them (they differ within the spec's latitude)
    /// a quotient: a denormal one equal to one of them; a normal one within
    /// D3D11.3's 2.5 ulp of both (the GPUs' own quotients differ by 1 ulp)
    Quotient,
    Free,    ///< an operation the denorm mode does not cover; GPUs disagree
    /// ftz: Either, or any NaN / either signed zero where both give one
    Flushed,
};

bool is_nan(u32 x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }

std::vector<u32> section_words(const std::vector<std::string>& lines) {
    std::vector<u32> words;
    for (const auto& line : lines) {
        if (line.rfind("u0 ", 0) != 0) continue;
        for (u32 k = 0; k < 8; ++k) words.push_back(static_cast<u32>(std::stoul(line.substr(9 + k * 9, 8), nullptr, 16)));
    }
    return words;
}

void expect_denorm_corpus(Harness& h, bool is_64bit, const std::string& name, const std::array<DenormRule, 16>& rules) {
    u32 hr = 0;
    const auto got_lines = run_generic(h, is_64bit, name.c_str(), false, hr);
    ASSERT_EQ(hr, S_OK) << name;
    const auto got = section_words(got_lines);
    const auto warp = section_words(expected_section(name, "d3d12_denorm_probe.expected", "WARP12"));
    const auto gpu = section_words(expected_section(name, "d3d12_denorm_probe.winoracle-gpu.expected", "GPU"));
    ASSERT_EQ(got.size(), 1024u) << name;
    ASSERT_EQ(warp.size(), 1024u) << name;
    ASSERT_EQ(gpu.size(), 1024u) << name;
    int failures = 0;
    for (u32 i = 0; i < 1024; ++i) {
        const DenormRule rule = rules[i % 16];
        bool ok = true;
        switch (rule) {
        case DenormRule::Both: ok = got[i] == warp[i] && got[i] == gpu[i]; break;
        case DenormRule::Gpu: ok = got[i] == gpu[i]; break;
        case DenormRule::Either: ok = got[i] == gpu[i] || got[i] == warp[i]; break;
        case DenormRule::Flushed:
            // Any NaN where both give one; either signed zero where both
            // give a zero (IEEE 754-2008 minNum / maxNum leave its sign open).
            ok = got[i] == gpu[i] || got[i] == warp[i] ||
                 (is_nan(got[i]) && is_nan(gpu[i]) && is_nan(warp[i])) ||
                 ((got[i] & 0x7FFFFFFFu) == 0u && (gpu[i] & 0x7FFFFFFFu) == 0u && (warp[i] & 0x7FFFFFFFu) == 0u);
            break;
        case DenormRule::Quotient: {
            const auto ulps = [](u32 x, u32 y) { return x > y ? x - y : y - x; };
            const bool normal = (got[i] & 0x7F800000u) != 0u && (got[i] & 0x7F800000u) != 0x7F800000u;
            ok = got[i] == gpu[i] || got[i] == warp[i] || (is_nan(got[i]) && is_nan(gpu[i]) && is_nan(warp[i])) ||
                 (normal && (got[i] >> 31) == (gpu[i] >> 31) && ulps(got[i], gpu[i]) <= 2u &&
                  ulps(got[i], warp[i]) <= 2u);
            break;
        }
        case DenormRule::Free: break;
        }
        if (!ok && ++failures <= 12) {
            ADD_FAILURE() << name << " word " << i << " (thread " << i / 16 << ", k " << i % 16 << "): got " << std::hex
                          << got[i] << ", WARP " << warp[i] << ", GPU " << gpu[i];
        }
    }
    EXPECT_EQ(failures, 0) << name;
}

}  // namespace

TEST_P(D3D12DxilMetalTest, Fp32DenormPreserveCorpusMatchesTheGpus) {
    // d3d12_denorm.hlsl, -denorm preserve: denormal operands of both signs
    // and results crossing the normal/denormal boundary, per operation
    // class. WARP12 and the RTX 4080 agree bit for bit on add, subtract,
    // multiply, every compare, min, max, clamp and saturate, so Vitreo must
    // too. mad is fused on the RTX 4080 and not on WARP (both allowed:
    // DXIL.rst FMad); Vitreo fuses, as the GPU does.
    using R = DenormRule;
    std::array<R, 16> all{};
    all.fill(R::Both);
    expect_denorm_corpus(*h, GetParam(), "cs_add_preserve", all);
    expect_denorm_corpus(*h, GetParam(), "cs_mul_preserve", all);
    expect_denorm_corpus(*h, GetParam(), "cs_cmp_preserve", all);
    // Words 5, 6, 13 and 14 are a * b + c written as two operations:
    // the RTX 4080 contracts them (the fast flags allow it), WARP and
    // Vitreo round twice.
    std::array<R, 16> mad{};
    mad.fill(R::Gpu);
    for (u32 k : {5u, 6u, 13u, 14u}) mad[k] = R::Either;
    expect_denorm_corpus(*h, GetParam(), "cs_mad_preserve", mad);
    // Division: D3D11.3 allows 2.5 ulp; the GPUs differ by an ulp in 177
    // words. A denormal quotient is Vitreo's correctly rounded one; a
    // normal one is the GPU's divide.
    std::array<R, 16> quotient{};
    quotient.fill(R::Quotient);
    expect_denorm_corpus(*h, GetParam(), "cs_div_preserve", quotient);
    // The rest of the corpus: frac, floor, ceil, round, trunc, sqrt, rsqrt,
    // exp2 and log2 are not "basic arithmetic" (the wiki's list), and the
    // GPUs disagree (WARP keeps denormal inputs, the RTX flushes them);
    // dot, the double conversion, f32tof16, the int round trip, lerp
    // (fmul + fadd: contracted on the RTX 4080, not on WARP), negate and
    // abs are covered.
    expect_denorm_corpus(*h, GetParam(), "cs_misc_preserve",
                         {R::Free, R::Free, R::Free, R::Free, R::Free, R::Free, R::Free, R::Gpu, R::Free, R::Free,
                          R::Both, R::Both, R::Both, R::Either, R::Both, R::Both});
}

TEST_P(D3D12DxilMetalTest, Fp32DenormFtzCorpusMatchesTheGpus) {
    // The same corpus with -denorm ftz: denormal inputs read as signed zero
    // and denormal results flush. NaN bit patterns differ between the GPUs
    // (WARP 0xFFC00000, the RTX 0x7FFFFFFF) and so do signs of flushed
    // zeros in mad, min, max and clamp; each such word must equal one of
    // them.
    using R = DenormRule;
    std::array<R, 16> flushed{};
    flushed.fill(R::Flushed);
    for (const char* name : {"cs_add_ftz", "cs_mul_ftz", "cs_cmp_ftz", "cs_mad_ftz"}) {
        expect_denorm_corpus(*h, GetParam(), name, flushed);
    }
    std::array<R, 16> quotient{};
    quotient.fill(R::Quotient);
    expect_denorm_corpus(*h, GetParam(), "cs_div_ftz", quotient);
}

TEST_P(D3D12DxilMetalTest, DynamicResourcesMatchTheGpus) {
    // SM 6.6 ResourceDescriptorHeap[] (createHandleFromHeap + annotateHandle)
    // over the generic harness's heap, with
    // D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED: raw,
    // structured, typed and texture SRVs, raw / structured / typed UAVs,
    // GetDimensions, a NonUniformResourceIndex index (d3d12_dynres.hlsl).
    // WARP12 and the RTX 4080 agree line for line; without the flag both
    // fail CreateComputePipelineState with E_INVALIDARG.
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_dynres", true, hr,
                                 D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, expected_section("cs_dynres", "d3d12_dynres_probe.expected", "WARP12"));
    EXPECT_EQ(got, expected_section("cs_dynres", "d3d12_dynres_probe.winoracle-gpu.expected", "GPU"));
    (void)run_generic(*h, GetParam(), "cs_dynres", false, hr);
    EXPECT_EQ(hr, E_INVALIDARG);
}

TEST_P(D3D12DxilMetalTest, Dot2AddHalfMatchesTheGpu) {
    // dot2add(a, b, acc) = acc + a.x * b.x + a.y * b.y (SM 6.4). The spec
    // leaves the products' precision open ("need not emit INF due to fp16
    // overflow"): the RTX 4080 rounds each to half (d3d12_dxilc_probe
    // cs_dot2 matches acc + half(p0) + half(p1) to 1 ulp), Vitreo keeps them
    // exact in float32. Each word must lie within the half rounding of the
    // two products (2^-11 of their magnitudes) plus 2 float32 ulps of the
    // GPU's result; the inputs are the shader's (d3d12_dxilc.hlsl cs_dot2).
    u32 hr = 0;
    const auto got = run_generic(*h, GetParam(), "cs_dot2", false, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = expected_section("cs_dot2", "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU");
    ASSERT_EQ(got.size(), want.size());
    const auto word = [](u32 w) { return gen_src_word(w & 255u); };
    for (usize i = 1; i < got.size(); ++i) {
        const u32 base = static_cast<u32>(std::stoul(got[i].substr(3, 4)));
        for (u32 k = 0; k < 8; ++k) {
            const u32 g = static_cast<u32>(std::stoul(got[i].substr(9 + k * 9, 8), nullptr, 16));
            const u32 w = static_cast<u32>(std::stoul(want[i].substr(9 + k * 9, 8), nullptr, 16));
            const u32 t = (base + k) / 16, j = (base + k) % 16;
            const u32 a = word(t * 3 + j), b = word(t * 5 + j + 1);
            const double p0 = half_value(a & 0xbfffu) * half_value(b & 0xbfffu);
            const double p1 = half_value((a >> 16) & 0xbfffu) * half_value((b >> 16) & 0xbfffu);
            float gf = 0, wf = 0;
            std::memcpy(&gf, &g, 4);
            std::memcpy(&wf, &w, 4);
            const double ulp = std::ldexp(1.0, std::ilogb(static_cast<double>(wf)) - 23);
            const double bound = (std::fabs(p0) + std::fabs(p1)) * std::ldexp(1.0, -11) + 2 * ulp;
            EXPECT_LE(std::fabs(static_cast<double>(gf) - static_cast<double>(wf)), bound)
                << "cs_dot2 word " << (base + k) << std::hex << " got " << g << " want " << w;
        }
    }
}

/// d3d12_dxilc_probe.c's pixel harness through the D3D12 frontend: @p vs
/// (SV_VertexID, no input layout) and @p ps with an empty root signature,
/// one 3-vertex triangle list on a 16x16 R32G32B32A32_UINT target cleared
/// to 0, optionally with conservative rasterization; 16 rows of 64 words.
std::vector<std::string> run_pixel(Harness& h, bool is_64bit, const char* name, const char* vs_name,
                                   const char* ps_name, bool conservative, u32& pso_hr) {
    constexpr u32 kRt = 16, kRow = 256;
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x2000;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kClear = kData + 0x1A5E0;
    const Sm6Blob& vs = sm6_blob(vs_name);
    const Sm6Blob& ps = sm6_blob(ps_name);
    EXPECT_NE(vs.code, nullptr) << vs_name;
    EXPECT_NE(ps.code, nullptr) << ps_name;
    if (vs.code == nullptr || ps.code == nullptr) return {};
    EXPECT_TRUE(h.memory.write_bytes(kVs, vs.code, vs.size).is_ok());
    EXPECT_TRUE(h.memory.write_bytes(kPs, ps.code, ps.size).is_ok());
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[is_64bit];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[is_64bit];
    h.zero(kDesc, gl.size);
    h.write_ptr(kDesc + gl.pRootSignature, root);
    h.write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
    h.write_ptr(kDesc + gl.VS_BytecodeLength, vs.size);
    h.write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
    h.write_ptr(kDesc + gl.PS_BytecodeLength, ps.size);
    const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
    h.w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
    h.w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h.w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h.w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h.w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h.w32(kDesc + gl.RasterizerState_ConservativeRaster, conservative ? 1u : 0u);
    h.w32(kDesc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    h.w32(kDesc + gl.NumRenderTargets, 1);
    h.w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R32G32B32A32_UINT);
    h.w32(kDesc + gl.SampleDesc_Count, 1);
    pso_hr = h.com(dev, DeviceSlot::CreateGraphicsPipelineState,
                   {{kDesc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);
    const GuestAddr color = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
                                       DXGI_FORMAT_R32G32B32A32_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const GuestAddr out = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kRow * kRt);
    const GuestAddr rtvs = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h.cpu_start(rtvs);
    (void)h.com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h.write_ptr(kRtvSlot, rtv);
    h.zero(kClear, 16);
    const auto& vp = layout::D3D12_VIEWPORT[is_64bit];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, 4);
        h.w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[is_64bit];
    h.w32(kScissor + rect.left, 0);
    h.w32(kScissor + rect.top, 0);
    h.w32(kScissor + rect.right, kRt);
    h.w32(kScissor + rect.bottom, kRt);
    const GuestAddr list = h.list(dev, pso);
    (void)h.com(list, GraphicsCommandListSlot::ClearRenderTargetView, {{rtv}, {kClear}, {0}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
    (void)h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology, {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    (void)h.com(list, GraphicsCommandListSlot::OMSetRenderTargets, {{1}, {kRtvSlot}, {0}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::DrawInstanced, {{3}, {1}, {0}, {0}});
    h.transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    h.copy_to_footprint(list, out, color, DXGI_FORMAT_R32G32B32A32_UINT, kRt, kRt);
    h.run(dev, queue, list);
    const std::vector<u8> pixels = h.read_back(out, kRow * kRt);
    std::vector<std::string> lines{std::string("d3d12_sm6_exec_probe: ") + name + " on WARP12"};
    for (u32 y = 0; y < kRt; ++y) {
        char cell[16];
        std::snprintf(cell, sizeof(cell), "y%02u:", y);
        std::string line = cell;
        for (u32 x = 0; x < kRt * 4; ++x) {
            u32 v = 0;
            std::memcpy(&v, pixels.data() + y * kRow + x * 4, 4);
            std::snprintf(cell, sizeof(cell), " %08x", v);
            line += cell;
        }
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> dxilc_gpu(const std::string& name) {
    return expected_section(name, "d3d12_dxilc_probe.winoracle-gpu.expected", "GPU");
}

TEST_P(D3D12DxilMetalTest, HelperLanesMatchTheGpu) {
    // SM 6.6 IsHelperLane, read across the quad: the covered pixels and
    // the quad helper lanes are the rasterizer's (the D3D11.3 top-left
    // rule), and by default no helper lane takes part in a wave operation
    // (d3d12_dxilc.hlsl ps_helper; the RTX 4080's pix_helper).
    u32 hr = 0;
    const auto got = run_pixel(*h, GetParam(), "pix_helper", "vs_pix", "ps_helper", false, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, dxilc_gpu("pix_helper"));
}

TEST_P(D3D12DxilMetalTest, QuadVotesAndHelperWavesMatchTheGpu) {
    // SM 6.7 QuadAny / QuadAll over the 2x2 quad (helpers included) and
    // [WaveOpsIncludeHelperLanes]: a quad holding a helper lane sees it in
    // WaveActiveAnyTrue (d3d12_dxilc.hlsl ps_quad, dxc 1.9).
    u32 hr = 0;
    const auto got = run_pixel(*h, GetParam(), "pix_quad", "vs_pix", "ps_quad", false, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, dxilc_gpu("pix_quad"));
}

TEST_P(D3D12DxilMetalTest, InnerCoverageMatchesTheGpu) {
    // SV_InnerCoverage from DXIL under conservative rasterization: the
    // conservatively covered pixels, and 1 in .x where the pixel lies
    // entirely inside the triangle (d3d12_dxilc.hlsl ps_inner).
    u32 hr = 0;
    const auto got = run_pixel(*h, GetParam(), "pix_inner", "vs_pix", "ps_inner", true, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, dxilc_gpu("pix_inner"));
}

TEST_P(D3D12DxilMetalTest, Native16BitSignaturesMatchTheGpu) {
    // half4 / uint16_t2 / int16_t signature elements from the VS to the PS
    // (-enable-16bit-types). The flat uint16 / int16 words must match
    // exactly; the interpolated halves within 2 half ulps (attribute
    // interpolation precision is the implementation's, D3D11.3 3.4.2).
    u32 hr = 0;
    const auto got = run_pixel(*h, GetParam(), "pix_sig16", "vs_sig16", "ps_sig16", false, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = dxilc_gpu("pix_sig16");
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 k = 0; k < 64; ++k) {
            const u32 g = static_cast<u32>(std::stoul(got[i].substr(5 + k * 9, 8), nullptr, 16));
            const u32 w = static_cast<u32>(std::stoul(want[i].substr(5 + k * 9, 8), nullptr, 16));
            if (k % 4 >= 2) {
                EXPECT_EQ(g, w) << "pix_sig16 row " << (i - 1) << " word " << k;
                continue;
            }
            for (u32 shift = 0; shift < 32; shift += 16) {
                const u32 gh = (g >> shift) & 0xFFFFu, wh = (w >> shift) & 0xFFFFu;
                const u32 distance = gh > wh ? gh - wh : wh - gh;
                EXPECT_LE(distance, 2u) << "pix_sig16 row " << (i - 1) << " word " << k << std::hex << " got " << g
                                        << " want " << w;
            }
        }
    }
}

/// The draw harness of d3d12_bary_probe.c (and d3d12_c4_probe.c's draw
/// sections) through the D3D12 frontend: @p vs (SV_VertexID, no input
/// layout) and @p ps with a root signature of one descriptor table holding
/// the raw UAV u1 of @p words zeroed words, into an 8x8 R8G8B8A8_UNORM
/// target; one
/// @p topology draw of @p vertices vertices (@p instances instances from
/// @p start_vertex / @p start_instance), read back as the probe prints it:
/// "## <name>" then 8 words per line.
struct UavDraw {
    const char* name{nullptr};
    const char* vs{nullptr};
    const char* ps{nullptr};
    u32 topology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    u32 topology_type{D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
    u32 vertices{3};
    u32 instances{1};
    u32 start_vertex{0};
    u32 start_instance{0};
    /// DrawIndexedInstanced with these 16-bit indices when not empty
    /// (start_vertex is then BaseVertexLocation).
    std::vector<u16> indices;
    u32 words{64 * 8};
};

std::vector<std::string> run_uav_draw(Harness& h, bool is_64bit, const UavDraw& d, u32& pso_hr) {
    constexpr u32 kRt = 8;
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.base_shader_register = 1;
    table.ranges = {uav_range};
    rs.parameters = {table};
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    constexpr GuestAddr kVs = kShader;
    constexpr GuestAddr kPs = kShader + 0x4000;
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kIbv = kData + 0x1A580;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    const Sm6Blob& vs = sm6_blob(d.vs);
    const Sm6Blob& ps = sm6_blob(d.ps);
    EXPECT_NE(vs.code, nullptr) << d.vs;
    EXPECT_NE(ps.code, nullptr) << d.ps;
    if (vs.code == nullptr || ps.code == nullptr) return {};
    EXPECT_TRUE(h.memory.write_bytes(kVs, vs.code, vs.size).is_ok());
    EXPECT_TRUE(h.memory.write_bytes(kPs, ps.code, ps.size).is_ok());
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[is_64bit];
    const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[is_64bit];
    h.zero(kDesc, gl.size);
    h.write_ptr(kDesc + gl.pRootSignature, root);
    h.write_ptr(kDesc + gl.VS_pShaderBytecode, kVs);
    h.write_ptr(kDesc + gl.VS_BytecodeLength, vs.size);
    h.write_ptr(kDesc + gl.PS_pShaderBytecode, kPs);
    h.write_ptr(kDesc + gl.PS_BytecodeLength, ps.size);
    const GuestAddr rt0 = kDesc + gl.BlendState_RenderTarget;
    h.w32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
    h.w32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
    h.w32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
    h.w32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
    h.w32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
    h.w32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h.w32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h.w32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
    h.w32(kDesc + gl.PrimitiveTopologyType, d.topology_type);
    h.w32(kDesc + gl.NumRenderTargets, 1);
    h.w32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
    h.w32(kDesc + gl.SampleDesc_Count, 1);
    pso_hr = h.com(dev, DeviceSlot::CreateGraphicsPipelineState,
                   {{kDesc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);
    const GuestAddr color = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_DIMENSION_TEXTURE2D, kRt, kRt,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const u32 bytes = d.words * 4;
    const GuestAddr out = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, bytes,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr upload = h.buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      bytes + 4096);
    const GuestAddr readback = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, bytes);
    EXPECT_EQ(h.com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h.read_ptr(kOut2);
    h.zero(mapped, bytes + 4096);
    if (!d.indices.empty()) {
        EXPECT_TRUE(h.memory.write_bytes(mapped + bytes, reinterpret_cast<const u8*>(d.indices.data()),
                                         d.indices.size() * 2).is_ok());
    }
    const GuestAddr heap = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[is_64bit];
    h.zero(kDesc2, ul.size);
    h.w32(kDesc2 + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h.w32(kDesc2 + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h.w32(kDesc2 + ul.Buffer_NumElements, d.words);
    h.w32(kDesc2 + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h.com(dev, DeviceSlot::CreateUnorderedAccessView, {{out}, {0}, {kDesc2}, {h.cpu_start(heap)}});
    const GuestAddr rtvs = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    const u64 rtv = h.cpu_start(rtvs);
    (void)h.com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
    h.write_ptr(kRtvSlot, rtv);
    const auto& vp = layout::D3D12_VIEWPORT[is_64bit];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 b = 0;
        std::memcpy(&b, &value, 4);
        h.w32(at, b);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(kRt));
    put_f(kView + vp.Height, static_cast<float>(kRt));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[is_64bit];
    h.w32(kScissor + rect.left, 0);
    h.w32(kScissor + rect.top, 0);
    h.w32(kScissor + rect.right, kRt);
    h.w32(kScissor + rect.bottom, kRt);
    const GuestAddr list = h.list(dev, pso);
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{out}, Harness::wide(0), {upload}, Harness::wide(0), Harness::wide(bytes)});
    h.transition(list, out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.write_ptr(kArray, heap);
    (void)h.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable,
                {{0}, Harness::wide(h.gpu_start(heap))});
    (void)h.com(list, GraphicsCommandListSlot::OMSetRenderTargets, {{1}, {kRtvSlot}, {0}, {0}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
    (void)h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology, {{d.topology}});
    if (d.indices.empty()) {
        (void)h.com(list, GraphicsCommandListSlot::DrawInstanced,
                    {{d.vertices}, {d.instances}, {d.start_vertex}, {d.start_instance}});
    } else {
        const auto& ib = layout::D3D12_INDEX_BUFFER_VIEW[is_64bit];
        h.zero(kIbv, ib.size);
        h.w64(kIbv + ib.BufferLocation, h.gpu_va(upload) + bytes);
        h.w32(kIbv + ib.SizeInBytes, static_cast<u32>(d.indices.size() * 2));
        h.w32(kIbv + ib.Format, DXGI_FORMAT_R16_UINT);
        (void)h.com(list, GraphicsCommandListSlot::IASetIndexBuffer, {{kIbv}});
        (void)h.com(list, GraphicsCommandListSlot::DrawIndexedInstanced,
                    {{static_cast<u32>(d.indices.size())}, {d.instances}, {0}, {d.start_vertex},
                     {d.start_instance}});
    }
    h.transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{readback}, Harness::wide(0), {out}, Harness::wide(0), Harness::wide(bytes)});
    h.run(dev, queue, list);
    const std::vector<u8> words = h.read_back(readback, bytes);
    std::vector<std::string> lines{std::string("## ") + d.name};
    std::string line;
    for (u32 w = 0; w < d.words; ++w) {
        u32 v = 0;
        std::memcpy(&v, words.data() + w * 4, 4);
        char cell[16];
        std::snprintf(cell, sizeof(cell), "%08x", v);
        line += cell;
        if (w % 8 == 7) {
            lines.push_back(line);
            line.clear();
        } else {
            line += ' ';
        }
    }
    return lines;
}

/// The "## <name>" block of a probe's expected file: its word lines.
std::vector<std::string> probe_section(const std::string& file, const std::string& name) {
    std::ifstream in(std::string(VITREO_SOURCE_DIR) + "/tests/oracle_data/" + file);
    std::vector<std::string> out;
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.rfind("## ", 0) == 0) {
            inside = line == "## " + name;
        } else if (line.empty() || line[0] == '#' || !std::isxdigit(static_cast<unsigned char>(line[0]))) {
            // A comment or a message line ends the block.
            if (inside && !out.empty()) inside = false;
            continue;
        }
        if (inside) out.push_back(line);
    }
    return out;
}

/// Word @p k of line @p i (after the header) of a section.
u32 section_word(const std::vector<std::string>& lines, usize i, u32 k) {
    return static_cast<u32>(std::stoul(lines[i].substr(k * 9, 8), nullptr, 16));
}

TEST_P(D3D12DxilMetalTest, AttributeAtVertexMatchesTheGpu) {
    // SM 6.1 GetAttributeAtVertex (d3d12_bary.hlsl ps_bary*, dxc 1.6) on a
    // 6-vertex list and a 4-vertex strip: per pixel the flat id, the id at
    // vertices 0-2, the three SV_Barycentrics weights and SV_PrimitiveID |
    // GetAttributeAtVertex(attr, 2).y's high half. Vertex k is the order of
    // the weights (an odd strip triangle is v1, v3, v2), measured on the RTX
    // 4080 (d3d12_bary_probe.winoracle-gpu.expected): the ids and the
    // attribute exactly, the weights within 2^-24 of 1.0 (interpolation
    // precision is the implementation's, D3D11.3 3.4.2: Metal's third
    // perspective weight is off by up to 8 ulp of a 0.06 weight, 3e-8).
    for (const char* ps : {"ps_bary", "ps_bary_np", "ps_bary_cen"}) {
        for (const bool strip : {false, true}) {
            const std::string name = std::string(ps) + (strip ? " strip" : " list");
            UavDraw d;
            d.name = name.c_str();
            d.vs = "vs_bary";
            d.ps = ps;
            d.topology = strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            d.vertices = strip ? 4 : 6;
            u32 hr = 0;
            const auto got = run_uav_draw(*h, GetParam(), d, hr);
            ASSERT_EQ(hr, S_OK) << name;
            const auto want = probe_section("d3d12_bary_probe.winoracle-gpu.expected", name);
            ASSERT_EQ(got.size(), want.size()) << name;
            for (usize i = 1; i < got.size(); ++i) {
                for (u32 k = 0; k < 8; ++k) {
                    const u32 g = section_word(got, i, k), w = section_word(want, i, k);
                    if (k < 4 || k == 7) {
                        EXPECT_EQ(g, w) << name << " pixel " << (i - 1) << " word " << k << std::hex << " got " << g
                                        << " want " << w;
                    } else {
                        float gf = 0, wf = 0;
                        std::memcpy(&gf, &g, 4);
                        std::memcpy(&wf, &w, 4);
                        EXPECT_LE(std::fabs(gf - wf), 6e-8f) << name << " pixel " << (i - 1) << " word " << k
                                                             << std::hex << " got " << g << " want " << w;
                    }
                }
            }
        }
    }
}

/// d3d12_a60_c4_probe.c's compute sections through the D3D12 frontend: the
/// probe's textures t0 (8x8 R32G32B32A32_FLOAT, texel (x, y, x + 8 y, 1)),
/// t1 (R32_UINT x + 8 y + 1000) and t3 (R32G32_UINT (x + 8 y, 0xC0DE0000 +
/// y)), a point / wrap static sampler s0, the raw UAV u1 in table 0 and
/// t0..t4 in table 1; one 1x1x1 dispatch of @p cs; @p words words of u1.
std::vector<std::string> run_c4_compute(Harness& h, bool is_64bit, const char* cs, const char* name, u32 words,
                                        u32& pso_hr) {
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    RootParameter t0;
    t0.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange u1;
    u1.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    u1.num_descriptors = 1;
    u1.base_shader_register = 1;
    t0.ranges = {u1};
    RootParameter t1;
    t1.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange srvs;
    srvs.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvs.num_descriptors = 5;
    t1.ranges = {srvs};
    rs.parameters = {t0, t1};
    StaticSampler s;
    s.filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.address_u = s.address_v = s.address_w = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.comparison_func = D3D12_COMPARISON_FUNC_NEVER;
    s.max_lod = 3.402823466e+38f;
    rs.static_samplers = {s};
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    const Sm6Blob& b = sm6_blob(cs);
    EXPECT_NE(b.code, nullptr) << cs;
    if (b.code == nullptr) return {};
    EXPECT_TRUE(h.memory.write_bytes(kShader, b.code, b.size).is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[is_64bit];
    h.zero(kDesc, cl.size);
    h.write_ptr(kDesc + cl.pRootSignature, root);
    h.write_ptr(kDesc + cl.CS_pShaderBytecode, kShader);
    h.write_ptr(kDesc + cl.CS_BytecodeLength, b.size);
    pso_hr = h.com(dev, DeviceSlot::CreateComputePipelineState, {{kDesc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}});
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);
    const u32 bytes = 4096;
    const GuestAddr out = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, bytes,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr upload = h.buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 0x10000);
    const GuestAddr readback = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, bytes);
    EXPECT_EQ(h.com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h.read_ptr(kOut2);
    h.zero(mapped, 0x10000);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const float f[4] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 8 * y), 1.f};
            EXPECT_TRUE(h.memory.write_bytes(mapped + y * 256 + x * 16, reinterpret_cast<const u8*>(f), 16).is_ok());
            h.w32(mapped + 0x2000 + y * 256 + x * 4, x + 8 * y + 1000);
            h.w32(mapped + 0x6000 + y * 256 + x * 8, x + 8 * y);
            h.w32(mapped + 0x6000 + y * 256 + x * 8 + 4, 0xC0DE0000u + y);
        }
    }
    const u32 formats[4] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_UINT, 0, DXGI_FORMAT_R32G32_UINT};
    const u64 offsets[4] = {0, 0x2000, 0, 0x6000};
    GuestAddr tex[4] = {};
    const GuestAddr heap = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
    const u64 inc = h.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize, {{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}});
    const u64 cpu = h.cpu_start(heap);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[is_64bit];
    h.zero(kDesc2, ul.size);
    h.w32(kDesc2 + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h.w32(kDesc2 + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h.w32(kDesc2 + ul.Buffer_NumElements, bytes / 4);
    h.w32(kDesc2 + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h.com(dev, DeviceSlot::CreateUnorderedAccessView, {{out}, {0}, {kDesc2}, {cpu}});
    const auto& sl = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[is_64bit];
    for (u32 k = 0; k < 4; ++k) {
        if (formats[k] == 0) continue;
        tex[k] = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, formats[k], 0);
        h.zero(kDesc2, sl.size);
        h.w32(kDesc2 + sl.Format, formats[k]);
        h.w32(kDesc2 + sl.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2D);
        h.w32(kDesc2 + sl.Shader4ComponentMapping, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        h.w32(kDesc2 + sl.Texture2D_MipLevels, 1);
        (void)h.com(dev, DeviceSlot::CreateShaderResourceView, {{tex[k]}, {kDesc2}, {cpu + inc * (1 + k)}});
    }
    const GuestAddr list = h.list(dev, pso);
    const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[is_64bit];
    for (u32 k = 0; k < 4; ++k) {
        if (tex[k] == 0) continue;
        h.zero(kDesc, l.size);
        h.zero(kDesc2, l.size);
        h.write_ptr(kDesc + l.pResource, tex[k]);
        h.w32(kDesc + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        h.write_ptr(kDesc2 + l.pResource, upload);
        h.w32(kDesc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        h.w64(kDesc2 + l.PlacedFootprint_Offset, offsets[k]);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Format, formats[k]);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Width, 8);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Height, 8);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_Depth, 1);
        h.w32(kDesc2 + l.PlacedFootprint_Footprint_RowPitch, 256);
        (void)h.com(list, GraphicsCommandListSlot::CopyTextureRegion, {{kDesc}, {0}, {0}, {0}, {kDesc2}, {0}});
        h.transition(list, tex[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{out}, Harness::wide(0), {upload}, Harness::wide(0x8000), Harness::wide(bytes)});
    h.transition(list, out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.write_ptr(kArray, heap);
    (void)h.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRootSignature, {{root}});
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable, {{0}, Harness::wide(h.gpu_start(heap))});
    (void)h.com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                {{1}, Harness::wide(h.gpu_start(heap) + inc)});
    (void)h.com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
    h.transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{readback}, Harness::wide(0), {out}, Harness::wide(0), Harness::wide(bytes)});
    h.run(dev, queue, list);
    const std::vector<u8> got = h.read_back(readback, bytes);
    std::vector<std::string> lines{std::string("## ") + name};
    std::string line;
    for (u32 w = 0; w < words; ++w) {
        u32 v = 0;
        std::memcpy(&v, got.data() + w * 4, 4);
        char cell[16];
        std::snprintf(cell, sizeof(cell), "%08x", v);
        line += cell;
        if (w % 8 == 7) {
            lines.push_back(line);
            line.clear();
        } else {
            line += ' ';
        }
    }
    return lines;
}

TEST_P(D3D12DxilMetalTest, ProgrammableOffsetsMatchWarp) {
    // SM 6.7 programmable offsets (d3d12_c4.hlsl cs_offs, dxc 1.9): Load and
    // SampleLevel keep the offsets' low 4 bits as a signed value, Gather
    // (gather4_po) its 6; offsets (lane - 16, 9 - lane) reach past both
    // ranges. Point sampling and loads are exact (d3d12_a60_c4_probe.expected,
    // WARP12; RTX 4080 baseline pending).
    u32 hr = 0;
    const auto got = run_c4_compute(*h, GetParam(), "cs_offs", "offs", 256, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, probe_section("d3d12_a60_c4_probe.expected", "offs"));
}

TEST_P(D3D12DxilMetalTest, GatherRawMatchesWarp) {
    // SM 6.7 GatherRaw of an R32_UINT and of an R32G32_UINT view read as
    // uint64 (d3d12_c4.hlsl cs_raw): the four texels bit for bit, in
    // Gather's order (d3d12_a60_c4_probe.expected, WARP12).
    u32 hr = 0;
    const auto got = run_c4_compute(*h, GetParam(), "cs_raw", "raw", 192, hr);
    ASSERT_EQ(hr, S_OK);
    EXPECT_EQ(got, probe_section("d3d12_a60_c4_probe.expected", "raw"));
}

TEST_P(D3D12DxilMetalTest, StartVertexAndInstanceLocationMatchWarp) {
    // SM 6.8 SV_StartVertexLocation / SV_StartInstanceLocation (d3d12_c4.hlsl
    // vs_start): DrawInstanced(3, 2, 5, 7) gives 5 and 7 with SV_VertexID
    // from 0, DrawIndexedInstanced(3, 2, 0, 4, 9) 4 and 9 with the raw
    // indices; SV_InstanceID counts from 0. ps_start reads the three
    // vertices' records through GetAttributeAtVertex (the per-vertex capture
    // route, one capture per instance), ps_start_flat the provoking one
    // (the plain draw). d3d12_a60_c4_probe.expected (WARP12).
    for (const char* ps : {"ps_start", "ps_start_flat"}) {
        for (const bool indexed : {false, true}) {
            const std::string name = std::string("start ") + ps + (indexed ? " indexed" : " draw");
            UavDraw d;
            d.name = name.c_str();
            d.vs = "vs_start";
            d.ps = ps;
            d.vertices = 3;
            d.instances = 2;
            d.start_vertex = indexed ? 4 : 5;
            d.start_instance = indexed ? 9 : 7;
            if (indexed) d.indices = {2, 0, 1};
            d.words = 32;
            u32 hr = 0;
            const auto got = run_uav_draw(*h, GetParam(), d, hr);
            ASSERT_EQ(hr, S_OK) << name;
            EXPECT_EQ(got, probe_section("d3d12_a60_c4_probe.expected", name)) << name;
        }
    }
}

TEST_P(D3D12DxilMetalTest, AttributeAtVertexOnLinesMatchesWarp) {
    // GetAttributeAtVertex on a line list (d3d12_c4.hlsl vs_prim with
    // d3d12_bary ps_bary): vertex 0 and 1 of the line, vertex 2 reading 0,
    // the third weight 0 ("for line primitives the third barycentric weight
    // ... is guaranteed to be exactly 0.0", DXC wiki SV_Barycentrics);
    // d3d12_a60_c4_probe.expected (WARP12; RTX 4080 baseline pending). The
    // covered pixels must agree; the two weights within 2^-20.
    UavDraw d;
    d.name = "prim lines";
    d.vs = "vs_prim";
    d.ps = "ps_bary";
    d.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    d.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    d.vertices = 4;
    u32 hr = 0;
    const auto got = run_uav_draw(*h, GetParam(), d, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = probe_section("d3d12_a60_c4_probe.expected", "prim lines");
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 k = 0; k < 8; ++k) {
            const u32 g = section_word(got, i, k), w = section_word(want, i, k);
            if (k == 4 || k == 5) {
                float gf = 0, wf = 0;
                std::memcpy(&gf, &g, 4);
                std::memcpy(&wf, &w, 4);
                EXPECT_LE(std::fabs(gf - wf), 1e-6f) << "pixel " << (i - 1) << " word " << k << std::hex << " got " << g
                                                     << " want " << w;
            } else {
                EXPECT_EQ(g, w) << "pixel " << (i - 1) << " word " << k << std::hex << " got " << g << " want " << w;
            }
        }
    }
}

TEST_P(D3D12DxilMetalTest, AttributeAtVertexOnPointsMatchesWarp) {
    // GetAttributeAtVertex on a point list (vs_prim + ps_bary): each point
    // one pixel, its id at vertex 0, vertices 1 and 2 reading 0, and
    // SV_PrimitiveID; the weights of a point are left undefined (the DXC
    // wiki defines them for triangles and lines) and not compared.
    // d3d12_a60_c4_probe.expected (WARP12; RTX 4080 baseline pending).
    UavDraw d;
    d.name = "prim points";
    d.vs = "vs_prim";
    d.ps = "ps_bary";
    d.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    d.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    d.vertices = 4;
    u32 hr = 0;
    const auto got = run_uav_draw(*h, GetParam(), d, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = probe_section("d3d12_a60_c4_probe.expected", "prim points");
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 k : {0u, 1u, 2u, 3u, 7u}) {
            const u32 g = section_word(got, i, k), w = section_word(want, i, k);
            EXPECT_EQ(g, w) << "pixel " << (i - 1) << " word " << k << std::hex << " got " << g << " want " << w;
        }
    }
}

/// d3d12_a66_c6_probe.c's draws through the D3D12 frontend, built as the
/// probe builds them: a pipeline-state stream (root signature with one
/// table holding the raw UAV u1 of 4096 words, every stage, ALLOW_STREAM_
/// OUTPUT; AS / MS / VS / GS / PS; one render-target format, R8G8B8A8 when
/// @p render_target else UNKNOWN with none bound; the rasterizer state; SO
/// of ID and ATTR at a 20-byte stride; @p views view instances at location
/// (0, 0)), a size x size viewport, DrawInstanced(vertices) or
/// DispatchMesh(1, 1, 1); u1's first @p words words as "## <name>" blocks.
/// With stream output, `so` receives the probe's "<name> buffer" block.
struct C6Draw {
    const char* name{nullptr};
    const char* vs{nullptr};
    const char* gs{nullptr};
    const char* ps{nullptr};
    const char* ms{nullptr};
    const char* as{nullptr};
    const char* hs{nullptr};
    const char* ds{nullptr};
    u32 topology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    u32 topology_type{D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
    u32 vertices{3};
    u32 size{8};
    bool render_target{false};
    u32 forced{0};
    bool conservative{false};
    bool multisample_lines{false};
    bool stream_output{false};
    u32 views{0};
    u32 words{2048};
};

std::vector<std::string> word_block(const std::string& name, const u8* data, u32 words) {
    std::vector<std::string> lines{"## " + name};
    std::string line;
    for (u32 w = 0; w < words; ++w) {
        u32 v = 0;
        std::memcpy(&v, data + w * 4, 4);
        char cell[16];
        std::snprintf(cell, sizeof(cell), "%08x", v);
        line += cell;
        if (w % 8 == 7) {
            lines.push_back(line);
            line.clear();
        } else {
            line += ' ';
        }
    }
    return lines;
}

std::vector<std::string> run_c6_draw(Harness& h, bool x64, const C6Draw& d, u32& pso_hr,
                                     std::vector<std::string>* so = nullptr) {
    constexpr u32 kWords = 4096;
    constexpr u32 kSoBytes = 4096;
    const GuestAddr dev = h.device();
    const GuestAddr queue = h.queue(dev);
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootDescriptorRange uav_range;
    uav_range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.num_descriptors = 1;
    uav_range.base_shader_register = 1;
    table.ranges = {uav_range};
    rs.parameters = {table};
    rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK) << error;
    EXPECT_TRUE(h.memory.write_bytes(kBlob, blob.data(), blob.size()).is_ok());
    const GuestAddr root = h.made(h.com(dev, DeviceSlot::CreateRootSignature,
                                        {{0}, {kBlob}, {blob.size()},
                                         {h.iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}));
    // The stream (kData + 0x1B000..), its side tables and the shaders
    // (kShader + 0x3000 k).
    const auto align = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    const u64 pointer = x64 ? 8u : 4u;
    const GuestAddr stream = kData + 0x1B000;
    const GuestAddr so_decl = kData + 0x1B800;
    const GuestAddr so_names = kData + 0x1B900;
    const GuestAddr so_stride = kData + 0x1B980;
    const GuestAddr locations = kData + 0x1BA00;
    const GuestAddr stream_desc = kData + 0x1BB00;
    h.zero(stream, 0x800);
    u64 cursor = 0;
    const auto sub = [&](u32 type, u64 alignment) {
        h.w32(stream + cursor, type);
        const u64 at = align(cursor + 4, alignment);
        return stream + at;
    };
    const auto end_sub = [&](GuestAddr at, u64 bytes) {
        cursor = align(at - stream + bytes, pointer);
    };
    {
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, pointer);
        h.write_ptr(at, root);
        end_sub(at, pointer);
    }
    const auto& bc = layout::D3D12_SHADER_BYTECODE[x64];
    u32 slot = 0;
    bool shaders_ok = true;
    const auto shader = [&](u32 type, const char* name) {
        if (name == nullptr) return;
        const Sm6Blob& b = sm6_blob(name);
        if (b.code == nullptr || b.size > 0x3000) {
            ADD_FAILURE() << "shader " << name;
            shaders_ok = false;
            return;
        }
        const GuestAddr code = kShader + 0x3000 * slot++;
        EXPECT_TRUE(h.memory.write_bytes(code, b.code, b.size).is_ok());
        const GuestAddr at = sub(type, pointer);
        h.write_ptr(at + bc.pShaderBytecode, code);
        h.write_ptr(at + bc.BytecodeLength, b.size);
        end_sub(at, bc.size);
    };
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, d.as);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, d.ms);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, d.vs);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, d.hs);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, d.ds);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, d.gs);
    shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, d.ps);
    if (!shaders_ok) return {};
    {
        const auto& rt = layout::D3D12_RT_FORMAT_ARRAY[x64];
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, 4);
        h.w32(at + rt.NumRenderTargets, 1);
        h.w32(at + rt.RTFormats, d.render_target ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN);
        end_sub(at, rt.size);
    }
    if (d.ms == nullptr) {
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, 4);
        h.w32(at, d.topology_type);
        end_sub(at, 4);
    }
    {
        const auto& r = layout::D3D12_RASTERIZER_DESC[x64];
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, 4);
        h.w32(at + r.FillMode, D3D12_FILL_MODE_SOLID);
        h.w32(at + r.CullMode, D3D12_CULL_MODE_NONE);
        h.w32(at + r.DepthClipEnable, 1);
        h.w32(at + r.MultisampleEnable, d.multisample_lines ? 1u : 0u);
        h.w32(at + r.ForcedSampleCount, d.forced);
        h.w32(at + r.ConservativeRaster, d.conservative ? 1u : 0u);
        end_sub(at, r.size);
    }
    {
        const auto& ds = layout::D3D12_DEPTH_STENCIL_DESC1[x64];
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, 4);
        h.w32(at + ds.DepthFunc, D3D12_COMPARISON_FUNC_ALWAYS);
        end_sub(at, ds.size);
    }
    {
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, 4);
        h.w32(at, 1);
        end_sub(at, 8);
        const GuestAddr mask = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, 4);
        h.w32(mask, 0xFFFFFFFFu);
        end_sub(mask, 4);
    }
    if (d.stream_output) {
        const auto& e = layout::D3D12_SO_DECLARATION_ENTRY[x64];
        EXPECT_TRUE(h.memory.write_bytes(so_names, reinterpret_cast<const u8*>("ID\0ATTR\0"), 8).is_ok());
        h.zero(so_decl, e.size * 2);
        h.write_ptr(so_decl + e.SemanticName, so_names);
        h.w32(so_decl + e.ComponentCount, 1);
        h.write_ptr(so_decl + e.size + e.SemanticName, so_names + 3);
        h.w32(so_decl + e.size + e.ComponentCount, 4);
        h.w32(so_stride, 20);
        const auto& sd = layout::D3D12_STREAM_OUTPUT_DESC[x64];
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, pointer);
        h.write_ptr(at + sd.pSODeclaration, so_decl);
        h.w32(at + sd.NumEntries, 2);
        h.write_ptr(at + sd.pBufferStrides, so_stride);
        h.w32(at + sd.NumStrides, 1);
        h.w32(at + sd.RasterizedStream, 0);
        end_sub(at, sd.size);
    }
    if (d.views != 0) {
        h.zero(locations, 32);
        const auto& vi = layout::D3D12_VIEW_INSTANCING_DESC[x64];
        const GuestAddr at = sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, pointer);
        h.w32(at + vi.ViewInstanceCount, d.views);
        h.write_ptr(at + vi.pViewInstanceLocations, locations);
        h.w32(at + vi.Flags, 0);
        end_sub(at, vi.size);
    }
    const auto& sdl = layout::D3D12_PIPELINE_STATE_STREAM_DESC[x64];
    h.w64(stream_desc + sdl.SizeInBytes, cursor);
    h.write_ptr(stream_desc + sdl.pPipelineStateSubobjectStream, stream);
    pso_hr = static_cast<u32>(h.com(dev, DeviceSlot::CreatePipelineState,
                                    {{stream_desc}, {h.iid(IID_ID3D12PIPELINESTATE)}, {kOut}}));
    if (pso_hr != S_OK) return {};
    const GuestAddr pso = h.read_ptr(kOut);

    const u32 bytes = kWords * 4;
    const GuestAddr out = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, bytes,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const GuestAddr upload = h.buffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, bytes);
    const GuestAddr readback = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, bytes);
    const GuestAddr so_buffer = h.buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, kSoBytes);
    const GuestAddr so_readback = h.buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, kSoBytes);
    EXPECT_EQ(h.com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    h.zero(h.read_ptr(kOut2), bytes);
    const GuestAddr heap = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[x64];
    h.zero(kDesc2, ul.size);
    h.w32(kDesc2 + ul.Format, DXGI_FORMAT_R32_TYPELESS);
    h.w32(kDesc2 + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
    h.w32(kDesc2 + ul.Buffer_NumElements, kWords);
    h.w32(kDesc2 + ul.Buffer_Flags, D3D12_BUFFER_UAV_FLAG_RAW);
    (void)h.com(dev, DeviceSlot::CreateUnorderedAccessView, {{out}, {0}, {kDesc2}, {h.cpu_start(heap)}});
    constexpr GuestAddr kView = kData + 0x1A500;
    constexpr GuestAddr kScissor = kData + 0x1A540;
    constexpr GuestAddr kRtvSlot = kData + 0x1A5C0;
    constexpr GuestAddr kSoView = kData + 0x1A600;
    const GuestAddr list = h.list(dev, pso);
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{out}, Harness::wide(0), {upload}, Harness::wide(0), Harness::wide(bytes)});
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{so_buffer}, Harness::wide(0), {upload}, Harness::wide(0), Harness::wide(kSoBytes)});
    h.transition(list, out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.transition(list, so_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    h.write_ptr(kArray, heap);
    (void)h.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, {{1}, {kArray}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable,
                {{0}, Harness::wide(h.gpu_start(heap))});
    if (d.render_target) {
        const GuestAddr color = h.resource(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const GuestAddr rtvs = h.heap(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        const u64 rtv = h.cpu_start(rtvs);
        (void)h.com(dev, DeviceSlot::CreateRenderTargetView, {{color}, {0}, {rtv}});
        h.write_ptr(kRtvSlot, rtv);
        (void)h.com(list, GraphicsCommandListSlot::OMSetRenderTargets, {{1}, {kRtvSlot}, {0}, {0}});
    } else {
        (void)h.com(list, GraphicsCommandListSlot::OMSetRenderTargets, {{0}, {0}, {0}, {0}});
    }
    const auto& vp = layout::D3D12_VIEWPORT[x64];
    const auto put_f = [&](GuestAddr at, float value) {
        u32 b = 0;
        std::memcpy(&b, &value, 4);
        h.w32(at, b);
    };
    put_f(kView + vp.TopLeftX, 0.f);
    put_f(kView + vp.TopLeftY, 0.f);
    put_f(kView + vp.Width, static_cast<float>(d.size));
    put_f(kView + vp.Height, static_cast<float>(d.size));
    put_f(kView + vp.MinDepth, 0.f);
    put_f(kView + vp.MaxDepth, 1.f);
    const auto& rect = layout::D3D12_RECT[x64];
    h.w32(kScissor + rect.left, 0);
    h.w32(kScissor + rect.top, 0);
    h.w32(kScissor + rect.right, d.size);
    h.w32(kScissor + rect.bottom, d.size);
    (void)h.com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {kView}});
    (void)h.com(list, GraphicsCommandListSlot::RSSetScissorRects, {{1}, {kScissor}});
    if (d.stream_output) {
        const auto& sv = layout::D3D12_STREAM_OUTPUT_BUFFER_VIEW[x64];
        h.zero(kSoView, sv.size);
        h.w64(kSoView + sv.BufferLocation, h.gpu_va(so_buffer));
        h.w64(kSoView + sv.SizeInBytes, kSoBytes - 256);
        h.w64(kSoView + sv.BufferFilledSizeLocation, h.gpu_va(so_buffer) + kSoBytes - 16);
        (void)h.com(list, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {kSoView}});
    }
    if (d.ms != nullptr) {
        (void)h.com(list, GraphicsCommandListSlot::DispatchMesh, {{1}, {1}, {1}});
    } else {
        (void)h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology, {{d.topology}});
        (void)h.com(list, GraphicsCommandListSlot::DrawInstanced, {{d.vertices}, {1}, {0}, {0}});
    }
    h.transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    h.transition(list, so_buffer, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{readback}, Harness::wide(0), {out}, Harness::wide(0), Harness::wide(bytes)});
    (void)h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                {{so_readback}, Harness::wide(0), {so_buffer}, Harness::wide(0), Harness::wide(kSoBytes)});
    h.run(dev, queue, list);
    const std::vector<u8> words = h.read_back(readback, bytes);
    if (so != nullptr) {
        const std::vector<u8> s = h.read_back(so_readback, kSoBytes);
        std::array<u8, 128> block{};
        std::memcpy(block.data(), s.data(), 30 * 4);
        std::memcpy(block.data() + 30 * 4, s.data() + kSoBytes - 16, 4);
        *so = word_block(std::string(d.name) + " buffer", block.data(), 32);
    }
    return word_block(d.name, words.data(), d.words);
}

/// Words of a "## name" block as floats/ints, for tolerance compares.
float word_float(const std::vector<std::string>& lines, usize i, u32 k) {
    const u32 w = section_word(lines, i, k);
    float f = 0;
    std::memcpy(&f, &w, 4);
    return f;
}

/// d3d12_c6's ps_c6 records ((x + 8 y) * 4 + slot, 8 words): ids and the
/// GetAttributeAtVertex words exact (vertex 2 of a line is undefined and
/// skipped there), the three weights within @p tolerance. A record whose
/// WARP weights do not sum to 1 (WARP evaluates a TIR edge pixel's
/// SV_Barycentrics off the pixel centre) must have its own sum to 1: D3D11.3
/// 3.5.5, attributes at the pixel centre.
void expect_c6_records(const std::vector<std::string>& got, const std::vector<std::string>& want, bool lines,
                       float tolerance) {
    ASSERT_EQ(got.size(), want.size()) << got[0];
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 k = 0; k < 8; ++k) {
            const u32 g = section_word(got, i, k), w = section_word(want, i, k);
            if (k == 3 && lines) continue;
            if (k == 7 && lines) {
                EXPECT_EQ(g & 0xFFFFu, w & 0xFFFFu) << got[0] << " record " << (i - 1) << std::hex << " got " << g
                                                     << " want " << w;
                continue;
            }
            if (k < 4 || k == 7) {
                EXPECT_EQ(g, w) << got[0] << " record " << (i - 1) << " word " << k << std::hex << " got " << g
                                << " want " << w;
            }
        }
        const float wsum = word_float(want, i, 4) + word_float(want, i, 5) + word_float(want, i, 6);
        const bool off_centre = section_word(want, i, 0) != 0 && std::fabs(wsum - 1.0f) > 1e-3f;
        if (off_centre) {
            const float gsum = word_float(got, i, 4) + word_float(got, i, 5) + word_float(got, i, 6);
            EXPECT_NEAR(gsum, 1.0f, 1e-6f) << got[0] << " record " << (i - 1);
            continue;
        }
        for (u32 k = 4; k < 7; ++k) {
            EXPECT_LE(std::fabs(word_float(got, i, k) - word_float(want, i, k)), tolerance)
                << got[0] << " record " << (i - 1) << " word " << k << std::hex << " got " << section_word(got, i, k)
                << " want " << section_word(want, i, k);
        }
    }
}

TEST_P(D3D12DxilMetalTest, AttributeAtVertexInEveryPipelineShape) {
    // GetAttributeAtVertex and SV_Barycentrics (d3d12_c6 ps_c6) with
    // conservative rasterization, target-independent rasterization (4x,
    // UAV-only), vertex-shader stream output, geometry-shader lines and
    // behind a mesh shader (ms_c6: triangle 1 indexes 3, 2, 1, so vertex k is
    // the k-th index). d3d12_a66_c6_probe.expected (WARP12; RTX 4080 baseline
    // pending). Conservative rasterization extrapolates the weights past the
    // triangle (the spec allows it).
    struct Case {
        const char* name;
        const char* vs;
        const char* gs;
        const char* ms;
        const char* hs;
        const char* ds;
        bool rt;
        u32 forced;
        bool cr;
        bool so;
        bool lines;
    };
    const Case cases[] = {
        {"bary cr", "vs_c6", nullptr, nullptr, nullptr, nullptr, true, 0, true, false, false},
        {"bary tir", "vs_c6", nullptr, nullptr, nullptr, nullptr, false, 4, false, false, false},
        {"bary so", "vs_c6", nullptr, nullptr, nullptr, nullptr, true, 0, false, true, false},
        {"bary gs lines", "vs_c6", "gs_c6_lines", nullptr, nullptr, nullptr, true, 0, false, false, true},
        {"bary tir gs lines", "vs_c6", "gs_c6_lines", nullptr, nullptr, nullptr, false, 4, false, false, true},
        {"bary ms", nullptr, nullptr, "ms_c6", nullptr, nullptr, true, 0, false, false, false},
        {"bary ds", "vs_c6", nullptr, nullptr, "hs_c6", "ds_c6", true, 0, false, false, false},
        {"bary tir ds", "vs_c6", nullptr, nullptr, "hs_c6", "ds_c6", false, 4, false, false, false},
    };
    for (const Case& c : cases) {
        C6Draw d;
        d.name = c.name;
        d.vs = c.vs;
        d.gs = c.gs;
        d.ms = c.ms;
        d.hs = c.hs;
        d.ds = c.ds;
        if (c.ds != nullptr) {
            d.topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            d.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        }
        d.ps = "ps_c6";
        d.vertices = 6;
        d.render_target = c.rt;
        d.forced = c.forced;
        d.conservative = c.cr;
        d.stream_output = c.so;
        u32 hr = 0;
        std::vector<std::string> so;
        const auto got = run_c6_draw(*h, GetParam(), d, hr, c.so ? &so : nullptr);
        ASSERT_EQ(hr, S_OK) << c.name;
        // A TIR line's weights come from its snapped endpoints (1e-3).
        expect_c6_records(got, probe_section("d3d12_a66_c6_probe.expected", c.name), c.lines,
                          c.lines && c.forced != 0 ? 1e-3f : 6e-8f);
        if (c.so) {
            // The six vertices' ID and ATTR, and BufferFilledSize 120.
            auto want = probe_section("d3d12_a66_c6_probe.expected", std::string(c.name) + " buffer");
            want.erase(std::remove_if(want.begin(), want.end(),
                                      [](const std::string& l) { return l.rfind("##", 0) == 0; }),
                       want.end());
            ASSERT_FALSE(want.empty());
            ASSERT_FALSE(so.empty());
            EXPECT_EQ(std::vector<std::string>(so.begin() + 1, so.end()), want);
        }
    }
}

TEST_P(D3D12DxilMetalTest, BarycentricsOfDomainShaderIsolines) {
    // Isolines from hs_c6_iso / ds_c6_iso (each 2-point patch k one line of
    // vs_c6 vertices 2k, 2k + 1): SV_Barycentrics' third weight is "exactly
    // 0.0" for lines (DXC wiki SV_Barycentrics), the first two sum to 1, and
    // GetAttributeAtVertex 0 / 1 name the line's vertices. Which pixels an
    // isoline covers is not compared: WARP12's x64 and x86 baselines
    // (d3d12_a66_c6_probe "bary ds isolines") disagree with each other.
  for (const u32 forced : {0u, 4u}) {
    C6Draw d;
    d.name = forced ? "bary tir ds isolines" : "bary ds isolines";
    d.forced = forced;
    d.vs = "vs_c6";
    d.hs = "hs_c6_iso";
    d.ds = "ds_c6_iso";
    d.ps = "ps_c6";
    d.render_target = forced == 0;
    d.topology = D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST;
    d.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    d.vertices = 6;
    u32 hr = 0;
    const auto got = run_c6_draw(*h, GetParam(), d, hr);
    ASSERT_EQ(hr, S_OK);
    u32 covered = 0;
    for (usize i = 1; i < got.size(); ++i) {
        if (section_word(got, i, 0) == 0) continue;
        ++covered;
        const u32 line = (section_word(got, i, 7) & 0xFFu);
        EXPECT_EQ(section_word(got, i, 0), 100 + 2 * line) << "record " << (i - 1);
        EXPECT_EQ(section_word(got, i, 1), 100 + 2 * line) << "record " << (i - 1);
        EXPECT_EQ(section_word(got, i, 2), 101 + 2 * line) << "record " << (i - 1);
        EXPECT_EQ(section_word(got, i, 6), 0u) << "record " << (i - 1);
        EXPECT_NEAR(word_float(got, i, 4) + word_float(got, i, 5), 1.0f, 1e-6f) << "record " << (i - 1);
    }
    EXPECT_GT(covered, 8u);
  }
}

TEST_P(D3D12DxilMetalTest, ViewInstancingTier3RunsStagesBeforeViewIdOnce) {
    // d3d12_a66_c6_probe "vi *" (d3d12_c6.hlsl counters in u1: 0 VS, 1-3 VS
    // per view, 8 GS (8-10 per view), 12 HS control points, 16 PS (16-18 per
    // view), 20 DS (20-22 per view), 24 MS (25-27 per view), 30 AS), three
    // views at (0, 0). ViewInstancingTier 3 (DirectX-Specs "View
    // Instancing"): "The implementation of view instancing always occurs at
    // the first shader stage that references SV_ViewID or the rasterizer,
    // whichever is earlier". Expected values: the RTX 4080 SUPER (tier 3,
    // d3d12_a66_c6_probe.winoracle-gpu.expected), except where the spec
    // leaves the first SV_ViewID stage free ("Redundant shading is limited
    // to just the first SV_ViewID dependent shader stage's code, if not
    // avoided completely": Vitreo runs that stage per view, the RTX runs its
    // view-blind part once -- "vs-id" word 0, "ms-id" word 24, and the
    // view-blind GS after "vs-id", logically instanced) and where the RTX
    // contradicts it ("as ms ps-id": the RTX's pixel shader reads SV_ViewID 0
    // in every view; tier 3 is "functionally the same as Tier 1", so each
    // view reads its own).
    struct Case {
        const char* name;
        const char* vs;
        const char* gs;
        const char* ps;
        const char* ms;
        const char* as;
        const char* ds;
        std::vector<std::pair<u32, u32>> words;  // the non-zero counters
    };
    const std::vector<Case> cases = {
        {"vi vs ps-id", "vs_vi", nullptr, "ps_vi_id", nullptr, nullptr, nullptr,
         {{0, 3}, {16, 0x40}, {17, 0x40}, {18, 0x40}}},
        {"vi vs-id ps", "vs_vi_id", nullptr, "ps_vi", nullptr, nullptr, nullptr,
         {{0, 9}, {1, 3}, {2, 3}, {3, 3}, {16, 0xc0}}},
        {"vi vs gs-id ps", "vs_vi", "gs_vi_id", "ps_vi", nullptr, nullptr, nullptr,
         {{0, 3}, {8, 1}, {9, 1}, {10, 1}, {16, 0xc0}}},
        {"vi vs gs ps-id", "vs_vi", "gs_vi", "ps_vi_id", nullptr, nullptr, nullptr,
         {{0, 3}, {8, 1}, {16, 0x40}, {17, 0x40}, {18, 0x40}}},
        {"vi vs-id gs ps", "vs_vi_id", "gs_vi", "ps_vi", nullptr, nullptr, nullptr,
         {{0, 9}, {1, 3}, {2, 3}, {3, 3}, {8, 3}, {16, 0xc0}}},
        {"vi ms ps-id", nullptr, nullptr, "ps_vi_id", "ms_vi", nullptr, nullptr,
         {{16, 0x40}, {17, 0x40}, {18, 0x40}, {24, 1}}},
        {"vi ms-id ps", nullptr, nullptr, "ps_vi", "ms_vi_id", nullptr, nullptr,
         {{16, 0xc0}, {24, 3}, {25, 1}, {26, 1}, {27, 1}}},
        {"vi as ms ps-id", nullptr, nullptr, "ps_vi_id", "ms_vi_ap", "as_vi", nullptr,
         {{16, 0x40}, {17, 0x40}, {18, 0x40}, {24, 1}, {30, 1}}},
        {"vi as ms-id ps", nullptr, nullptr, "ps_vi", "ms_vi_id_ap", "as_vi", nullptr,
         {{16, 0xc0}, {24, 3}, {25, 1}, {26, 1}, {27, 1}, {30, 1}}},
        {"vi vs hs ds-id ps", "vs_vi", nullptr, "ps_vi", nullptr, nullptr, "ds_vi_id",
         {{0, 3}, {12, 3}, {16, 0xc0}, {20, 3}, {21, 3}, {22, 3}}},
        {"vi vs hs ds ps-id", "vs_vi", nullptr, "ps_vi_id", nullptr, nullptr, "ds_vi",
         {{0, 3}, {12, 3}, {16, 0x40}, {17, 0x40}, {18, 0x40}, {20, 3}}},
        {"vi vs hs ds gs-id ps", "vs_vi", "gs_vi_id", "ps_vi", nullptr, nullptr, "ds_vi",
         {{0, 3}, {8, 1}, {9, 1}, {10, 1}, {12, 3}, {16, 0xc0}, {20, 3}}},
    };
    for (const Case& c : cases) {
        C6Draw d;
        d.name = c.name;
        d.vs = c.vs;
        d.gs = c.gs;
        d.ps = c.ps;
        d.ms = c.ms;
        d.as = c.as;
        d.ds = c.ds;
        d.hs = c.ds != nullptr ? "hs_vi" : nullptr;
        if (c.ds != nullptr) {
            d.topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            d.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        }
        d.views = 3;
        d.words = 32;
        u32 hr = 0;
        const auto got = run_c6_draw(*h, GetParam(), d, hr);
        ASSERT_EQ(hr, S_OK) << c.name;
        ASSERT_EQ(got.size(), 5u) << c.name;
        for (u32 w = 0; w < 32; ++w) {
            u32 want = 0;
            for (const auto& [word, value] : c.words) {
                if (word == w) want = value;
            }
            EXPECT_EQ(section_word(got, 1 + w / 8, w % 8), want) << c.name << " word " << w;
        }
    }
}

TEST_P(D3D12DxilMetalTest, RasterEmulatedLinesAndPointsWithoutPixelShader) {
    // d3d12_a66_c6_probe "crnops *" (WARP12): conservative or target-
    // independent rasterization of lines and points with no pixel shader is
    // accepted (with a pixel shader the conservative pipeline is refused) and
    // draws nothing a UAV-only pass can see.
    struct Case {
        const char* name;
        bool points;
        u32 forced;
        bool cr;
    };
    const Case cases[] = {
        {"crnops cr lines", false, 0, true},   {"crnops cr points", true, 0, true},
        {"crnops tir4 lines", false, 4, false}, {"crnops tir4 points", true, 4, false},
        {"crnops tir1 lines", false, 1, false}, {"crnops tir16 points", true, 16, false},
    };
    for (const Case& c : cases) {
        C6Draw d;
        d.name = c.name;
        d.vs = c.points ? "vs_points" : "vs_lines";
        d.topology = c.points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        d.topology_type = c.points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        d.vertices = c.points ? 6 : 10;
        d.size = 16;
        d.forced = c.forced;
        d.conservative = c.cr;
        d.words = 8;
        u32 hr = 0;
        const auto got = run_c6_draw(*h, GetParam(), d, hr);
        ASSERT_EQ(hr, S_OK) << c.name;
        // The block only (its statistics lines follow it in the file).
        auto want = probe_section("d3d12_a66_c6_probe.expected", c.name);
        if (want.size() > got.size()) want.resize(got.size());
        EXPECT_EQ(got, want) << c.name;
    }
    C6Draw with_ps;
    with_ps.name = "crnops cr lines ps";
    with_ps.vs = "vs_lines";
    with_ps.ps = "ps_cov";
    with_ps.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    with_ps.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    with_ps.vertices = 10;
    with_ps.size = 16;
    with_ps.conservative = true;
    u32 hr = 0;
    (void)run_c6_draw(*h, GetParam(), with_ps, hr);
    EXPECT_EQ(hr, E_INVALIDARG);
}

TEST_P(D3D12DxilMetalTest, AdjacencyWithoutGeometryShaderMatchesWarp) {
    // d3d12_a66_c6_probe "adj *" (WARP12): with no geometry shader an
    // adjacency topology draws its lines (vertices 1, 2 of each four) and
    // triangles (0, 2, 4 of each six) without the adjacent vertices, every
    // vertex still running the vertex shader (word 1023 counts 12), the
    // provoking vertex and SV_PrimitiveID as WARP's.
    struct Case {
        const char* name;
        u32 topology;
        u32 type;
    };
    const Case cases[] = {
        {"adj trilist", D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
        {"adj tristrip", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
        {"adj linelist", D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
        {"adj linestrip", D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
    };
    for (const Case& c : cases) {
        C6Draw d;
        d.name = c.name;
        d.vs = "vs_adj";
        d.ps = "ps_adj";
        d.topology = c.topology;
        d.topology_type = c.type;
        d.vertices = 12;
        d.size = 16;
        d.words = 1024;
        u32 hr = 0;
        const auto got = run_c6_draw(*h, GetParam(), d, hr);
        ASSERT_EQ(hr, S_OK) << c.name;
        // The block only (its statistics lines follow it in the file).
        auto want = probe_section("d3d12_a66_c6_probe.expected", c.name);
        if (want.size() > got.size()) want.resize(got.size());
        ASSERT_EQ(got.size(), want.size()) << c.name;
        // Per pixel: the hits; the provoking VID and SV_PrimitiveID where one
        // primitive covers it (where two do, the last unordered UAV store
        // wins); and vs_adj's count.
        for (u32 pixel = 0; pixel < 256; ++pixel) {
            const u32 word = pixel * 4;
            const auto at = [&](const std::vector<std::string>& b, u32 w) {
                return section_word(b, 1 + w / 8, w % 8);
            };
            EXPECT_EQ(at(got, word), at(want, word)) << c.name << " pixel " << pixel;
            if (at(want, word) == 1) {
                EXPECT_EQ(at(got, word + 1), at(want, word + 1)) << c.name << " pixel " << pixel;
                EXPECT_EQ(at(got, word + 2), at(want, word + 2)) << c.name << " pixel " << pixel;
            }
        }
        EXPECT_EQ(section_word(got, 128, 7), 12u) << c.name;
        EXPECT_EQ(section_word(want, 128, 7), 12u) << c.name;
    }
}

TEST_P(D3D12DxilMetalTest, MeshPixelRowFedByVertexAndPrimitiveOutputs) {
    // d3d12_a66_c6_probe "mix": ps_mix packs TEXCOORD0 (a mesh vertex
    // attribute) and TEXCOORD1 (a primitive attribute) into v1.xy / v1.zw.
    // Linkage is by semantic (DirectX-Specs "Mesh Shader"), so the pipeline
    // is valid (WARP12: S_OK). The interpolated vertex attribute matches
    // WARP's; the primitive attribute is the primitive's (5, 7) at every
    // pixel ("Primitive attributes do not require any interpolation
    // modifiers to be specified, nor do they have any effect") -- WARP12
    // returns a varying value there, which the spec does not allow.
    C6Draw d;
    d.name = "mix";
    d.ms = "ms_mix";
    d.ps = "ps_mix";
    d.words = 256;
    u32 hr = 0;
    const auto got = run_c6_draw(*h, GetParam(), d, hr);
    ASSERT_EQ(hr, S_OK);
    const auto warp = probe_section("d3d12_a66_c6_probe.expected", "mix");
    ASSERT_EQ(got.size(), warp.size());
    for (usize i = 1; i < got.size(); ++i) {
        for (u32 k = 0; k < 8; ++k) {
            if ((k & 3u) < 2u) {
                // Interpolation: within a few ulps of WARP's.
                EXPECT_NEAR(word_float(got, i, k), word_float(warp, i, k), 1e-6f)
                    << "line " << i << " word " << k;
            } else {
                EXPECT_EQ(word_float(got, i, k), (k & 3u) == 2u ? 5.0f : 7.0f) << "line " << i << " word " << k;
            }
        }
    }
}

/// One lp draw of d3d12_a66_c6_probe (16x16, UAV-only): vs_lines (5 lines,
/// or 4 for ps_lines_bary) or vs_points (6 points).
std::vector<std::string> run_lp_draw(Harness& h, bool x64, const char* name, bool points, const char* ps,
                                     u32 forced, bool quad, u32& hr) {
    C6Draw d;
    d.name = name;
    d.vs = points ? "vs_points" : "vs_lines";
    d.ps = ps;
    d.topology = points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    d.topology_type = points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    d.vertices = points ? 6 : (std::string(ps) == "ps_lines_bary" ? 8 : 10);
    d.size = 16;
    d.forced = forced;
    d.multisample_lines = quad;
    d.words = std::string(ps) == "ps_lines_bary" ? 4096 : 1024;
    return run_c6_draw(h, x64, d, hr);
}

/// Word @p w of 4-word record @p r of a block (8 words a line).
u32 record4(const std::vector<std::string>& lines, u32 r, u32 w) {
    const u32 index = r * 4 + w;
    return section_word(lines, 1 + index / 8, index % 8);
}

float record4f(const std::vector<std::string>& lines, u32 r, u32 w) {
    const u32 v = record4(lines, r, w);
    float f = 0;
    std::memcpy(&f, &v, 4);
    return f;
}

TEST_P(D3D12DxilMetalTest, AliasedLinesUnderTargetIndependentRasterization) {
    // ForcedSampleCount 4, aliased lines (MultisampleEnable false): the
    // diamond-exit rule (D3D11.3 3.4.3), every sample of a covered pixel
    // ("all of the MSAA samples in the pixel are hit", 15.14.1), one
    // invocation, attributes along the major axis -- as WARP12
    // (d3d12_a66_c6_probe "lines tir4"; RTX 4080 baseline pending) and
    // Metal's own lines ("lines tir1"). One pixel differs: WARP also draws
    // (13, 5), where line 0 ends inside the pixel's diamond, which the
    // exit rule excludes (Vitreo's native lines exclude it too).
    u32 hr = 0;
    const auto got = run_lp_draw(*h, GetParam(), "lines tir4", false, "ps_cov", 4, false, hr);
    ASSERT_EQ(hr, S_OK);
    const auto want = probe_section("d3d12_a66_c6_probe.expected", "lines tir4");
    ASSERT_EQ(got.size(), want.size());
    const auto& want_block = want;
    for (u32 r = 0; r < 256; ++r) {
        if (r == 93) {
            EXPECT_EQ(record4(got, r, 0), 0u) << "pixel (13, 5): B inside the diamond";
            continue;
        }
        for (u32 w = 0; w < 3; ++w) {
            EXPECT_EQ(record4(got, r, w), record4(want_block, r, w)) << "pixel " << r << " word " << w;
        }
        EXPECT_NEAR(record4f(got, r, 3), record4f(want_block, r, 3), 1e-4f) << "pixel " << r;
    }
}

TEST_P(D3D12DxilMetalTest, QuadrilateralLinesUnderTargetIndependentRasterization) {
    // MultisampleEnable lines (QUADRILATERAL_WIDE, 1.4 wide, 3.4.5) at
    // ForcedSampleCount 4 and 8: each sample's coverage of the rectangle
    // and the lines covering each pixel exactly as WARP12 (d3d12_a66_c6_probe
    // "lines tir4 quad" / "tir8 quad"), attributes "constant across the
    // perpendicular" within 1e-3. WARP shades a pixel on the rectangle's
    // diagonal once per triangle of it; Vitreo once per line (the union of
    // the coverage), so word 2 counts the lines.
    for (const u32 forced : {4u, 8u}) {
        const std::string name = forced == 4 ? "lines tir4 quad" : "lines tir8 quad";
        u32 hr = 0;
        const auto got = run_lp_draw(*h, GetParam(), name.c_str(), false, "ps_cov", forced, true, hr);
        ASSERT_EQ(hr, S_OK) << name;
        const auto want = probe_section("d3d12_a66_c6_probe.expected", name);
        ASSERT_EQ(got.size(), want.size()) << name;
        for (u32 r = 0; r < 256; ++r) {
            EXPECT_EQ(record4(got, r, 0), record4(want, r, 0)) << name << " pixel " << r;
            EXPECT_EQ(record4(got, r, 1), record4(want, r, 1)) << name << " pixel " << r;
            EXPECT_EQ(record4(got, r, 2), static_cast<u32>(std::popcount(record4(got, r, 1))))
                << name << " pixel " << r;
            EXPECT_NEAR(record4f(got, r, 3), record4f(want, r, 3), 1e-3f) << name << " pixel " << r;
        }
    }
}

TEST_P(D3D12DxilMetalTest, PointsUnderTargetIndependentRasterization) {
    // A point is "a square of width 1 oriented to the RenderTarget" whose
    // "coverage ... follows Triangle Rasterization Rules" (D3D11.3 3.4.6),
    // "a coverage test ... for each sample location" (Learn, Rasterization
    // Rules, multisampling): the expected coverage is computed here from the
    // 4x standard pattern (kStandardPattern4, measured), the snapped centre
    // and the top-left rule. WARP12 (d3d12_a66_c6_probe "points tir4")
    // instead covers the pixel holding the centre with every sample, and
    // agrees at ForcedSampleCount 1 ("points tir1", compared exactly).
    // RTX 4080 baseline pending.
    static constexpr float kPoint[6][2] = {{2.5f, 2.5f},   {5.0f, 5.0f},   {7.25f, 3.75f},
                                           {10.5f, 12.0f}, {13.0f, 13.5f}, {0.1f, 15.9f}};
    static constexpr int kPattern[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
    std::array<u32, 256> coverage{};
    std::array<u32, 256> ids{};
    for (u32 k = 0; k < 6; ++k) {
        // The centre at 16.8 (round to nearest).
        const double cx = std::nearbyint(static_cast<double>(kPoint[k][0]) * 256) / 256;
        const double cy = std::nearbyint(static_cast<double>(kPoint[k][1]) * 256) / 256;
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                for (u32 s = 0; s < 4; ++s) {
                    const double sx = x + 0.5 + kPattern[s][0] / 16.0;
                    const double sy = y + 0.5 + kPattern[s][1] / 16.0;
                    // Top and left edges in, right and bottom out.
                    if (sx >= cx - 0.5 && sx < cx + 0.5 && sy >= cy - 0.5 && sy < cy + 0.5) {
                        coverage[x + 16 * y] |= 1u << s;
                        ids[x + 16 * y] |= 1u << k;
                    }
                }
            }
        }
    }
    u32 hr = 0;
    const auto got = run_lp_draw(*h, GetParam(), "points tir4", true, "ps_cov", 4, false, hr);
    ASSERT_EQ(hr, S_OK);
    ASSERT_EQ(got.size(), 129u);
    for (u32 r = 0; r < 256; ++r) {
        EXPECT_EQ(record4(got, r, 0), coverage[r]) << "pixel " << r;
        EXPECT_EQ(record4(got, r, 1), ids[r]) << "pixel " << r;
        EXPECT_EQ(record4(got, r, 2), coverage[r] != 0 ? 1u : 0u) << "pixel " << r;
        EXPECT_EQ(record4(got, r, 3), coverage[r] != 0 ? 0x3e800000u : 0u) << "pixel " << r;
    }
    const auto one = run_lp_draw(*h, GetParam(), "points tir1", true, "ps_cov", 1, false, hr);
    ASSERT_EQ(hr, S_OK);
    auto want = probe_section("d3d12_a66_c6_probe.expected", "points tir1");
    EXPECT_EQ(one, want);
}

TEST_P(D3D12DxilMetalTest, AttributeAtVertexOnTargetIndependentLines) {
    // GetAttributeAtVertex and SV_Barycentrics.z of 4 aliased lines at
    // ForcedSampleCount 4 (ps_lines_bary): vertex 0 / 1 the line's, vertex 2
    // 0 (| 0x100), the third weight exactly 0; WARP12 equal but for pixel
    // (13, 5) of line 0 (see AliasedLinesUnderTargetIndependentRasterization).
    u32 hr = 0;
    const auto got = run_lp_draw(*h, GetParam(), "lines bary tir4", false, "ps_lines_bary", 4, false, hr);
    ASSERT_EQ(hr, S_OK);
    auto want = probe_section("d3d12_a66_c6_probe.expected", "lines bary tir4");
    ASSERT_EQ(got.size(), want.size());
    for (u32 r = 0; r < 1024; ++r) {
        if (r == 93 * 4) continue;
        for (u32 w = 0; w < 4; ++w) {
            EXPECT_EQ(record4(got, r, w), record4(want, r, w)) << "record " << r << " word " << w;
        }
    }
}

TEST_P(D3D12DxilMetalTest, RasterEmulationOfThirtyInputRegisters) {
    // ps_many30 reads v1..v30 (120 components): with the 7 edge slots it
    // exceeds Metal's 124 fragment input components, so the edge data comes
    // from the edge buffer. Conservative: exact as WARP12
    // (d3d12_a66_c6_probe "many30 cr"); TIR 4x: the attributes exact, and
    // every pixel of the screen-covering triangle fully covered (WARP12
    // splits it at the clip into two, shading the diagonal's pixels twice).
    // ps_many (v1..v31 and the UAV gate's [[sample_id]]: 125 components)
    // fails loudly, as the plain pipeline does in Metal.
    for (const bool cr : {true, false}) {
        const std::string name = cr ? "many30 cr" : "many30 tir4";
        C6Draw d;
        d.name = name.c_str();
        d.vs = "vs_many";
        d.ps = "ps_many30";
        d.render_target = cr;
        d.conservative = cr;
        d.forced = cr ? 0 : 4;
        d.words = 512;
        u32 hr = 0;
        const auto got = run_c6_draw(*h, GetParam(), d, hr);
        ASSERT_EQ(hr, S_OK) << name;
        auto want = probe_section("d3d12_a66_c6_probe.expected", name);
        ASSERT_EQ(got.size(), want.size()) << name;
        for (usize i = 1; i < got.size(); ++i) {
            for (u32 k = 0; k < 8; ++k) {
                const u32 g = section_word(got, i, k), w = section_word(want, i, k);
                if (!cr && k == 7) {
                    EXPECT_EQ(g, w == 0 ? 0u : 0xFu) << name << " pixel " << (i - 1);
                    continue;
                }
                EXPECT_EQ(g, w) << name << " pixel " << (i - 1) << " word " << k;
            }
        }
        d.ps = "ps_many";
        (void)run_c6_draw(*h, GetParam(), d, hr);
        EXPECT_EQ(hr, static_cast<u32>(E_INVALIDARG)) << name;
    }
}

TEST_P(D3D12DxilMetalTest, ConservativeLinesArePipelineErrors) {
    // Measured on WARP12 (d3d12_a66_c6_probe): ConservativeRaster with a
    // LINE or POINT topology type, with geometry-shader lines and with
    // domain-shader isolines is E_INVALIDARG.
    struct Case {
        const char* name;
        const char* vs;
        const char* gs;
        const char* hs;
        const char* ds;
        const char* ps;
        u32 type;
    };
    const Case cases[] = {
        {"lines cr", "vs_lines", nullptr, nullptr, nullptr, "ps_cov", D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
        {"points cr", "vs_points", nullptr, nullptr, nullptr, "ps_cov", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT},
        {"bary cr gs lines", "vs_c6", "gs_c6_lines", nullptr, nullptr, "ps_c6", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
        {"bary cr ds isolines", "vs_c6", nullptr, "hs_c6_iso", "ds_c6_iso", "ps_c6",
         D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH},
    };
    for (const Case& c : cases) {
        C6Draw d;
        d.name = c.name;
        d.vs = c.vs;
        d.gs = c.gs;
        d.hs = c.hs;
        d.ds = c.ds;
        d.ps = c.ps;
        d.topology_type = c.type;
        d.render_target = true;
        d.conservative = true;
        u32 hr = 0;
        (void)run_c6_draw(*h, GetParam(), d, hr);
        EXPECT_EQ(hr, static_cast<u32>(E_INVALIDARG)) << c.name;
    }
}

TEST_P(D3D12DxilMetalTest, RefusedProgramsFailPipelineCreation) {
    // The runtime refuses an unsigned container (measured: E_INVALIDARG).
    u32 hr = 0;
    // d3d12_sm6_exec_probe "container digest zero: 0x80070057".
    (void)run_generic(*h, GetParam(), "!cs_flow", false, hr);
    EXPECT_EQ(hr, static_cast<u32>(E_INVALIDARG));
}

}  // namespace
}  // namespace vitreo::winapi
