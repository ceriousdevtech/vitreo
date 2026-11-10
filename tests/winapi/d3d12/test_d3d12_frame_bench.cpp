// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_frame_bench.cpp
/// @brief A synthetic deferred SM5 frame through d3d12.dll on the GPU: where
/// the CPU time of a steady-state D3D12 frame goes.
///
/// The frame has the shape of an Unreal Engine SM5 deferred renderer, the
/// workload the D3D12 path is slow on: a depth prepass, a four-target base
/// pass recorded on four lists, shadow cascades (one depth slice each), a
/// point light's cube faces through a vertex shader that selects the slice and
/// through a geometry shader, full-screen lighting, a compute post chain and a
/// UI pass. Each draw does what the engine's D3D12 RHI does per draw: copy its
/// material's descriptors into a ring in one large shader-visible heap, set
/// that table and a root CBV into a per-frame upload ring, set the vertex and
/// index buffers, and change pipeline every few draws. Copy-queue uploads are
/// waited on by the direct queue; frames are kept two in flight.
///
/// The phases come from VITREO_D3D12_PROFILE's counters (frame_profile.hpp):
/// guest time in record calls, ExecuteCommandLists (replay), the Metal worker's
/// encode, CPU waits, and the GPU's busy time.
///
/// Run as a benchmark with VITREO_D3D12_FRAME_BENCH=<frames>; otherwise it
/// runs two small frames as a smoke test. Shaders: tests/oracle_data/
/// d3d12_frame_bench (fxc on winoracle-gpu).

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/common/frame_profile.hpp"
#include "graphics/common/gpu_busy.hpp"
#include "graphics/metal/metal_device.hpp"

#include "../../oracle_data/d3d12_frame_bench/d3d12_frame_bench_shaders.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;
namespace fp = graphics::frame_profile;

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x80000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kIid = kData + 0x100;
constexpr GuestAddr kOut = kData + 0x200;
constexpr GuestAddr kOut2 = kData + 0x240;
constexpr GuestAddr kDesc = kData + 0x400;
constexpr GuestAddr kDesc2 = kData + 0x800;
constexpr GuestAddr kArray = kData + 0xC00;
constexpr GuestAddr kScratch = kData + 0x1000;   // per-call structures
constexpr GuestAddr kBlob = kData + 0x4000;
constexpr GuestAddr kShader = kData + 0x8000;
constexpr GuestAddr kNames = kData + 0x10000;

/// Guest threads beyond the main one (the multithreaded bench): each has
/// its own CPU state, stack and scratch in its own region.
constexpr GuestAddr kThreadRegion = 0x200000;
constexpr usize kThreadRegionSize = 0x20000;
constexpr u32 kMaxThreads = 16;

/// One guest thread's call frame and its recording cursors: the descriptor
/// ring and upload-ring slices the lists it records allocate from.
struct Ctx {
    cpu::CpuState cpu;
    GuestAddr stack{kStack};
    GuestAddr scratch{kScratch};
    GuestAddr iid{kIid}, out{kOut}, out2{kOut2}, desc{kDesc}, desc2{kDesc2}, array{kArray},
        blob{kBlob}, shader{kShader}, names{kNames};
    u32 ring{0}, ring_start{0}, ring_end{0};
    u64 cb_cursor{0};
};
thread_local Ctx* t_ctx = nullptr;

/// x64 calls through the exports and vtables (as test_d3d12_metal.cpp).
class Guest {
public:
    Guest() {
        EXPECT_TRUE(memory.map_region(kData, kDataSize, MemoryProtection::ReadWrite)
                        .is_ok());
        EXPECT_TRUE(memory.map_region(kThreadRegion, kThreadRegionSize * kMaxThreads,
                                      MemoryProtection::ReadWrite)
                        .is_ok());
        main_ctx.cpu.mode = cpu::CpuMode::Long;
        for (u32 t = 0; t < kMaxThreads; ++t) {
            thread_ctx[t].cpu.mode = cpu::CpuMode::Long;
            Ctx& c = thread_ctx[t];
            const GuestAddr base = kThreadRegion + t * kThreadRegionSize;
            c.scratch = base;
            c.desc = base + 0x1000;
            c.desc2 = base + 0x1400;
            c.array = base + 0x1800;
            c.iid = base + 0x1900;
            c.out = base + 0x1A00;
            c.out2 = base + 0x1A40;
            c.blob = base + 0x2000;
            c.shader = base + 0x4000;   // 0x12000 bytes of bytecode
            c.names = base + 0x16000;
            c.stack = base + kThreadRegionSize - 0x1000;
        }
        manager = std::make_unique<D3D12Manager>(memory, true,
                                                 graphics::MetalDevice::instance());
        exports = create_d3d12_thunks(*manager);
        for (const auto& entry : exports.entries) by_name[entry.name] = &entry;
        D3D12Manager::MethodThunks thunks{};
        GuestAddr next = 0x70000000;
        const auto fill = [&](const char* interface_name, auto& table) {
            for (usize slot = 0; slot < table.size(); ++slot) {
                table[slot] = next;
                by_thunk[next] = by_name[d3d12_method_export_name(interface_name, slot)];
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

    void w32(GuestAddr at, u32 v) { (void)memory.write_u32(at, v); }
    void w64(GuestAddr at, u64 v) { (void)memory.write_u64(at, v); }
    void wf(GuestAddr at, float v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        w32(at, bits);
    }
    void zero(GuestAddr at, usize bytes) {
        static const std::vector<u8> z(0x4000, 0);
        (void)memory.write_bytes(at, z.data(), bytes);
    }
    [[nodiscard]] u64 r64(GuestAddr at) const { return memory.read_u64(at).value(); }

    /// One call: args in RCX, RDX, R8, R9 (and XMM0-3), then the stack.
    /// The calling thread's frame (the main one unless it is a worker).
    Ctx& ctx() { return t_ctx != nullptr ? *t_ctx : main_ctx; }

    u64 invoke(const ExportEntry* entry, std::initializer_list<u64> args) {
        Ctx& c = ctx();
        cpu::CpuState& cpu = c.cpu;
        cpu.gpr.rsp = c.stack;
        u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
        usize i = 0;
        for (const u64 value : args) {
            if (i < 4) {
                *regs[i] = value;
                cpu.sse.xmm[i].u64_[0] = value;
            } else {
                (void)memory.write_u64(c.stack + 0x28 + (i - 4) * 8, value);
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return cpu.gpr.rax;
    }
    u64 call(const std::string& name, std::initializer_list<u64> args) {
        return invoke(by_name.at(name), args);
    }
    /// COM method @p slot of @p object (this first).
    template <typename... A>
    u64 com(GuestAddr object, usize slot, A... args) {
        const u64 vtable = r64(object);
        const u64 thunk = r64(vtable + slot * 8);
        return invoke(by_thunk.at(thunk), {object, static_cast<u64>(args)...});
    }
    GuestAddr iid(const GuidBytes& value) {
        (void)memory.write_bytes(ctx().iid, value.data(), value.size());
        return ctx().iid;
    }
    GuestAddr made(u64 hr) {
        EXPECT_EQ(hr, S_OK);
        return hr == S_OK ? r64(ctx().out) : 0;
    }

    GuestMemory memory;
    Ctx main_ctx;
    Ctx thread_ctx[kMaxThreads];
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, const ExportEntry*> by_thunk;
};

/// The frame's sizes; a smoke run scales the draw counts down.
struct Shape {
    u32 width{1600};
    u32 height{900};
    u32 prepass_draws{1500};
    u32 base_draws{1500};          // over four lists
    u32 cascade_draws{300};        // per cascade, four cascades
    u32 vs_layer_draws{100};       // cube faces, vertex shader selects
    u32 gs_layer_draws{50};        // cube faces, geometry shader
    u32 lights{50};
    u32 post_dispatches{20};
    u32 ui_draws{300};
    u32 pso_run{4};                // draws between pipeline changes
    u32 materials{64};
    /// Live resources besides the frame's own: an engine keeps thousands
    /// (streamed meshes, textures, pooled buffers), and any per-draw walk of
    /// the object table pays for them.
    u32 extra_buffers{8000};
    u32 timestamps_per_frame{0}; // optional serial same-work query-cost probe
    bool resolve_each_timestamp{false};
};

constexpr u32 kSrvsPerMaterial = 12;
constexpr u32 kOnlineHeapSize = 500000;   // the engine's default online heap
constexpr u32 kIndicesPerDraw = 360;
constexpr u32 kVertices = 65536;

class FrameBench {
public:
    explicit FrameBench(const Shape& shape) : s(shape) {}
    ~FrameBench() { stop_workers(); }
    FrameBench(const FrameBench&) = delete;
    FrameBench& operator=(const FrameBench&) = delete;

    void build() {
        dev = g.made(g.call("D3D12CreateDevice", {0, D3D_FEATURE_LEVEL_11_0,
                                                  g.iid(IID_ID3D12DEVICE), g.ctx().out}));
        ASSERT_NE(dev, 0u);
        increment = static_cast<u32>(g.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        rtv_increment = static_cast<u32>(g.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize,
                                               D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
        dsv_increment = static_cast<u32>(g.com(dev, DeviceSlot::GetDescriptorHandleIncrementSize,
                                               D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
        sampler_increment = static_cast<u32>(g.com(
            dev, DeviceSlot::GetDescriptorHandleIncrementSize,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
        direct = queue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        copy = queue(D3D12_COMMAND_LIST_TYPE_COPY);
        frame_fence = fence();
        copy_fence = fence();
        make_roots();
        make_pipelines();
        make_resources();
        make_descriptors();
        if (s.timestamps_per_frame != 0) {
            const auto& q = layout::D3D12_QUERY_HEAP_DESC[1];
            g.zero(g.ctx().desc, q.size);
            g.w32(g.ctx().desc + q.Type, D3D12_QUERY_HEAP_TYPE_TIMESTAMP);
            g.w32(g.ctx().desc + q.Count, s.timestamps_per_frame * kFramesInFlight);
            timestamp_heap = g.made(g.com(dev, DeviceSlot::CreateQueryHeap,
                g.ctx().desc, g.iid(IID_ID3D12QUERYHEAP), g.ctx().out));
            ASSERT_NE(timestamp_heap, 0u);
            timestamp_readback = buffer(u64{s.timestamps_per_frame} * kFramesInFlight * sizeof(u64),
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        for (u32 f = 0; f < kFramesInFlight; ++f) {
            for (u32 l = 0; l < kLists; ++l) {
                allocators[f][l] = allocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
            }
            copy_allocators[f] = allocator(D3D12_COMMAND_LIST_TYPE_COPY);
        }
        for (u32 f = 0; f < kFramesInFlight; ++f) {
            for (u32 l = 0; l < kLists; ++l) {
                frame_lists[f][l] =
                    command_list(D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0][l]);
                ASSERT_EQ(g.com(frame_lists[f][l], GraphicsCommandListSlot::Close), S_OK);
            }
        }
        lists = frame_lists[0];
        copy_list = command_list(D3D12_COMMAND_LIST_TYPE_COPY, copy_allocators[0]);
        ASSERT_EQ(g.com(copy_list, GraphicsCommandListSlot::Close), S_OK);
    }

    /// Record and submit frame @p n (1-based), then wait for frame n - 2.
    void frame(u64 n) {
        const u32 f = static_cast<u32>(n % kFramesInFlight);
        // The GPU is past frame n - kFramesInFlight: its allocators are free.
        if (n > kFramesInFlight) wait(n - kFramesInFlight);
        timestamp_draws = timestamp_written = 0;
        timestamp_base = f * s.timestamps_per_frame;
        Ctx& c = g.ctx();
        c.ring = c.ring_start =
            static_cast<u32>((n % kFramesInFlight) * (kOnlineHeapSize / kFramesInFlight));
        c.ring_end = c.ring + kOnlineHeapSize / kFramesInFlight;
        c.cb_cursor = static_cast<u32>(f * kUploadPerFrame);
        for (u32 l = 0; l < kLists; ++l) {
            ASSERT_EQ(g.com(allocators[f][l], 8 /* Reset */), S_OK);
            ASSERT_EQ(g.com(lists[l], GraphicsCommandListSlot::Reset, allocators[f][l], 0u),
                      S_OK);
        }
        copy_upload(n);

        prepass(lists[0]);
        for (u32 l = 0; l < 4; ++l) base_pass(lists[1 + l], l);
        shadows(lists[5]);
        lighting_and_post(lists[6]);
        ui(lists[7]);
        if (s.timestamps_per_frame != 0) {
            ASSERT_EQ(timestamp_written, s.timestamps_per_frame);
            if (!s.resolve_each_timestamp) {
                (void)g.com(lists[7], GraphicsCommandListSlot::ResolveQueryData,
                    timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, timestamp_base,
                    s.timestamps_per_frame, timestamp_readback, u64{timestamp_base} * sizeof(u64));
            }
        }
        for (u32 l = 0; l < kLists; ++l) {
            ASSERT_EQ(g.com(lists[l], GraphicsCommandListSlot::Close), S_OK);
        }
        execute(direct, {lists[0]});
        execute(direct, {lists[1], lists[2], lists[3], lists[4]});
        execute(direct, {lists[5]});
        execute(direct, {lists[6]});
        execute(direct, {lists[7]});
        (void)g.com(direct, CommandQueueSlot::Signal, frame_fence, n);
    }

    void wait(u64 value) {
        ASSERT_EQ(g.com(frame_fence, FenceSlot::SetEventOnCompletion, value, 0u), S_OK);
    }

    void check_timestamps(u64 frame) {
        if (s.timestamps_per_frame == 0) return;
        ASSERT_EQ(g.com(timestamp_readback, ResourceSlot::Map, 0u, 0u, g.ctx().out2), S_OK);
        const GuestAddr data = g.r64(g.ctx().out2) +
            (frame % kFramesInFlight) * s.timestamps_per_frame * sizeof(u64);
        u64 previous = 0;
        for (u32 i = 0; i < s.timestamps_per_frame; ++i) {
            const u64 value = g.r64(data + i * sizeof(u64));
            EXPECT_GT(value, 0u);
            EXPECT_GE(value, previous);
            previous = value;
        }
        (void)g.com(timestamp_readback, ResourceSlot::Unmap, 0u, 0u);
    }

    // ---- the multithreaded frame (VITREO_D3D12_FRAME_BENCH_THREADS) -------
    //
    // As Unreal's parallel RHI translate: the eight lists of a frame are
    // recorded on worker threads, each list with its own allocator and its
    // own slices of the descriptor and upload rings; the main (RHI submit)
    // thread submits each group the moment its lists are closed, and the
    // workers record frame n + 1 while frame n executes.

    void start_workers(u32 threads) {
        workers_stop = false;
        for (u32 t = 0; t < threads && t < kMaxThreads; ++t) {
            workers.emplace_back([this, t] { worker(t); });
        }
    }
    void stop_workers() {
        {
            std::lock_guard<std::mutex> lock(task_mutex);
            workers_stop = true;
        }
        task_cv.notify_all();
        for (auto& worker_thread : workers) worker_thread.join();
        workers.clear();
    }

    /// Frames @p first .. @p last on the workers.
    void frames_threaded(u64 first, u64 last) {
        const auto allow = [&](u64 m) {
            // Frame m's allocators were last used by frame m - kFramesInFlight.
            if (m > kFramesInFlight) wait(m - kFramesInFlight);
            publish(m);
        };
        allow(first);
        for (u64 n = first; n <= last; ++n) {
            if (n < last) allow(n + 1);
            copy_upload(n);
            GuestAddr* frame = frame_lists[n % kFramesInFlight];
            wait_closed(n, {0});
            execute(direct, {frame[0]});
            wait_closed(n, {1, 2, 3, 4});
            execute(direct, {frame[1], frame[2], frame[3], frame[4]});
            wait_closed(n, {5});
            execute(direct, {frame[5]});
            wait_closed(n, {6});
            execute(direct, {frame[6]});
            wait_closed(n, {7});
            execute(direct, {frame[7]});
            (void)g.com(direct, CommandQueueSlot::Signal, frame_fence, n);
        }
    }

    /// Slot (0, 0) of the lit scene colour after the frame, read back.
    u32 probe_pixel() {
        const GuestAddr list = lists[0];
        ASSERT_RESULT(g.com(allocators[0][0], 8));
        ASSERT_RESULT(g.com(list, GraphicsCommandListSlot::Reset, allocators[0][0], 0u));
        transition(list, scene_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[1];
        g.zero(g.ctx().desc, l.size);
        g.zero(g.ctx().desc2, l.size);
        g.w64(g.ctx().desc + l.pResource, readback);
        g.w32(g.ctx().desc + l.Type, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        g.w32(g.ctx().desc + l.PlacedFootprint_Footprint_Format, DXGI_FORMAT_R16G16B16A16_FLOAT);
        g.w32(g.ctx().desc + l.PlacedFootprint_Footprint_Width, 32);
        g.w32(g.ctx().desc + l.PlacedFootprint_Footprint_Height, 1);
        g.w32(g.ctx().desc + l.PlacedFootprint_Footprint_Depth, 1);
        g.w32(g.ctx().desc + l.PlacedFootprint_Footprint_RowPitch, 256);
        g.w64(g.ctx().desc2 + l.pResource, scene_color);
        g.w32(g.ctx().desc2 + l.Type, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
        const auto& box = layout::D3D12_BOX[1];
        g.zero(scr(), box.size);
        g.w32(scr() + box.right, 32);
        g.w32(scr() + box.bottom, 1);
        g.w32(scr() + box.back, 1);
        (void)g.com(list, GraphicsCommandListSlot::CopyTextureRegion, g.ctx().desc, 0u, 0u, 0u,
                    g.ctx().desc2, scr());
        transition(list, scene_color, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ASSERT_RESULT(g.com(list, GraphicsCommandListSlot::Close));
        execute(direct, {list});
        ++probe_value;
        (void)g.com(direct, CommandQueueSlot::Signal, frame_fence, probe_value);
        wait(probe_value);
        EXPECT_EQ(g.com(readback, ResourceSlot::Map, 0u, 0u, g.ctx().out2), S_OK);
        const GuestAddr host = g.r64(g.ctx().out2);
        u32 word = 0;
        for (u32 k = 0; k < 64; ++k) word |= g.memory.read_u32(host + k * 4).value();
        (void)g.com(readback, ResourceSlot::Unmap, 0u, 0u);
        return word;
    }

    u64 probe_value{1ull << 40};

    /// One round of what a streaming / precaching thread does beside the
    /// frame, on guest thread frame @p t: a new pipeline state, a buffer
    /// and its views into the shader-visible heap's spare slots (the ones
    /// no frame's ring uses), descriptor copies into them, then the
    /// releases. Every call is free-threaded on Windows.
    D3D12Manager& manager() { return *g.manager; }

    void churn(u32 t, u32 round) {
        t_ctx = &g.thread_ctx[t];
        PsoDesc d;
        d.vs = g_bench_vs_depth;
        d.vs_size = sizeof(g_bench_vs_depth);
        d.elements = 1;
        d.depth = d.depth_write = true;
        d.depth_bias = static_cast<i32>(1000 + round);  // a pipeline no frame uses
        const GuestAddr pso = pipeline(d);
        EXPECT_NE(pso, 0u);
        const GuestAddr extra_buffer =
            buffer(65536, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        const u64 spare = online.cpu + u64{kSpareSlot} * increment;
        for (u32 k = 0; k < 16; ++k) {
            // A raw buffer SRV, then a material texture copied over it.
            const auto& l = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[1];
            g.zero(g.ctx().desc, l.size);
            g.w32(g.ctx().desc + l.Format, DXGI_FORMAT_R32_TYPELESS);
            g.w32(g.ctx().desc + l.ViewDimension, D3D12_SRV_DIMENSION_BUFFER);
            g.w32(g.ctx().desc + l.Shader4ComponentMapping,
                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
            g.w32(g.ctx().desc + l.Buffer_NumElements, 16384);
            g.w32(g.ctx().desc + l.Buffer_Flags, D3D12_BUFFER_SRV_FLAG_RAW);
            (void)g.com(dev, DeviceSlot::CreateShaderResourceView, extra_buffer,
                        g.ctx().desc, spare + (k % kSpareSlots) * u64{increment});
            (void)g.com(dev, DeviceSlot::CopyDescriptorsSimple, 1u,
                        spare + ((k + 1) % kSpareSlots) * u64{increment},
                        offline.cpu + (k % s.materials) * kSrvsPerMaterial * u64{increment},
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        {
            // A MINIMUM_ sampler into the sampler heap's spare slots (the
            // frames use the first four): the first one in the process makes
            // the backend rebuild its pipelines at their next draw, while
            // the frames draw.
            const auto& sl = layout::D3D12_SAMPLER_DESC[1];
            g.zero(g.ctx().desc, sl.size);
            g.w32(g.ctx().desc + sl.Filter, D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR);
            g.w32(g.ctx().desc + sl.AddressU, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
            g.w32(g.ctx().desc + sl.AddressV, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
            g.w32(g.ctx().desc + sl.AddressW, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
            g.w32(g.ctx().desc + sl.MaxAnisotropy, 1);
            g.w32(g.ctx().desc + sl.ComparisonFunc, D3D12_COMPARISON_FUNC_NEVER);
            g.wf(g.ctx().desc + sl.MaxLOD, 1000.0f);
            (void)g.com(dev, DeviceSlot::CreateSampler, g.ctx().desc,
                        samplers.cpu + (8 + round % 32) * u64{sampler_increment});
        }
        EXPECT_EQ(g.com(extra_buffer, 2 /* Release */), 0u);
        EXPECT_EQ(g.com(pso, 2 /* Release */), 0u);
        t_ctx = nullptr;
    }

private:
    /// Shader-visible heap slots past every frame's ring (churn).
    static constexpr u32 kSpareSlots = 64;
    static constexpr u32 kSpareSlot = kOnlineHeapSize + 0;
    static constexpr u32 kFramesInFlight = 3;
    static constexpr u32 kLists = 8;
    static constexpr u64 kUploadPerFrame = 4u << 20;

    // Helpers that may not use ASSERT (they return values).
    static void ASSERT_RESULT(u64 hr) { EXPECT_EQ(hr, S_OK); }

    /// The calling thread's scratch for per-call structures.
    GuestAddr scr() { return g.ctx().scratch; }

    /// Streaming uploads on the copy queue, waited on by the direct queue.
    void copy_upload(u64 n) {
        const u32 f = static_cast<u32>(n % kFramesInFlight);
        ASSERT_RESULT(g.com(copy_allocators[f], 8));
        ASSERT_RESULT(g.com(copy_list, GraphicsCommandListSlot::Reset, copy_allocators[f], 0u));
        (void)g.com(copy_list, GraphicsCommandListSlot::CopyBufferRegion, streamed,
                    u64{(n % 4) * 65536}, upload, u64{kUploadPerFrame * kFramesInFlight},
                    u64{65536});
        ASSERT_RESULT(g.com(copy_list, GraphicsCommandListSlot::Close));
        execute(copy, {copy_list});
        (void)g.com(copy, CommandQueueSlot::Signal, copy_fence, n);
        (void)g.com(direct, CommandQueueSlot::Wait, copy_fence, n);
    }

    struct Task {
        u64 frame{0};
        u32 list{0};
    };
    void publish(u64 n) {
        {
            std::lock_guard<std::mutex> lock(task_mutex);
            for (u32 l = 0; l < kLists; ++l) tasks.push_back({n, l});
        }
        task_cv.notify_all();
    }
    void wait_closed(u64 n, std::initializer_list<u32> which) {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] {
            for (const u32 l : which) {
                if (closed[n % kFramesInFlight][l] != n) return false;
            }
            return true;
        });
    }
    void worker(u32 t) {
        t_ctx = &g.thread_ctx[t];
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(task_mutex);
                task_cv.wait(lock, [&] { return workers_stop || !tasks.empty(); });
                if (tasks.empty()) break;
                task = tasks.front();
                tasks.erase(tasks.begin());
            }
            record_list(task.frame, task.list);
            {
                std::lock_guard<std::mutex> lock(done_mutex);
                closed[task.frame % kFramesInFlight][task.list] = task.frame;
            }
            done_cv.notify_all();
        }
        t_ctx = nullptr;
    }
    /// List @p l of frame @p n, from Reset to Close, on the calling worker.
    void record_list(u64 n, u32 l) {
        const u32 f = static_cast<u32>(n % kFramesInFlight);
        Ctx& c = g.ctx();
        const u32 per_frame = kOnlineHeapSize / kFramesInFlight;
        const u32 per_list = per_frame / kLists;
        c.ring = c.ring_start = f * per_frame + l * per_list;
        c.ring_end = c.ring_start + per_list;
        c.cb_cursor = f * kUploadPerFrame + l * (kUploadPerFrame / kLists);
        const GuestAddr list = frame_lists[f][l];
        ASSERT_RESULT(g.com(allocators[f][l], 8 /* Reset */));
        ASSERT_RESULT(g.com(list, GraphicsCommandListSlot::Reset, allocators[f][l], 0u));
        if (l == 0) {
            prepass(list);
        } else if (l <= 4) {
            base_pass(list, l - 1);
        } else if (l == 5) {
            shadows(list);
        } else if (l == 6) {
            lighting_and_post(list);
        } else {
            ui(list);
        }
        ASSERT_RESULT(g.com(list, GraphicsCommandListSlot::Close));
    }

    GuestAddr queue(u32 type) {
        const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[1];
        g.zero(g.ctx().desc, l.size);
        g.w32(g.ctx().desc + l.Type, type);
        return g.made(g.com(dev, DeviceSlot::CreateCommandQueue, g.ctx().desc,
                            g.iid(IID_ID3D12COMMANDQUEUE), g.ctx().out));
    }
    GuestAddr fence() {
        return g.made(g.com(dev, DeviceSlot::CreateFence, u64{0}, 0u,
                            g.iid(IID_ID3D12FENCE), g.ctx().out));
    }
    GuestAddr allocator(u32 type) {
        return g.made(g.com(dev, DeviceSlot::CreateCommandAllocator, type,
                            g.iid(IID_ID3D12COMMANDALLOCATOR), g.ctx().out));
    }
    GuestAddr command_list(u32 type, GuestAddr alloc) {
        return g.made(g.com(dev, DeviceSlot::CreateCommandList, 0u, type, alloc, 0u,
                            g.iid(IID_ID3D12GRAPHICSCOMMANDLIST), g.ctx().out));
    }
    void execute(GuestAddr q, std::initializer_list<GuestAddr> which) {
        usize i = 0;
        for (const GuestAddr list : which) g.w64(g.ctx().array + 8 * i++, list);
        (void)g.com(q, CommandQueueSlot::ExecuteCommandLists, static_cast<u32>(i), g.ctx().array);
    }

    GuestAddr root(const RootSignatureDesc& rs) {
        std::vector<u8> blob;
        std::string error;
        EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK)
            << error;
        (void)g.memory.write_bytes(g.ctx().blob, blob.data(), blob.size());
        return g.made(g.com(dev, DeviceSlot::CreateRootSignature, 0u, g.ctx().blob, blob.size(),
                            g.iid(IID_ID3D12ROOTSIGNATURE), g.ctx().out));
    }

    void make_roots() {
        const auto table = [](u32 type, u32 count, u32 visibility) {
            RootParameter p;
            p.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.shader_visibility = visibility;
            RootDescriptorRange r;
            r.range_type = type;
            r.num_descriptors = count;
            p.ranges.push_back(r);
            return p;
        };
        const auto cbv = [](u32 reg, u32 visibility) {
            RootParameter p;
            p.parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.shader_register = reg;
            p.shader_visibility = visibility;
            return p;
        };
        RootSignatureDesc rs;
        rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        RootParameter constants;
        constants.parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.shader_register = 3;
        constants.num_32bit_values = 32;  // PassConstants: 8 float4
        constants.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
        rs.parameters = {
            table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvsPerMaterial, D3D12_SHADER_VISIBILITY_PIXEL),
            table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, D3D12_SHADER_VISIBILITY_PIXEL),
            cbv(0, D3D12_SHADER_VISIBILITY_ALL),
            cbv(1, D3D12_SHADER_VISIBILITY_ALL),
            cbv(2, D3D12_SHADER_VISIBILITY_PIXEL),
            constants,
            table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, D3D12_SHADER_VISIBILITY_VERTEX),
            table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, D3D12_SHADER_VISIBILITY_VERTEX),
        };
        graphics_root = root(rs);
        RootSignatureDesc cs;
        RootParameter cs_constants = constants;
        cs.parameters = {table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, D3D12_SHADER_VISIBILITY_ALL),
                         table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, D3D12_SHADER_VISIBILITY_ALL),
                         cs_constants};
        compute_root = root(cs);
    }

    struct PsoDesc {
        const unsigned char* vs{nullptr};
        usize vs_size{0};
        const unsigned char* ps{nullptr};
        usize ps_size{0};
        const unsigned char* gs{nullptr};
        usize gs_size{0};
        u32 elements{0};        // 1: position, 2: position + attributes, 3: UI
        u32 targets{0};
        u32 target_format{DXGI_FORMAT_R16G16B16A16_FLOAT};
        bool depth{false};
        bool depth_write{false};
        u32 depth_func{D3D12_COMPARISON_FUNC_GREATER_EQUAL};
        bool blend{false};
        u32 cull{D3D12_CULL_MODE_BACK};
        i32 depth_bias{0};
    };

    GuestAddr pipeline(const PsoDesc& d) {
        GuestAddr at = g.ctx().shader;
        const auto put = [&](const unsigned char* bytes, usize size) {
            const GuestAddr here = at;
            (void)g.memory.write_bytes(here, bytes, size);
            at += (size + 255) & ~usize{255};
            return here;
        };
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[1];
        const auto& ie = layout::D3D12_INPUT_ELEMENT_DESC[1];
        const auto& bl = layout::D3D12_RENDER_TARGET_BLEND_DESC[1];
        g.zero(g.ctx().desc, gl.size);
        g.w64(g.ctx().desc + gl.pRootSignature, graphics_root);
        g.w64(g.ctx().desc + gl.VS_pShaderBytecode, put(d.vs, d.vs_size));
        g.w64(g.ctx().desc + gl.VS_BytecodeLength, d.vs_size);
        if (d.ps != nullptr) {
            g.w64(g.ctx().desc + gl.PS_pShaderBytecode, put(d.ps, d.ps_size));
            g.w64(g.ctx().desc + gl.PS_BytecodeLength, d.ps_size);
        }
        if (d.gs != nullptr) {
            g.w64(g.ctx().desc + gl.GS_pShaderBytecode, put(d.gs, d.gs_size));
            g.w64(g.ctx().desc + gl.GS_BytecodeLength, d.gs_size);
        }
        for (u32 t = 0; t < 8; ++t) {
            const GuestAddr rt = g.ctx().desc + gl.BlendState_RenderTarget + t * bl.size;
            (void)g.memory.write_u8(rt + bl.RenderTargetWriteMask, 0xF);
            if (d.blend && t < d.targets) {
                g.w32(rt + bl.BlendEnable, 1);
                g.w32(rt + bl.SrcBlend, D3D12_BLEND_ONE);
                g.w32(rt + bl.DestBlend, D3D12_BLEND_ONE);
                g.w32(rt + bl.BlendOp, D3D12_BLEND_OP_ADD);
                g.w32(rt + bl.SrcBlendAlpha, D3D12_BLEND_ONE);
                g.w32(rt + bl.DestBlendAlpha, D3D12_BLEND_ONE);
                g.w32(rt + bl.BlendOpAlpha, D3D12_BLEND_OP_ADD);
            }
        }
        g.w32(g.ctx().desc + gl.SampleMask, 0xFFFFFFFFu);
        g.w32(g.ctx().desc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        g.w32(g.ctx().desc + gl.RasterizerState_CullMode, d.cull);
        g.w32(g.ctx().desc + gl.RasterizerState_DepthBias, static_cast<u32>(d.depth_bias));
        g.w32(g.ctx().desc + gl.RasterizerState_DepthClipEnable, 1);
        g.w32(g.ctx().desc + gl.DepthStencilState_DepthEnable, d.depth ? 1u : 0u);
        g.w32(g.ctx().desc + gl.DepthStencilState_DepthWriteMask,
              d.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO);
        g.w32(g.ctx().desc + gl.DepthStencilState_DepthFunc, d.depth_func);
        // The input layout: semantic names in guest memory.
        static const char* kSemantic = "ATTRIBUTE";
        (void)g.memory.write_bytes(g.ctx().names, reinterpret_cast<const u8*>(kSemantic), 10);
        struct Element {
            u32 index, format, slot, offset;
        };
        std::vector<Element> elements;
        if (d.elements >= 1 && d.elements <= 2) {
            elements.push_back({0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0});
        }
        if (d.elements == 2) {
            elements.push_back({1, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0});
            elements.push_back({2, DXGI_FORMAT_R32G32_FLOAT, 1, 12});
            elements.push_back({3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 20});
        }
        if (d.elements == 3) {
            elements.push_back({0, DXGI_FORMAT_R32G32_FLOAT, 0, 0});
            elements.push_back({1, DXGI_FORMAT_R32G32_FLOAT, 0, 8});
        }
        const GuestAddr layout_at = scr() + 0x800;
        for (usize e = 0; e < elements.size(); ++e) {
            const GuestAddr el = layout_at + e * ie.size;
            g.zero(el, ie.size);
            g.w64(el + ie.SemanticName, g.ctx().names);
            g.w32(el + ie.SemanticIndex, elements[e].index);
            g.w32(el + ie.Format, elements[e].format);
            g.w32(el + ie.InputSlot, elements[e].slot);
            g.w32(el + ie.AlignedByteOffset, elements[e].offset);
            g.w32(el + ie.InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
        }
        if (!elements.empty()) {
            g.w64(g.ctx().desc + gl.InputLayout_pInputElementDescs, layout_at);
            g.w32(g.ctx().desc + gl.InputLayout_NumElements, static_cast<u32>(elements.size()));
        }
        g.w32(g.ctx().desc + gl.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        g.w32(g.ctx().desc + gl.NumRenderTargets, d.targets);
        for (u32 t = 0; t < d.targets; ++t) {
            g.w32(g.ctx().desc + gl.RTVFormats + t * 4, d.target_format);
        }
        if (d.depth) g.w32(g.ctx().desc + gl.DSVFormat, DXGI_FORMAT_D32_FLOAT);
        g.w32(g.ctx().desc + gl.SampleDesc_Count, 1);
        return g.made(g.com(dev, DeviceSlot::CreateGraphicsPipelineState, g.ctx().desc,
                            g.iid(IID_ID3D12PIPELINESTATE), g.ctx().out));
    }

    void make_pipelines() {
        // Prepass and shadow depth: four "materials" each (depth bias and
        // cull variants), as masked and two-sided materials make them.
        for (u32 v = 0; v < 4; ++v) {
            PsoDesc d;
            d.vs = g_bench_vs_depth;
            d.vs_size = sizeof(g_bench_vs_depth);
            d.elements = 1;
            d.depth = d.depth_write = true;
            d.cull = v % 2 == 0 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            d.depth_bias = static_cast<i32>(v * 3);
            depth_psos.push_back(pipeline(d));
        }
        for (u32 v = 0; v < 24; ++v) {
            PsoDesc d;
            d.vs = g_bench_vs_base;
            d.vs_size = sizeof(g_bench_vs_base);
            d.ps = g_bench_ps_base;
            d.ps_size = sizeof(g_bench_ps_base);
            d.elements = 2;
            d.targets = 4;
            d.depth = true;
            d.depth_write = false;
            d.cull = v % 3 == 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
            d.depth_bias = static_cast<i32>(v);  // distinct pipelines
            base_psos.push_back(pipeline(d));
        }
        {
            PsoDesc d;
            d.vs = g_bench_vs_layer;
            d.vs_size = sizeof(g_bench_vs_layer);
            d.elements = 1;
            d.depth = d.depth_write = true;
            layer_pso = pipeline(d);
            d.vs = g_bench_vs_layered;
            d.vs_size = sizeof(g_bench_vs_layered);
            d.gs = g_bench_gs_layered;
            d.gs_size = sizeof(g_bench_gs_layered);
            gs_pso = pipeline(d);
        }
        {
            PsoDesc d;
            d.vs = g_bench_vs_fullscreen;
            d.vs_size = sizeof(g_bench_vs_fullscreen);
            d.ps = g_bench_ps_light;
            d.ps_size = sizeof(g_bench_ps_light);
            d.targets = 1;
            d.blend = true;
            d.cull = D3D12_CULL_MODE_NONE;
            light_pso = pipeline(d);
        }
        {
            PsoDesc d;
            d.vs = g_bench_vs_ui;
            d.vs_size = sizeof(g_bench_vs_ui);
            d.ps = g_bench_ps_ui;
            d.ps_size = sizeof(g_bench_ps_ui);
            d.elements = 3;
            d.targets = 1;
            d.blend = true;
            d.cull = D3D12_CULL_MODE_NONE;
            ui_pso = pipeline(d);
        }
        {
            const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[1];
            g.zero(g.ctx().desc, cl.size);
            (void)g.memory.write_bytes(g.ctx().shader, g_bench_cs_post, sizeof(g_bench_cs_post));
            g.w64(g.ctx().desc + cl.pRootSignature, compute_root);
            g.w64(g.ctx().desc + cl.CS_pShaderBytecode, g.ctx().shader);
            g.w64(g.ctx().desc + cl.CS_BytecodeLength, sizeof(g_bench_cs_post));
            post_pso = g.made(g.com(dev, DeviceSlot::CreateComputePipelineState, g.ctx().desc,
                                    g.iid(IID_ID3D12PIPELINESTATE), g.ctx().out));
        }
    }

    GuestAddr texture(u32 width, u32 height, u32 slices, u32 format, u32 flags,
                      u32 state, u32 heap_type = D3D12_HEAP_TYPE_DEFAULT) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[1];
        const auto& rd = layout::D3D12_RESOURCE_DESC[1];
        g.zero(g.ctx().desc, hp.size);
        g.zero(g.ctx().desc2, rd.size);
        g.w32(g.ctx().desc + hp.Type, heap_type);
        g.w32(g.ctx().desc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        g.w64(g.ctx().desc2 + rd.Width, width);
        g.w32(g.ctx().desc2 + rd.Height, height);
        (void)g.memory.write_u16(g.ctx().desc2 + rd.DepthOrArraySize, static_cast<u16>(slices));
        (void)g.memory.write_u16(g.ctx().desc2 + rd.MipLevels, 1);
        g.w32(g.ctx().desc2 + rd.Format, format);
        g.w32(g.ctx().desc2 + rd.SampleDesc_Count, 1);
        g.w32(g.ctx().desc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
        g.w32(g.ctx().desc2 + rd.Flags, flags);
        return g.made(g.com(dev, DeviceSlot::CreateCommittedResource, g.ctx().desc, 0u, g.ctx().desc2,
                            state, 0u, g.iid(IID_ID3D12RESOURCE), g.ctx().out));
    }
    GuestAddr buffer(u64 bytes, u32 heap_type, u32 state, u32 flags = 0) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[1];
        const auto& rd = layout::D3D12_RESOURCE_DESC[1];
        g.zero(g.ctx().desc, hp.size);
        g.zero(g.ctx().desc2, rd.size);
        g.w32(g.ctx().desc + hp.Type, heap_type);
        g.w32(g.ctx().desc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
        g.w64(g.ctx().desc2 + rd.Width, bytes);
        g.w32(g.ctx().desc2 + rd.Height, 1);
        (void)g.memory.write_u16(g.ctx().desc2 + rd.DepthOrArraySize, 1);
        (void)g.memory.write_u16(g.ctx().desc2 + rd.MipLevels, 1);
        g.w32(g.ctx().desc2 + rd.SampleDesc_Count, 1);
        g.w32(g.ctx().desc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        g.w32(g.ctx().desc2 + rd.Flags, flags);
        return g.made(g.com(dev, DeviceSlot::CreateCommittedResource, g.ctx().desc, 0u, g.ctx().desc2,
                            state, 0u, g.iid(IID_ID3D12RESOURCE), g.ctx().out));
    }
    u64 va(GuestAddr resource) { return g.com(resource, ResourceSlot::GetGPUVirtualAddress); }

    void make_resources() {
        const u32 rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (auto& gb : gbuffer) {
            gb = texture(s.width, s.height, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        scene_color = texture(s.width, s.height, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              rt | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        post[0] = texture(s.width / 2, s.height / 2, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        post[1] = texture(s.width / 2, s.height / 2, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        depth = texture(s.width, s.height, 1, DXGI_FORMAT_R32_TYPELESS,
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        shadow = texture(1024, 1024, 4, DXGI_FORMAT_R32_TYPELESS,
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cube = texture(512, 512, 6, DXGI_FORMAT_R32_TYPELESS,
                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
        for (u32 m = 0; m < kTextures; ++m) {
            material_textures[m] = texture(64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        positions = buffer(kVertices * 12u, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        attributes = buffer(kVertices * 36u, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        indices = buffer(1u << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        materials_cb = buffer(64u * 1024u, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        // Per-frame constants (view + one Primitive block a draw), three
        // frames of it, plus the copy queue's source.
        upload = buffer(kUploadPerFrame * kFramesInFlight + 65536, D3D12_HEAP_TYPE_UPLOAD,
                        D3D12_RESOURCE_STATE_GENERIC_READ);
        streamed = buffer(4u * 65536u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        readback = buffer(4096, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        for (u32 k = 0; k < s.extra_buffers; ++k) {
            extra.push_back(buffer(4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON));
        }
        ASSERT_EQ(g.com(upload, ResourceSlot::Map, 0u, 0u, g.ctx().out2), S_OK);
        upload_host = g.r64(g.ctx().out2);
        upload_va = va(upload);
        positions_va = va(positions);
        attributes_va = va(attributes);
        indices_va = va(indices);
        materials_va = va(materials_cb);
        fill_geometry();
    }

    /// Every constant block of the upload ring: identity matrices, then
    /// parameters (View: view_params[0] = 0.5, [1] = (1, 1, 0, 0); Primitive:
    /// the same words). Meshes: 200 small clockwise triangles each at z = 0.5,
    /// mesh 0's first one over the top-left pixel; indices 0..359 per mesh.
    void fill_geometry() {
        std::vector<u8> block(256, 0);
        const auto put_f = [&](std::vector<u8>& to, usize at, float v) {
            std::memcpy(to.data() + at, &v, 4);
        };
        for (u32 k = 0; k < 4; ++k) put_f(block, (k * 4 + k) * 4, 1.0f);
        for (u32 k = 0; k < 4; ++k) put_f(block, 64 + k * 4, 0.5f);
        put_f(block, 80, 1.0f);
        put_f(block, 84, 1.0f);
        const u64 ring_bytes = kUploadPerFrame * kFramesInFlight + 65536;
        for (u64 at = 0; at + 256 <= ring_bytes; at += 256) {
            (void)g.memory.write_bytes(upload_host + at, block.data(), block.size());
        }
        const u64 pos_bytes = kVertices * 12u;
        const u64 attr_bytes = kVertices * 36u;
        const u64 index_bytes = 97u * 1024u;
        const u64 material_bytes = 64u * 1024u;
        const GuestAddr staging =
            buffer(pos_bytes + attr_bytes + index_bytes + material_bytes,
                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        ASSERT_EQ(g.com(staging, ResourceSlot::Map, 0u, 0u, g.ctx().out2), S_OK);
        const GuestAddr host = g.r64(g.ctx().out2);
        std::vector<float> pos(kVertices * 3);
        std::vector<float> attr(kVertices * 9);
        u32 seed = 12345;
        const auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (u32 v = 0; v < kVertices; v += 3) {
            float cx = rnd() * 1.9f - 0.95f;
            float cy = rnd() * 1.9f - 0.95f;
            if (v == 0) {
                cx = -1.0f;
                cy = 1.0f;
            }
            const float size = 0.06f;
            const float tri[3][2] = {{cx, cy}, {cx + size, cy}, {cx, cy - size}};
            for (u32 k = 0; k < 3 && v + k < kVertices; ++k) {
                pos[(v + k) * 3 + 0] = tri[k][0];
                pos[(v + k) * 3 + 1] = tri[k][1];
                pos[(v + k) * 3 + 2] = 0.5f;
                float* a = &attr[(v + k) * 9];
                a[0] = 0; a[1] = 0; a[2] = 1;      // normal
                a[3] = 0.5f; a[4] = 0.5f;          // uv
                a[5] = 1; a[6] = 0; a[7] = 0; a[8] = 1;  // tangent
            }
        }
        std::vector<u16> idx(index_bytes / 2, 0);
        for (u32 m = 0; m < 97; ++m) {
            for (u32 i = 0; i < kIndicesPerDraw; ++i) idx[m * 512 + i] = static_cast<u16>(i);
        }
        std::vector<float> mats(material_bytes / 4, 0.25f);
        (void)g.memory.write_bytes(host, reinterpret_cast<const u8*>(pos.data()), pos_bytes);
        (void)g.memory.write_bytes(host + pos_bytes, reinterpret_cast<const u8*>(attr.data()),
                                   attr_bytes);
        (void)g.memory.write_bytes(host + pos_bytes + attr_bytes,
                                   reinterpret_cast<const u8*>(idx.data()), index_bytes);
        (void)g.memory.write_bytes(host + pos_bytes + attr_bytes + index_bytes,
                                   reinterpret_cast<const u8*>(mats.data()), material_bytes);
        const GuestAddr alloc = allocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
        const GuestAddr list = command_list(D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
        (void)g.com(list, GraphicsCommandListSlot::CopyBufferRegion, positions, u64{0}, staging,
                    u64{0}, pos_bytes);
        (void)g.com(list, GraphicsCommandListSlot::CopyBufferRegion, attributes, u64{0},
                    staging, pos_bytes, attr_bytes);
        (void)g.com(list, GraphicsCommandListSlot::CopyBufferRegion, indices, u64{0}, staging,
                    pos_bytes + attr_bytes, index_bytes);
        (void)g.com(list, GraphicsCommandListSlot::CopyBufferRegion, materials_cb, u64{0},
                    staging, pos_bytes + attr_bytes + index_bytes, material_bytes);
        transition(list, positions, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        transition(list, attributes, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        transition(list, indices, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_INDEX_BUFFER);
        transition(list, materials_cb, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        ASSERT_EQ(g.com(list, GraphicsCommandListSlot::Close), S_OK);
        execute(direct, {list});
        ++probe_value;
        (void)g.com(direct, CommandQueueSlot::Signal, frame_fence, probe_value);
        wait(probe_value);
    }

    struct Heap {
        GuestAddr heap{0};
        u64 cpu{0};
        u64 gpu{0};
    };
    Heap descriptor_heap(u32 type, u32 count, bool visible) {
        const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[1];
        g.zero(g.ctx().desc, l.size);
        g.w32(g.ctx().desc + l.Type, type);
        g.w32(g.ctx().desc + l.NumDescriptors, count);
        g.w32(g.ctx().desc + l.Flags, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 0u);
        Heap h;
        h.heap = g.made(g.com(dev, DeviceSlot::CreateDescriptorHeap, g.ctx().desc,
                              g.iid(IID_ID3D12DESCRIPTORHEAP), g.ctx().out));
        (void)g.com(h.heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart, g.ctx().out2);
        h.cpu = g.r64(g.ctx().out2);
        if (visible) {
            (void)g.com(h.heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart, g.ctx().out2);
            h.gpu = g.r64(g.ctx().out2);
        }
        return h;
    }

    void srv(GuestAddr resource, u32 format, u32 slices, u64 dest) {
        const auto& l = layout::D3D12_SHADER_RESOURCE_VIEW_DESC[1];
        g.zero(g.ctx().desc, l.size);
        g.w32(g.ctx().desc + l.Format, format);
        g.w32(g.ctx().desc + l.Shader4ComponentMapping, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        if (slices > 1) {
            g.w32(g.ctx().desc + l.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
            g.w32(g.ctx().desc + l.Texture2DArray_MipLevels, 1);
            g.w32(g.ctx().desc + l.Texture2DArray_ArraySize, slices);
        } else {
            g.w32(g.ctx().desc + l.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2D);
            g.w32(g.ctx().desc + l.Texture2D_MipLevels, 1);
        }
        (void)g.com(dev, DeviceSlot::CreateShaderResourceView, resource, g.ctx().desc, dest);
    }
    void dsv(GuestAddr resource, u32 first, u32 slices, u64 dest) {
        const auto& l = layout::D3D12_DEPTH_STENCIL_VIEW_DESC[1];
        g.zero(g.ctx().desc, l.size);
        g.w32(g.ctx().desc + l.Format, DXGI_FORMAT_D32_FLOAT);
        g.w32(g.ctx().desc + l.ViewDimension, D3D12_DSV_DIMENSION_TEXTURE2DARRAY);
        g.w32(g.ctx().desc + l.Texture2DArray_FirstArraySlice, first);
        g.w32(g.ctx().desc + l.Texture2DArray_ArraySize, slices);
        (void)g.com(dev, DeviceSlot::CreateDepthStencilView, resource, g.ctx().desc, dest);
    }

    void make_descriptors() {
        online = descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                kOnlineHeapSize + kSpareSlots, true);
        offline = descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, false);
        samplers = descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, true);
        rtvs = descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, false);
        dsvs = descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false);
        // Offline: kSrvsPerMaterial per material, then the G-buffer + depth
        // (5), the post chain (SRV/UAV pairs), the height map.
        for (u32 m = 0; m < s.materials; ++m) {
            for (u32 k = 0; k < kSrvsPerMaterial; ++k) {
                srv(material_textures[(m * 7 + k) % kTextures], DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                    offline.cpu + (m * kSrvsPerMaterial + k) * u64{increment});
            }
        }
        lighting_srvs = s.materials * kSrvsPerMaterial;
        for (u32 k = 0; k < 4; ++k) {
            srv(gbuffer[k], DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                offline.cpu + (lighting_srvs + k) * u64{increment});
        }
        srv(depth, DXGI_FORMAT_R32_FLOAT, 1, offline.cpu + (lighting_srvs + 4) * u64{increment});
        post_descriptors = lighting_srvs + 5;
        // Post: [scene SRV, post0 UAV], [post0 SRV, post1 UAV], [post1 SRV, post0 UAV].
        const auto uav = [&](GuestAddr r, u64 dest) {
            const auto& l = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[1];
            g.zero(g.ctx().desc, l.size);
            g.w32(g.ctx().desc + l.Format, DXGI_FORMAT_R16G16B16A16_FLOAT);
            g.w32(g.ctx().desc + l.ViewDimension, D3D12_UAV_DIMENSION_TEXTURE2D);
            (void)g.com(dev, DeviceSlot::CreateUnorderedAccessView, r, 0u, g.ctx().desc, dest);
        };
        const auto at = [&](u32 index) { return offline.cpu + index * u64{increment}; };
        srv(scene_color, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, at(post_descriptors + 0));
        uav(post[0], at(post_descriptors + 1));
        srv(post[0], DXGI_FORMAT_R16G16B16A16_FLOAT, 1, at(post_descriptors + 2));
        uav(post[1], at(post_descriptors + 3));
        srv(post[1], DXGI_FORMAT_R16G16B16A16_FLOAT, 1, at(post_descriptors + 4));
        uav(post[0], at(post_descriptors + 5));
        height_srv = post_descriptors + 6;
        srv(material_textures[0], DXGI_FORMAT_R8G8B8A8_UNORM, 1, at(height_srv));
        // Samplers: four.
        const auto& sl = layout::D3D12_SAMPLER_DESC[1];
        for (u32 k = 0; k < 4; ++k) {
            g.zero(g.ctx().desc, sl.size);
            g.w32(g.ctx().desc + sl.Filter, k == 3 ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                            : D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            g.w32(g.ctx().desc + sl.AddressU, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
            g.w32(g.ctx().desc + sl.AddressV, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
            g.w32(g.ctx().desc + sl.AddressW, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
            g.w32(g.ctx().desc + sl.MaxAnisotropy, 1);
            g.w32(g.ctx().desc + sl.ComparisonFunc, D3D12_COMPARISON_FUNC_NEVER);
            g.wf(g.ctx().desc + sl.MaxLOD, 1000.0f);
            (void)g.com(dev, DeviceSlot::CreateSampler, g.ctx().desc,
                        samplers.cpu + k * u64{sampler_increment});
        }
        // RTVs: G-buffer 0-3, scene colour 4.
        const auto& rl = layout::D3D12_RENDER_TARGET_VIEW_DESC[1];
        const auto rtv = [&](GuestAddr r, u32 index) {
            g.zero(g.ctx().desc, rl.size);
            g.w32(g.ctx().desc + rl.Format, DXGI_FORMAT_R16G16B16A16_FLOAT);
            g.w32(g.ctx().desc + rl.ViewDimension, D3D12_RTV_DIMENSION_TEXTURE2D);
            (void)g.com(dev, DeviceSlot::CreateRenderTargetView, r, g.ctx().desc,
                        rtvs.cpu + index * u64{rtv_increment});
        };
        for (u32 k = 0; k < 4; ++k) rtv(gbuffer[k], k);
        rtv(scene_color, 4);
        // DSVs: scene depth 0, cascades 1-4, the cube 5.
        dsv(depth, 0, 1, dsvs.cpu);
        for (u32 k = 0; k < 4; ++k) dsv(shadow, k, 1, dsvs.cpu + (1 + k) * u64{dsv_increment});
        dsv(cube, 0, 6, dsvs.cpu + 5 * u64{dsv_increment});
        // The UI's vertices live in the attributes buffer (two float2).
    }

    // ---- recording helpers -------------------------------------------------

    void transition(GuestAddr list, GuestAddr resource, u32 before, u32 after) {
        const auto& l = layout::D3D12_RESOURCE_BARRIER[1];
        g.zero(scr(), l.size);
        g.w32(scr() + l.Type, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
        g.w64(scr() + l.Transition_pResource, resource);
        g.w32(scr() + l.Transition_Subresource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        g.w32(scr() + l.Transition_StateBefore, before);
        g.w32(scr() + l.Transition_StateAfter, after);
        (void)g.com(list, GraphicsCommandListSlot::ResourceBarrier, 1u, scr());
    }
    void uav_barrier(GuestAddr list, GuestAddr resource) {
        const auto& l = layout::D3D12_RESOURCE_BARRIER[1];
        g.zero(scr(), l.size);
        g.w32(scr() + l.Type, D3D12_RESOURCE_BARRIER_TYPE_UAV);
        g.w64(scr() + l.UAV_pResource, resource);
        (void)g.com(list, GraphicsCommandListSlot::ResourceBarrier, 1u, scr());
    }
    void viewport(GuestAddr list, u32 width, u32 height) {
        const auto& v = layout::D3D12_VIEWPORT[1];
        g.zero(scr() + 0x100, v.size);
        g.wf(scr() + 0x100 + v.Width, static_cast<float>(width));
        g.wf(scr() + 0x100 + v.Height, static_cast<float>(height));
        g.wf(scr() + 0x100 + v.MaxDepth, 1.0f);
        (void)g.com(list, GraphicsCommandListSlot::RSSetViewports, 1u, scr() + 0x100);
        g.w32(scr() + 0x140, 0);
        g.w32(scr() + 0x144, 0);
        g.w32(scr() + 0x148, width);
        g.w32(scr() + 0x14C, height);
        (void)g.com(list, GraphicsCommandListSlot::RSSetScissorRects, 1u, scr() + 0x140);
    }
    void targets(GuestAddr list, u32 first_rtv, u32 count, i32 dsv_index) {
        for (u32 k = 0; k < count; ++k) {
            g.w64(scr() + 0x200 + k * 8, rtvs.cpu + (first_rtv + k) * u64{rtv_increment});
        }
        g.w64(scr() + 0x280, dsvs.cpu + static_cast<u64>(std::max(dsv_index, 0)) * dsv_increment);
        (void)g.com(list, GraphicsCommandListSlot::OMSetRenderTargets, count,
                    count != 0 ? scr() + 0x200 : 0u, 0u,
                    dsv_index >= 0 ? scr() + 0x280 : 0u);
    }
    void clear_depth(GuestAddr list, u32 dsv_index) {
        u32 zero_bits = 0;
        (void)g.com(list, GraphicsCommandListSlot::ClearDepthStencilView,
                    dsvs.cpu + dsv_index * u64{dsv_increment}, D3D12_CLEAR_FLAG_DEPTH,
                    u64{zero_bits}, 0u, 0u, 0u);
    }
    void begin_graphics(GuestAddr list) {
        g.w64(scr() + 0x300, online.heap);
        g.w64(scr() + 0x308, samplers.heap);
        (void)g.com(list, GraphicsCommandListSlot::SetDescriptorHeaps, 2u, scr() + 0x300);
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, graphics_root);
        (void)g.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 1u,
                    samplers.gpu);
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 7u,
                    samplers.gpu);
        // The view's constants: one block a list, at the frame's cursor.
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView, 2u,
                    constants(256));
        // The vertex height map: copied once a list.
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 6u,
                    copy_descriptors(height_srv, 1));
    }
    /// A block of the frame's upload ring (the engine's fast constant
    /// allocator): the GPU VA of @p bytes, filled with a pattern.
    u64 constants(u32 bytes) {
        Ctx& c = g.ctx();
        const u64 at = c.cb_cursor;
        c.cb_cursor += (bytes + 255) & ~255u;
        g.w32(upload_host + at, static_cast<u32>(at));
        return upload_va + at;
    }
    /// CopyDescriptorsSimple of @p count offline descriptors into the online
    /// ring, as the engine's descriptor cache does per draw: the table's GPU
    /// handle.
    u64 copy_descriptors(u32 first, u32 count) {
        Ctx& c = g.ctx();
        if (c.ring + count > c.ring_end) c.ring = c.ring_start;
        const u64 dest = online.cpu + c.ring * u64{increment};
        const u64 gpu = online.gpu + c.ring * u64{increment};
        (void)g.com(dev, DeviceSlot::CopyDescriptorsSimple, count, dest,
                    offline.cpu + first * u64{increment},
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        c.ring += count;
        return gpu;
    }
    void mesh(GuestAddr list, u32 draw, bool attributes_too) {
        const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[1];
        const auto& ib = layout::D3D12_INDEX_BUFFER_VIEW[1];
        const u32 mesh_index = draw % 97;
        const u32 first = mesh_index * 600;
        g.w64(scr() + 0x400 + vb.BufferLocation, positions_va + first * 12u);
        g.w32(scr() + 0x400 + vb.SizeInBytes, 600 * 12u);
        g.w32(scr() + 0x400 + vb.StrideInBytes, 12);
        g.w64(scr() + 0x400 + vb.size + vb.BufferLocation, attributes_va + first * 36u);
        g.w32(scr() + 0x400 + vb.size + vb.SizeInBytes, 600 * 36u);
        g.w32(scr() + 0x400 + vb.size + vb.StrideInBytes, 36);
        (void)g.com(list, GraphicsCommandListSlot::IASetVertexBuffers, 0u,
                    attributes_too ? 2u : 1u, scr() + 0x400);
        g.w64(scr() + 0x480 + ib.BufferLocation, indices_va + mesh_index * 1024u);
        (void)first;
        g.w32(scr() + 0x480 + ib.SizeInBytes, kIndicesPerDraw * 2u);
        g.w32(scr() + 0x480 + ib.Format, DXGI_FORMAT_R16_UINT);
        (void)g.com(list, GraphicsCommandListSlot::IASetIndexBuffer, scr() + 0x480);
    }
    void timestamp_after_geometry_draw(GuestAddr list) {
        if (s.timestamps_per_frame == 0) return;
        ++timestamp_draws;
        const u32 total = s.prepass_draws + s.base_draws / 4 * 4 +
            s.cascade_draws * 4 + s.vs_layer_draws + s.gs_layer_draws;
        while (timestamp_written < s.timestamps_per_frame &&
               u64{timestamp_draws} * s.timestamps_per_frame >=
                   u64{timestamp_written + 1} * total) {
            const u32 slot = timestamp_base + timestamp_written++;
            (void)g.com(list, GraphicsCommandListSlot::EndQuery, timestamp_heap,
                D3D12_QUERY_TYPE_TIMESTAMP, slot);
            if (s.resolve_each_timestamp) {
                (void)g.com(list, GraphicsCommandListSlot::ResolveQueryData,
                    timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot, 1u,
                    timestamp_readback, u64{slot} * sizeof(u64));
            }
        }
    }
    void depth_draws(GuestAddr list, u32 count, u32 seed) {
        GuestAddr current = 0;
        for (u32 d = 0; d < count; ++d) {
            const GuestAddr pso = depth_psos[(d / s.pso_run + seed) % depth_psos.size()];
            if (pso != current) {
                (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, pso);
                current = pso;
            }
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView, 3u,
                        constants(256));
            mesh(list, d + seed, false);
            (void)g.com(list, GraphicsCommandListSlot::DrawIndexedInstanced, kIndicesPerDraw,
                        1u, 0u, 0u, 0u);
            timestamp_after_geometry_draw(list);
        }
    }

    void prepass(GuestAddr list) {
        begin_graphics(list);
        transition(list, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        targets(list, 0, 0, 0);
        clear_depth(list, 0);
        viewport(list, s.width, s.height);
        depth_draws(list, s.prepass_draws, 0);
    }

    void base_pass(GuestAddr list, u32 part) {
        begin_graphics(list);
        if (part == 0) {
            for (u32 k = 0; k < 4; ++k) {
                transition(list, gbuffer[k], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            float clear[4] = {0, 0, 0, 0};
            (void)g.memory.write_bytes(scr() + 0x500, reinterpret_cast<u8*>(clear), 16);
            for (u32 k = 0; k < 4; ++k) {
                (void)g.com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                            rtvs.cpu + k * u64{rtv_increment}, scr() + 0x500, 0u, 0u);
            }
        }
        targets(list, 0, 4, 0);
        viewport(list, s.width, s.height);
        const u32 count = s.base_draws / 4;
        GuestAddr current = 0;
        u32 material = ~0u;
        for (u32 d = 0; d < count; ++d) {
            const u32 global = part * count + d;
            const GuestAddr pso = base_psos[(global / s.pso_run) % base_psos.size()];
            if (pso != current) {
                (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, pso);
                current = pso;
            }
            const u32 m = (global / 2) % s.materials;
            if (m != material) {
                (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView,
                            4u, materials_va + m * 256u);
                material = m;
            }
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 0u,
                        copy_descriptors(m * kSrvsPerMaterial, kSrvsPerMaterial));
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView, 3u,
                        constants(256));
            mesh(list, global, true);
            (void)g.com(list, GraphicsCommandListSlot::DrawIndexedInstanced, kIndicesPerDraw,
                        1u, 0u, 0u, 0u);
            timestamp_after_geometry_draw(list);
        }
    }

    void shadows(GuestAddr list) {
        begin_graphics(list);
        viewport(list, 1024, 1024);
        for (u32 c = 0; c < 4; ++c) {
            targets(list, 0, 0, static_cast<i32>(1 + c));
            clear_depth(list, 1 + c);
            depth_draws(list, s.cascade_draws, c * 13);
        }
        viewport(list, 512, 512);
        targets(list, 0, 0, 5);
        clear_depth(list, 5);
        (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, layer_pso);
        for (u32 d = 0; d < s.vs_layer_draws; ++d) {
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView, 3u,
                        constants(256));
            mesh(list, d, false);
            (void)g.com(list, GraphicsCommandListSlot::DrawIndexedInstanced, kIndicesPerDraw,
                        1u, 0u, 0u, 0u);
            timestamp_after_geometry_draw(list);
        }
        (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, gs_pso);
        for (u32 d = 0; d < s.gs_layer_draws; ++d) {
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootConstantBufferView, 3u,
                        constants(256));
            mesh(list, d * 5, false);
            (void)g.com(list, GraphicsCommandListSlot::DrawIndexedInstanced, kIndicesPerDraw,
                        1u, 0u, 0u, 0u);
            timestamp_after_geometry_draw(list);
        }
    }

    void lighting_and_post(GuestAddr list) {
        begin_graphics(list);
        for (u32 k = 0; k < 4; ++k) {
            transition(list, gbuffer[k], D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(list, scene_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        targets(list, 4, 1, -1);
        {
            float clear[4] = {0.25f, 0.25f, 0.25f, 0.25f};
            (void)g.memory.write_bytes(scr() + 0x500, reinterpret_cast<u8*>(clear), 16);
            (void)g.com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                        rtvs.cpu + 4 * u64{rtv_increment}, scr() + 0x500, 0u, 0u);
        }
        viewport(list, s.width, s.height);
        (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, light_pso);
        (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 0u,
                    copy_descriptors(lighting_srvs, 5));
        for (u32 l = 0; l < s.lights; ++l) {
            for (u32 k = 0; k < 8; ++k) g.wf(scr() + 0x600 + k * 4, 0.01f * static_cast<float>(l + k));
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRoot32BitConstants, 5u, 8u,
                        scr() + 0x600, 0u);
            (void)g.com(list, GraphicsCommandListSlot::DrawInstanced, 3u, 1u, 0u, 0u);
        }
        transition(list, scene_color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // The post chain: compute, ping-ponging two half-size textures.
        (void)g.com(list, GraphicsCommandListSlot::SetComputeRootSignature, compute_root);
        (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, post_pso);
        for (u32 p = 0; p < s.post_dispatches; ++p) {
            const u32 pair = p == 0 ? 0u : 1u + (p % 2);
            const u64 table = copy_descriptors(post_descriptors + pair * 2, 2);
            (void)g.com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable, 0u, table);
            (void)g.com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable, 1u,
                        table + increment);
            for (u32 k = 0; k < 8; ++k) g.wf(scr() + 0x600 + k * 4, 1.0f);
            (void)g.com(list, GraphicsCommandListSlot::SetComputeRoot32BitConstants, 2u, 8u,
                        scr() + 0x600, 0u);
            (void)g.com(list, GraphicsCommandListSlot::Dispatch, (s.width / 2 + 7) / 8,
                        (s.height / 2 + 7) / 8, 1u);
            uav_barrier(list, post[0]);
            uav_barrier(list, post[1]);
        }
        transition(list, scene_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    void ui(GuestAddr list) {
        begin_graphics(list);
        targets(list, 4, 1, -1);
        viewport(list, s.width, s.height);
        (void)g.com(list, GraphicsCommandListSlot::SetPipelineState, ui_pso);
        const auto& vb = layout::D3D12_VERTEX_BUFFER_VIEW[1];
        g.w64(scr() + 0x400 + vb.BufferLocation, attributes_va);
        g.w32(scr() + 0x400 + vb.SizeInBytes, 4096 * 16u);
        g.w32(scr() + 0x400 + vb.StrideInBytes, 16);
        (void)g.com(list, GraphicsCommandListSlot::IASetVertexBuffers, 0u, 1u, scr() + 0x400);
        for (u32 d = 0; d < s.ui_draws; ++d) {
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable, 0u,
                        copy_descriptors((d % s.materials) * kSrvsPerMaterial, 1));
            for (u32 k = 0; k < 8; ++k) g.wf(scr() + 0x600 + k * 4, 0.001f * static_cast<float>(d + k));
            (void)g.com(list, GraphicsCommandListSlot::SetGraphicsRoot32BitConstants, 5u, 8u,
                        scr() + 0x600, 0u);
            (void)g.com(list, GraphicsCommandListSlot::DrawInstanced, 6u, 1u, (d % 600) * 6u, 0u);
        }
        transition(list, scene_color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    Shape s;
    Guest g;
    GuestAddr dev{0}, direct{0}, copy{0}, frame_fence{0}, copy_fence{0};
    GuestAddr timestamp_heap{0}, timestamp_readback{0};
    u32 timestamp_draws{0}, timestamp_written{0}, timestamp_base{0};
    u32 increment{0}, rtv_increment{0}, dsv_increment{0}, sampler_increment{0};
    GuestAddr graphics_root{0}, compute_root{0};
    std::vector<GuestAddr> depth_psos, base_psos;
    GuestAddr layer_pso{0}, gs_pso{0}, light_pso{0}, ui_pso{0}, post_pso{0};
    static constexpr u32 kTextures = 48;
    GuestAddr gbuffer[4]{}, scene_color{0}, post[2]{}, depth{0}, shadow{0}, cube{0};
    GuestAddr material_textures[kTextures]{};
    std::vector<GuestAddr> extra;
    GuestAddr positions{0}, attributes{0}, indices{0}, materials_cb{0}, upload{0},
        streamed{0}, readback{0};
    u64 upload_host{0}, upload_va{0}, positions_va{0}, attributes_va{0}, indices_va{0},
        materials_va{0};
    Heap online, offline, samplers, rtvs, dsvs;
    u32 lighting_srvs{0}, post_descriptors{0}, height_srv{0};
    GuestAddr allocators[kFramesInFlight][kLists]{};
    GuestAddr copy_allocators[kFramesInFlight]{};
    GuestAddr frame_lists[kFramesInFlight][kLists]{};
    GuestAddr* lists{frame_lists[0]};
    GuestAddr copy_list{0};
    // The workers (frames_threaded).
    std::vector<std::thread> workers;
    std::mutex task_mutex;
    std::condition_variable task_cv;
    std::vector<Task> tasks;
    bool workers_stop{false};
    std::mutex done_mutex;
    std::condition_variable done_cv;
    u64 closed[kFramesInFlight][kLists]{};
};

class D3D12FrameBench : public ::testing::Test {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        ::setenv("VITREO_D3D12", "1", 1);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
    }
};

/// The frame on the main thread alone (@p threads 0), or recorded on
/// @p threads workers (FrameBench::frames_threaded).
void run_frame_bench(u32 threads) {
    const char* frames_env = std::getenv("VITREO_D3D12_FRAME_BENCH");
    const u32 frames = frames_env != nullptr ? static_cast<u32>(std::atoi(frames_env)) : 0u;
    Shape shape;
    if (frames == 0) {
        // Smoke: a few draws of each kind, two frames.
        shape.width = 256;
        shape.height = 144;
        shape.prepass_draws = 16;
        shape.base_draws = 16;
        shape.cascade_draws = 4;
        shape.vs_layer_draws = 2;
        shape.gs_layer_draws = 2;
        shape.lights = 2;
        shape.post_dispatches = 2;
        shape.ui_draws = 4;
        shape.materials = 8;
        shape.extra_buffers = 16;
    }
    // VITREO_D3D12_FRAME_BENCH_SUBSET=geometry: the prepass and base pass
    // alone, the passes test_d3d11_frame_bench.cpp records through D3D11.
    if (const char* subset = std::getenv("VITREO_D3D12_FRAME_BENCH_SUBSET");
        subset != nullptr && std::strcmp(subset, "geometry") == 0) {
        shape.cascade_draws = shape.vs_layer_draws = shape.gs_layer_draws = 0;
        shape.lights = shape.post_dispatches = shape.ui_draws = 0;
    }
    // VITREO_D3D12_FRAME_BENCH_PROFILE=0: the profile off, to measure its cost
    // (only the wall time is then meaningful).
    const char* profile_env = std::getenv("VITREO_D3D12_FRAME_BENCH_PROFILE");
    if (const char* query_env = std::getenv("VITREO_D3D12_FRAME_BENCH_TIMESTAMPS")) {
        char* end = nullptr;
        const unsigned long queries = std::strtoul(query_env, &end, 10);
        ASSERT_TRUE(end != query_env && *end == '\0' && queries <= 1024);
        ASSERT_EQ(threads, 0u) << "timestamp-cost probe uses the serial frame";
        shape.timestamps_per_frame = static_cast<u32>(queries);
    }
    if (const char* each = std::getenv("VITREO_D3D12_FRAME_BENCH_RESOLVE_EACH_TIMESTAMP")) {
        shape.resolve_each_timestamp = each[0] != '\0' && each[0] != '0';
        ASSERT_EQ(threads, 0u);
    }
    const bool profiled = frames == 0 || profile_env == nullptr || profile_env[0] != '0';
    fp::set_enabled_for_testing(profiled);
    auto bench = std::make_unique<FrameBench>(shape);
    bench->build();
    if (::testing::Test::HasFatalFailure()) return;
    const u32 warm = frames == 0 ? 1u : 30u;
    const u32 measured = frames == 0 ? 1u : frames;
    u64 n = 0;
    const auto run = [&](u32 count) {
        if (threads == 0) {
            for (u32 i = 0; i < count; ++i) bench->frame(++n);
        } else {
            bench->frames_threaded(n + 1, n + count);
            n += count;
        }
        bench->wait(n);
    };
    if (threads != 0) bench->start_workers(threads);
    run(warm);
    (void)fp::take();
    char reasons[4096];
    fp::format_reasons(reasons, sizeof(reasons));
    graphics::reset_gpu_busy_for_testing();
    const u64 start = fp::now_ns();
    const double gpu_start = graphics::gpu_busy_clock_s();
    run(measured);
    const u64 wall = fp::now_ns() - start;
    const double gpu_end = graphics::gpu_busy_clock_s();
    const fp::Snapshot snap = fp::take();
    fp::format_reasons(reasons, sizeof(reasons));
    const graphics::RecentGpuBusy busy = graphics::recent_gpu_busy(gpu_end);
    (void)gpu_start;
    const double per = static_cast<double>(measured);
    const auto ms = [&](fp::Time t) {
        return static_cast<double>(snap.times[static_cast<usize>(t)]) / 1e6 / per;
    };
    const auto count = [&](fp::Counter c) {
        return static_cast<double>(snap.counters[static_cast<usize>(c)]) / per;
    };
    // The GPU's busy time over the last second, scaled to a frame.
    const double window_s = std::min(1.0, static_cast<double>(wall) / 1e9);
    const double frames_in_window = window_s * per / (static_cast<double>(wall) / 1e9);
    const double gpu_ms = frames_in_window > 0 ? busy.busy_ms / frames_in_window : 0.0;
    std::printf(
        "[d3d12-frame-bench] frames=%u threads=%u timestamps=%u resolve-each=%u wall=%.2fms/frame record=%.2f "
        "execute=%.2f (replay=%.2f flush=%.2f) encode=%.2f cpu-wait=%.2f "
        "lock{wait=%.2f contended=%.0f} gpu=%.2f "
        "variants=%.1f/%.2fms draws=%.0f disp=%.0f encoded=%.0f render-enc=%.1f "
        "compute-enc=%.1f blit-enc=%.1f begun=%.1f merged=%.1f residency=%.0f "
        "window-resolves=%.0f cbv-size-scans=%.0f upload=%.1fKB\n  %s\n",
        measured, threads, shape.timestamps_per_frame, unsigned(shape.resolve_each_timestamp), static_cast<double>(wall) / 1e6 / per, ms(fp::Time::ApiRecord),
        ms(fp::Time::ApiExecute), ms(fp::Time::Replay), ms(fp::Time::BackendFlush),
        ms(fp::Time::Encode), ms(fp::Time::CpuWait), ms(fp::Time::LockWait),
        count(fp::Counter::LockContended), gpu_ms,
        count(fp::Counter::VariantCompiles), ms(fp::Time::VariantCompile),
        count(fp::Counter::Draws), count(fp::Counter::Dispatches),
        count(fp::Counter::EncodedDraws), count(fp::Counter::RenderEncoders),
        count(fp::Counter::ComputeEncoders), count(fp::Counter::BlitEncoders),
        count(fp::Counter::PassesBegun), count(fp::Counter::PassesMerged),
        count(fp::Counter::ResidencyDecls), count(fp::Counter::WindowResolves),
        count(fp::Counter::RootCbvSizeScans),
        static_cast<double>(snap.counters[static_cast<usize>(fp::Counter::UploadBytes)]) /
            1024.0 / per,
        reasons);
    const u32 expected_draws = shape.prepass_draws + shape.base_draws / 4 * 4 +
                               shape.cascade_draws * 4 + shape.vs_layer_draws +
                               shape.gs_layer_draws + shape.lights + shape.ui_draws;
    if (profiled) {
    EXPECT_EQ(snap.counters[static_cast<usize>(fp::Counter::Draws)],
              static_cast<u64>(expected_draws) * measured);
    EXPECT_EQ(snap.counters[static_cast<usize>(fp::Counter::Dispatches)],
              static_cast<u64>(shape.post_dispatches) * measured);
    // Every draw reached the Metal worker.
    EXPECT_GE(snap.counters[static_cast<usize>(fp::Counter::EncodedDraws)],
              static_cast<u64>(expected_draws) * measured);
    }
    // The scene colour reached the readback: its clear (0.25) plus the lights.
    EXPECT_NE(bench->probe_pixel(), 0u);
    bench->check_timestamps(n);
    fp::set_enabled_for_testing(false);
}

TEST_F(D3D12FrameBench, DeferredFrame) { run_frame_bench(0); }

/// The frame recorded on VITREO_D3D12_FRAME_BENCH_THREADS workers (default
/// 4); its record calls, descriptor copies and replay overlap.
TEST_F(D3D12FrameBench, DeferredFrameThreaded) {
    const char* env = std::getenv("VITREO_D3D12_FRAME_BENCH_THREADS");
    const u32 threads = env != nullptr ? static_cast<u32>(std::atoi(env)) : 4u;
    run_frame_bench(std::clamp(threads, 1u, kMaxThreads));
}

/// Learn: command lists record concurrently, any thread submits to any
/// queue, device creation and descriptor writes are free-threaded. Four
/// threads record the frame's lists, the main thread executes them as they
/// close, and a fifth creates and releases pipelines, buffers and views and
/// writes spare descriptors of the heap the frames draw from -- all at
/// once. The frame still renders, every draw is replayed, and no exclusive
/// lock request is made under a shared hold. The ThreadSanitizer build
/// (VITREO_TSAN) runs this test for data races.
TEST_F(D3D12FrameBench, RecordExecuteAndCreateConcurrently) {
    Shape shape;
    shape.width = 256;
    shape.height = 144;
    shape.prepass_draws = 24;
    shape.base_draws = 24;
    shape.cascade_draws = 4;
    shape.vs_layer_draws = 2;
    shape.gs_layer_draws = 2;
    shape.lights = 2;
    shape.post_dispatches = 2;
    shape.ui_draws = 6;
    shape.materials = 8;
    shape.extra_buffers = 16;
    fp::set_enabled_for_testing(true);
    auto bench = std::make_unique<FrameBench>(shape);
    bench->build();
    if (HasFatalFailure()) return;
    bench->start_workers(4);
    (void)fp::take();
    constexpr u64 kFrames = 12;
    std::atomic<bool> frames_done{false};
    std::thread creator([&] {
        // At least three rounds, overlapping the frames as long as they run.
        for (u32 round = 0; round < 3 || (!frames_done.load() && round < 32); ++round) {
            bench->churn(kMaxThreads - 1, round);
        }
    });
    bench->frames_threaded(1, kFrames);
    bench->wait(kFrames);
    frames_done = true;
    creator.join();
    const fp::Snapshot snap = fp::take();
    const u64 expected_draws = shape.prepass_draws + shape.base_draws / 4 * 4 +
                               shape.cascade_draws * 4 + shape.vs_layer_draws +
                               shape.gs_layer_draws + shape.lights + shape.ui_draws;
    EXPECT_EQ(snap.counters[static_cast<usize>(fp::Counter::Draws)], expected_draws * kFrames);
    EXPECT_EQ(bench->manager().frontend().m_lock.upgrades(), 0u);
    EXPECT_NE(bench->probe_pixel(), 0u);
    fp::set_enabled_for_testing(false);
}

}  // namespace
}  // namespace vitreo::winapi
