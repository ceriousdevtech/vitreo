// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_threading.cpp
/// @brief The device's threading contract on the real Metal backend, through
///        the lock-taking guest entries (create_d3d11_thunks).
///
/// Learn ("Introduction to a Device in Direct3D 11", "Multithreading in
/// Direct3D 11", ID3D11Device): every ID3D11Device method is free-threaded
/// and may run concurrently with the others and with the immediate context,
/// which only the application must serialise. So shader creation on several
/// threads while another drives the context must give each caller exactly
/// what a serial call gives (S_OK, a distinct object with one reference that
/// renders by its HLSL), and must neither deadlock nor hold the device's one
/// lock through its translation. Expected texels are the HLSL's output
/// (d3d11_array_window.hlsl ps_cbcol returns c0) through D3D11.3 3.2.3.6
/// FLOAT -> UNORM: n/255 for n in 0..255 stores n exactly.

#include <gtest/gtest.h>

#include "d3d11_test_metal_vt_feedback.hpp"
#include "volume_lut_shaders.h"
#include "../../graphics/tessellation_shaders.h"
#include "../../oracle_data/d3d11_array_window_shaders.h"

#include <array>
#include <atomic>
#include <bit>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

class DeviceThreadingTest : public VtFeedbackTest {
protected:
    static constexpr u32 kSide = 4;

    void SetUp() override {
        VtFeedbackTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) return;
        m_exports = create_d3d11_thunks(d3d11);
    }
    void TearDown() override {
        D3D11Manager::set_pure_work_unlock_hook_for_test(nullptr, nullptr);
        VtFeedbackTest::TearDown();
    }

    [[nodiscard]] DirectHandler entry(const std::string& name) const {
        for (const ExportEntry& candidate : m_exports.entries) {
            if (candidate.name == name) return candidate.direct;
        }
        ADD_FAILURE() << "no export " << name;
        return DirectHandler{};
    }

    /// One guest thread's frame: its own registers, stack and out-slots.
    struct GuestThread {
        cpu::CpuState cpu{};
        Marshaler marshaler;
        GuestAddr base{0};
        explicit GuestThread(GuestMemory& memory) : marshaler(memory) {}
    };
    std::unique_ptr<GuestThread> guest_thread() {
        auto thread = std::make_unique<GuestThread>(memory);
        thread->base = memory.map_first_fit(0x20000000ull, 0x4000,
                                            MemoryProtection::ReadWrite);
        EXPECT_NE(thread->base, 0u);
        thread->cpu.mode = cpu::CpuMode::Long;
        thread->cpu.gpr.rsp = thread->base + 0x2000;
        thread->marshaler.attach_cpu(thread->cpu);
        return thread;
    }
    /// x64: `this` and three arguments in RCX, RDX, R8, R9; the rest at
    /// RSP + 0x28.
    u64 call(GuestThread& thread, const DirectHandler& direct, GuestAddr self,
             std::initializer_list<u64> args) {
        thread.cpu.gpr.rcx = self;
        usize index = 1;
        for (const u64 arg : args) {
            switch (index) {
            case 1: thread.cpu.gpr.rdx = arg; break;
            case 2: thread.cpu.gpr.r8 = arg; break;
            case 3: thread.cpu.gpr.r9 = arg; break;
            default:
                (void)memory.write_u64(
                    thread.cpu.gpr.rsp + 0x28 + (index - 4) * 8, arg);
                break;
            }
            ++index;
        }
        direct.fn(direct.context, thread.marshaler, direct.slot);
        return thread.cpu.gpr.rax;
    }
    /// ID3D11Device::Create*Shader(code, size, NULL, ppShader) on @p thread.
    struct Created {
        u32 hr{0};
        GuestAddr shader{0};
    };
    Created create(GuestThread& thread, usize slot, GuestAddr code,
                   usize size, u32 out_index) {
        const GuestAddr out = thread.base + 0x3000 + out_index * 8;
        (void)memory.write_u64(out, 0xBAD);
        Created result{};
        result.hr = static_cast<u32>(call(
            thread, entry(d3d11_device_method_export_name(slot)), device,
            {code, size, 0, out}));
        result.shader = memory.read_u64(out).value();
        return result;
    }

    static std::array<u32, 16> colour(u32 r, u32 g, u32 b) {
        std::array<u32, 16> p{};
        p[0] = std::bit_cast<u32>(r / 255.0f);
        p[1] = std::bit_cast<u32>(g / 255.0f);
        p[2] = std::bit_cast<u32>(b / 255.0f);
        p[3] = std::bit_cast<u32>(1.0f);
        return p;
    }
    /// R8G8B8A8_UNORM of (r, g, b, 255), as a little-endian word.
    static u32 rgba8(u32 r, u32 g, u32 b) {
        return r | (g << 8) | (b << 16) | (0xFFu << 24);
    }

    /// A 4x4 target bound with the full-screen pipeline of @p ps and a b0.
    void bind_target(GuestAddr ps) {
        m_target = texture(kSide, kSide, 1, 1, kRgba8Unorm,
                           D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);
        m_rtv = render_target_view(m_target, kRgba8Unorm, 0);
        pipeline(ps);
        viewport(0, 0, kSide, kSide);
        const std::array<u64, 1> list{m_rtv};
        context_call(33, {1, put(list), 0});  // OMSetRenderTargets
        const auto words = colour(0, 0, 0);
        m_params = buffer(64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER,
                          0, 0, 0, words.data());
        const std::array<u64, 1> cbs{m_params};
        context_call(16, {0, 1, put(cbs)});  // PSSetConstantBuffers
    }
    /// Texel (0,0) and (3,3) of the target.
    std::vector<u32> corners() {
        const auto texels =
            read_uint_level(m_target, kRgba8Unorm, kSide, kSide, 1, 0);
        if (texels.size() != kSide * kSide) return {};
        return {texels[0], texels[15]};
    }

    DllExports m_exports;
    GuestAddr m_target{0};
    GuestAddr m_rtv{0};
    GuestAddr m_params{0};
};

/// Learn (ID3D11Device): Create*Shader is free-threaded. The translation of
/// every stage runs with the device lock released -- another thread can take
/// it meanwhile -- unless the caller already holds the device's critical
/// section through ID3D11Multithread::Enter, which must keep excluding every
/// other thread until Leave.
TEST_F(DeviceThreadingTest, ShaderTranslationDoesNotHoldTheDeviceLock) {
    struct Probe {
        DxgiManager* dxgi;
        std::atomic<int> calls{0};
        std::atomic<int> free{0};
    } probe{&dxgi};
    D3D11Manager::set_pure_work_unlock_hook_for_test(
        [](void* context) {
            auto& p = *static_cast<Probe*>(context);
            ++p.calls;
            std::thread other([&p] {
                if (p.dxgi->api_mutex().try_lock()) {
                    ++p.free;
                    p.dxgi->api_mutex().unlock();
                }
            });
            other.join();
        },
        &probe);

    auto thread = guest_thread();
    ASSERT_NE(thread->base, 0u);
    struct Stage {
        usize slot;
        const unsigned char* code;
        usize size;
        const char* name;
    };
    const Stage stages[] = {
        {12, vtfb_vs_fullscreen, sizeof(vtfb_vs_fullscreen), "VS"},
        {13, lut_gs, sizeof(lut_gs), "GS"},
        {15, g_ps_cbcol, sizeof(g_ps_cbcol), "PS"},
        {16, tess_hs_tri, sizeof(tess_hs_tri), "HS"},
        {17, tess_ds_tri, sizeof(tess_ds_tri), "DS"},
        {18, g_cs_ld2d, sizeof(g_cs_ld2d), "CS"},
    };
    u32 index = 0;
    for (const Stage& stage : stages) {
        SCOPED_TRACE(stage.name);
        const int calls_before = probe.calls.load();
        const Created created = create(*thread, stage.slot,
                                       put(stage.code, stage.size), stage.size,
                                       index++);
        EXPECT_EQ(created.hr, d3d11::S_OK);
        EXPECT_NE(created.shader, 0u);
        EXPECT_GE(probe.calls.load(), calls_before + 1)
            << "the translation ran with the device lock held";
        EXPECT_EQ(probe.free.load(), probe.calls.load())
            << "another thread could not take the device lock meanwhile";
    }

    // Inside Enter (the critical section counted in api_lock_depth, as
    // impl_MultithreadMethod takes it) the lock stays held throughout.
    dxgi.api_mutex().lock();
    ++d3d11.api_lock_depth();
    const int calls_before = probe.calls.load();
    const Created inside = create(*thread, 15, put(g_ps_cbcol,
                                                   sizeof(g_ps_cbcol)),
                                  sizeof(g_ps_cbcol), index++);
    EXPECT_EQ(inside.hr, d3d11::S_OK);
    EXPECT_NE(inside.shader, 0u);
    EXPECT_EQ(probe.calls.load(), calls_before)
        << "an Enter hold was given up";
    --d3d11.api_lock_depth();
    dxgi.api_mutex().unlock();

    // Validation only (ppShader NULL): S_FALSE, translated unlocked too.
    const int before_validation = probe.calls.load();
    const GuestAddr code = put(g_ps_cbcol, sizeof(g_ps_cbcol));
    EXPECT_EQ(static_cast<u32>(call(*thread,
                                    entry(d3d11_device_method_export_name(15)),
                                    device, {code, sizeof(g_ps_cbcol), 0, 0})),
              1u);
    EXPECT_GE(probe.calls.load(), before_validation + 1);
}

/// A guest draw whose pipeline is new compiles it with the lock released
/// (single device, multithread protection off); the same draw again is a
/// cache hit and gives nothing up. Either way the draw renders c0.
TEST_F(DeviceThreadingTest, FirstDrawCompilesItsPipelineUnlocked) {
    const GuestAddr ps = shader(15, g_ps_cbcol, sizeof(g_ps_cbcol));
    ASSERT_NE(ps, 0u);
    bind_target(ps);
    if (HasFatalFailure()) return;
    const auto words = colour(0x30, 0x60, 0x90);
    context_call(48, {m_params, 0, 0, put(words), 64, 64});

    struct Probe {
        DxgiManager* dxgi;
        std::atomic<int> calls{0};
        std::atomic<int> free{0};
    } probe{&dxgi};
    D3D11Manager::set_pure_work_unlock_hook_for_test(
        [](void* context) {
            auto& p = *static_cast<Probe*>(context);
            ++p.calls;
            std::thread other([&p] {
                if (p.dxgi->api_mutex().try_lock()) {
                    ++p.free;
                    p.dxgi->api_mutex().unlock();
                }
            });
            other.join();
        },
        &probe);
    auto thread = guest_thread();
    ASSERT_NE(thread->base, 0u);
    const DirectHandler draw_entry = entry(d3d11_context_method_export_name(13));
    (void)call(*thread, draw_entry, context, {3, 0});  // Draw(3, 0)
    EXPECT_EQ(probe.calls.load(), 1);
    EXPECT_EQ(probe.free.load(), 1);
    (void)call(*thread, draw_entry, context, {3, 0});
    EXPECT_EQ(probe.calls.load(), 1) << "a cached pipeline gave the lock up";
    D3D11Manager::set_pure_work_unlock_hook_for_test(nullptr, nullptr);
    EXPECT_EQ(corners(), (std::vector<u32>{rgba8(0x30, 0x60, 0x90),
                                           rgba8(0x30, 0x60, 0x90)}));
}

/// Four threads create vertex, pixel and compute shaders while the test
/// thread keeps drawing on the immediate context. Every creation answers as
/// a serial one does, no call deadlocks, the draws meanwhile render their
/// colour, and each concurrently created pixel shader renders c0 afterwards.
TEST_F(DeviceThreadingTest, ConcurrentShaderCreationMatchesSerialCreation) {
    const GuestAddr serial_ps = shader(15, g_ps_cbcol, sizeof(g_ps_cbcol));
    ASSERT_NE(serial_ps, 0u);
    bind_target(serial_ps);
    if (HasFatalFailure()) return;

    constexpr u32 kThreads = 4;
    constexpr u32 kRounds = 6;
    const GuestAddr ps_code = put(g_ps_cbcol, sizeof(g_ps_cbcol));
    const GuestAddr vs_code = put(vtfb_vs_fullscreen,
                                  sizeof(vtfb_vs_fullscreen));
    const GuestAddr cs_code = put(g_cs_ld2d, sizeof(g_cs_ld2d));
    const auto words = colour(0x10, 0x80, 0xF0);
    const GuestAddr words_at = put(words);

    std::vector<std::unique_ptr<GuestThread>> threads;
    for (u32 t = 0; t < kThreads + 1; ++t) {
        threads.push_back(guest_thread());
        ASSERT_NE(threads.back()->base, 0u);
    }
    struct Result {
        std::vector<Created> ps, vs, cs;
    };
    std::vector<Result> results(kThreads);
    std::atomic<u32> running{kThreads};
    std::vector<std::thread> workers;
    for (u32 t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            GuestThread& self = *threads[t];
            for (u32 r = 0; r < kRounds; ++r) {
                results[t].ps.push_back(
                    create(self, 15, ps_code, sizeof(g_ps_cbcol), 3 * r));
                results[t].vs.push_back(create(
                    self, 12, vs_code, sizeof(vtfb_vs_fullscreen), 3 * r + 1));
                results[t].cs.push_back(
                    create(self, 18, cs_code, sizeof(g_cs_ld2d), 3 * r + 2));
            }
            --running;
        });
    }
    // The immediate context, driven through the same lock-taking entries.
    GuestThread& immediate = *threads[kThreads];
    const DirectHandler update = entry(d3d11_context_method_export_name(48));
    const DirectHandler draw_entry = entry(d3d11_context_method_export_name(13));
    u32 draws = 0;
    while (running.load() != 0 || draws < 4) {
        (void)call(immediate, update, context,
                   {m_params, 0, 0, words_at, 64, 64});
        (void)call(immediate, draw_entry, context, {3, 0});
        ++draws;
    }
    for (std::thread& worker : workers) worker.join();
    EXPECT_GE(draws, 4u);
    EXPECT_EQ(corners(), (std::vector<u32>{rgba8(0x10, 0x80, 0xF0),
                                           rgba8(0x10, 0x80, 0xF0)}));

    std::set<GuestAddr> seen{serial_ps};
    for (u32 t = 0; t < kThreads; ++t) {
        for (const auto* list :
             {&results[t].ps, &results[t].vs, &results[t].cs}) {
            ASSERT_EQ(list->size(), kRounds);
            for (const Created& c : *list) {
                EXPECT_EQ(c.hr, d3d11::S_OK);
                EXPECT_NE(c.shader, 0u);
                EXPECT_TRUE(seen.insert(c.shader).second)
                    << "two creations returned one object";
            }
        }
    }
    // Each pixel shader made concurrently renders c0, as the serial one.
    u32 shade = 0;
    for (u32 t = 0; t < kThreads; ++t) {
        for (const Created& c : results[t].ps) {
            const u32 r = 0x20 + 4 * shade, g = 0xC0 - 2 * shade, b = shade;
            ++shade;
            const auto c0 = colour(r, g, b);
            context_call(48, {m_params, 0, 0, put(c0), 64, 64});
            context_call(9, {c.shader, 0, 0});  // PSSetShader
            draw();
            EXPECT_EQ(corners(), (std::vector<u32>{rgba8(r, g, b),
                                                   rgba8(r, g, b)}))
                << "shader 0x" << std::hex << c.shader;
        }
    }
    context_call(9, {serial_ps, 0, 0});
    // One reference each: the application's (Learn, IUnknown::Release
    // returns the new count). The bound one is held by the context too.
    for (u32 t = 0; t < kThreads; ++t) {
        for (const Created& c : results[t].ps) {
            set_arg(0, c.shader);
            d3d11.impl_PixelShaderMethod(marshaler, 2);
            EXPECT_EQ(cpu.gpr.rax, 0u);
        }
        for (const Created& c : results[t].vs) {
            set_arg(0, c.shader);
            d3d11.impl_VertexShaderMethod(marshaler, 2);
            EXPECT_EQ(cpu.gpr.rax, 0u);
        }
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
