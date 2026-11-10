// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_concurrent_pso.cpp
/// @brief Pipeline states created from several guest threads at once.
///
/// Learn ("Multi-engine synchronization", "Free threading"): ID3D12Device's
/// create methods are free-threaded, and an engine precaching pipelines
/// calls them from every worker. The frontend builds a pipeline -- DXIL
/// lowering, DXBC -> SPIR-V -> MSL and the Metal compile -- outside its lock
/// (Frontend::build_pipeline_unlocked), so those builds run side by side.
/// These tests check that a concurrent build gives exactly what a serial
/// one does, and that the builds overlap.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"

#include "../../oracle_data/d3d12_shaders/d3d12_shaders.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

// These are host synchronization invariants, independent of guest bitness.
// Recording may overlap a reserved writer only inside its explicit window;
// object mutation and backend replay must remain excluded throughout.
TEST(D3D12FrontendLock, RecordingWindowAdmitsWaitingRecordersOnly) {
    using namespace std::chrono_literals;
    FrontendRwLock lock;
    (void)lock.lock(FrontendRwLock::Mode::Exclusive);
    auto recorder = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Recording);
        lock.unlock();
    });
    auto reader = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Shared);
        lock.unlock();
    });
    auto writer = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Exclusive);
        lock.unlock();
    });
    EXPECT_EQ(recorder.wait_for(20ms), std::future_status::timeout);
    EXPECT_TRUE(lock.begin_recording_window());
    EXPECT_EQ(recorder.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(reader.wait_for(20ms), std::future_status::timeout);
    EXPECT_EQ(writer.wait_for(20ms), std::future_status::timeout);
    lock.end_recording_window();
    lock.unlock();
    recorder.get(); reader.get(); writer.get();
}

TEST(D3D12FrontendLock, ClosingWindowDrainsItsRecorders) {
    using namespace std::chrono_literals;
    FrontendRwLock lock;
    std::promise<void> opened, close, recording, release;
    auto closed = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Exclusive);
        EXPECT_TRUE(lock.begin_recording_window());
        opened.set_value();
        close.get_future().wait();
        lock.end_recording_window();
        lock.unlock();
    });
    opened.get_future().wait();
    auto recorder = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Recording);
        recording.set_value();
        release.get_future().wait();
        lock.unlock();
    });
    recording.get_future().wait();
    close.set_value();
    EXPECT_EQ(closed.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(closed.wait_for(2s), std::future_status::ready);
    closed.get(); recorder.get();
}

TEST(D3D12FrontendLock, NestedOwnerRestoresExclusionAndUnsupportedHoldsStayClosed) {
    using namespace std::chrono_literals;
    FrontendRwLock lock;
    EXPECT_FALSE(lock.begin_recording_window());
    (void)lock.lock(FrontendRwLock::Mode::Recording);
    EXPECT_FALSE(lock.begin_recording_window());
    // A broader nested API must upgrade rather than inherit recording-only
    // permission. This also retains the original recursive upgrade contract.
    (void)lock.lock(FrontendRwLock::Mode::Shared);
    EXPECT_EQ(lock.mode(), FrontendRwLock::Mode::Exclusive);
    EXPECT_EQ(lock.upgrades(), 1u);
    EXPECT_FALSE(lock.begin_recording_window()); // depth two
    lock.unlock();
    EXPECT_TRUE(lock.begin_recording_window());
    (void)lock.lock(FrontendRwLock::Mode::Shared); // closes window
    auto recorder = std::async(std::launch::async, [&] {
        (void)lock.lock(FrontendRwLock::Mode::Recording);
        lock.unlock();
    });
    EXPECT_EQ(recorder.wait_for(30ms), std::future_status::timeout);
    lock.end_recording_window(); // idempotent after owner reentry
    lock.unlock(); lock.unlock();
    EXPECT_EQ(recorder.wait_for(2s), std::future_status::ready);
    recorder.get();
}

constexpr GuestAddr kRegion = 0x100000;
constexpr usize kSlotBytes = 0x40000;
constexpr usize kSlots = 17;  // slot 0 is the main thread's
constexpr usize kThreads = 8;

/// One guest thread's call frame: its own CPU state and scratch memory.
struct Frame {
    cpu::CpuState cpu;
    GuestAddr base{0};
    [[nodiscard]] GuestAddr stack() const { return base + 0x3F000; }
    [[nodiscard]] GuestAddr iid() const { return base + 0x100; }
    [[nodiscard]] GuestAddr out() const { return base + 0x200; }
    [[nodiscard]] GuestAddr desc() const { return base + 0x400; }
    [[nodiscard]] GuestAddr elements() const { return base + 0xC00; }
    [[nodiscard]] GuestAddr names() const { return base + 0xD00; }
    [[nodiscard]] GuestAddr shader_a() const { return base + 0x2000; }
    [[nodiscard]] GuestAddr shader_b() const { return base + 0x20000; }
    [[nodiscard]] GuestAddr blob() const { return base + 0x38000; }
};

/// The exports and vtables of one x64 D3D12Manager, callable from any
/// thread with its own Frame (the maps are read-only once built).
class Harness {
public:
    Harness() {
        EXPECT_TRUE(memory.map_region(kRegion, kSlotBytes * kSlots,
                                      MemoryProtection::ReadWrite).is_ok());
        manager = std::make_unique<D3D12Manager>(
            memory, true, graphics::MetalDevice::instance());
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

    [[nodiscard]] Frame frame(usize slot) const {
        Frame f;
        f.base = kRegion + slot * kSlotBytes;
        f.cpu.mode = cpu::CpuMode::Long;
        return f;
    }

    u64 call(Frame& f, const std::string& name, const std::vector<u64>& args) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) return ~0ull;
        f.cpu.gpr.rsp = f.stack();
        f.cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        u64* regs[] = {&f.cpu.gpr.rcx, &f.cpu.gpr.rdx, &f.cpu.gpr.r8,
                       &f.cpu.gpr.r9};
        for (usize i = 0; i < args.size(); ++i) {
            if (i < 4) {
                *regs[i] = args[i];
            } else {
                (void)memory.write_u64(f.stack() + 0x28 + (i - 4) * 8, args[i]);
            }
        }
        Marshaler m(memory);
        m.attach_cpu(f.cpu);
        found->second->handler(m);
        return f.cpu.gpr.rax;
    }

    u64 com(Frame& f, GuestAddr object, usize slot, std::vector<u64> args) {
        const u64 vtable = memory.read_u64(object).value();
        const u64 thunk = memory.read_u64(vtable + slot * 8).value();
        const auto name = by_thunk.find(thunk);
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), object);
        return call(f, name->second, args);
    }

    GuestAddr iid(Frame& f, const GuidBytes& value) {
        (void)memory.write_bytes(f.iid(), value.data(), value.size());
        return f.iid();
    }

    /// The object a (riid, ppv) create wrote, or 0 when it failed.
    GuestAddr made(Frame& f, u64 hr) {
        return hr == S_OK ? memory.read_u64(f.out()).value() : 0;
    }

    void zero(GuestAddr at, usize bytes) {
        const std::vector<u8> z(bytes, 0);
        (void)memory.write_bytes(at, z.data(), z.size());
    }

    GuestAddr root_signature(Frame& f, const RootSignatureDesc& rs) {
        std::vector<u8> blob;
        std::string error;
        EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                           error),
                  S_OK)
            << error;
        (void)memory.write_bytes(f.blob(), blob.data(), blob.size());
        return made(f, com(f, device, DeviceSlot::CreateRootSignature,
                           {0, f.blob(), blob.size(),
                            iid(f, IID_ID3D12ROOTSIGNATURE), f.out()}));
    }

    GuestMemory memory;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    GuestAddr device{0};
};

/// cs_madd with its dcl_thread_group changed to (x, y, 1), re-signed: a
/// different program for every (x, y), so each is its own Metal compile.
std::vector<u8> cs_with_group(u32 x, u32 y) {
    std::vector<u8> bytes(g_sh_cs_madd, g_sh_cs_madd + sizeof(g_sh_cs_madd));
    // D3D11.3 22.3.38 dcl_thread_group: opcode 155 (0x9B), length 4.
    constexpr u32 kToken = 0x0400009Bu;
    for (usize at = 32; at + 16 <= bytes.size(); at += 4) {
        u32 word = 0;
        std::memcpy(&word, bytes.data() + at, 4);
        if (word != kToken) continue;
        const u32 group[3] = {x, y, 1};
        std::memcpy(bytes.data() + at + 4, group, sizeof(group));
        u32 total = 0;
        std::memcpy(&total, bytes.data() + 24, 4);
        u8 hash[16];
        graphics::dxbc_container_checksum(bytes.data(), total, hash);
        std::memcpy(bytes.data() + 4, hash, 16);
        return bytes;
    }
    ADD_FAILURE() << "cs_madd has no dcl_thread_group";
    return {};
}

/// @p src (a DXBC container) with its SHEX token stream passed through
/// @p edit, the chunk offsets, TotalSize and hash redone.
template <typename Edit>
std::vector<u8> edit_shex(const u8* src, Edit edit) {
    const auto rd = [&](usize at) {
        u32 v = 0;
        std::memcpy(&v, src + at, 4);
        return v;
    };
    const u32 chunks = rd(28);
    std::vector<std::vector<u8>> parts;
    std::vector<std::array<u8, 4>> tags;
    for (u32 c = 0; c < chunks; ++c) {
        const u32 at = rd(32 + c * 4);
        const u32 size = rd(at + 4);
        std::array<u8, 4> tag{};
        std::memcpy(tag.data(), src + at, 4);
        std::vector<u8> body(src + at + 8, src + at + 8 + size);
        if (std::memcmp(tag.data(), "SHEX", 4) == 0) {
            std::vector<u32> tokens(size / 4);
            std::memcpy(tokens.data(), body.data(), size);
            if (!edit(tokens)) return {};
            tokens[1] = static_cast<u32>(tokens.size());
            body.resize(tokens.size() * 4);
            std::memcpy(body.data(), tokens.data(), body.size());
        }
        tags.push_back(tag);
        parts.push_back(std::move(body));
    }
    std::vector<u8> out(32 + chunks * 4u);
    std::memcpy(out.data(), src, 32);
    for (u32 c = 0; c < chunks; ++c) {
        const u32 at = static_cast<u32>(out.size());
        std::memcpy(out.data() + 32 + c * 4, &at, 4);
        const u32 size = static_cast<u32>(parts[c].size());
        out.insert(out.end(), tags[c].begin(), tags[c].end());
        const u8* sz = reinterpret_cast<const u8*>(&size);
        out.insert(out.end(), sz, sz + 4);
        out.insert(out.end(), parts[c].begin(), parts[c].end());
    }
    const u32 total = static_cast<u32>(out.size());
    std::memcpy(out.data() + 24, &total, 4);
    u8 hash[16];
    graphics::dxbc_container_checksum(out.data(), total, hash);
    std::memcpy(out.data() + 4, hash, 16);
    EXPECT_TRUE(graphics::dxbc_container_verified(out.data(), out.size()));
    return out;
}

/// The token index just past the first instruction of @p opcode, or 0.
usize after_opcode(const std::vector<u32>& tokens, u32 opcode) {
    for (usize i = 2; i < tokens.size();) {
        const u32 op = tokens[i] & 0x7FFu;
        const u32 length = (tokens[i] >> 24) & 0x7Fu;
        if (length == 0) return 0;
        i += length;
        if (op == opcode) return i;
    }
    return 0;
}

/// @p count dependent `mad rN.c, rN.c, l(a), l(b)` (D3D11.3 22.10.8; operand
/// tokens as fxc writes them) on the temp component @p mask_token /
/// @p select_token name, every constant drawn from @p seed.
std::vector<u32> mad_chain(u32 seed, u32 count, u32 mask_token, u32 select_token) {
    std::vector<u32> chain;
    std::mt19937 rng(seed);
    for (u32 k = 0; k < count; ++k) {
        const float a = 1.0f + static_cast<float>(rng() % 1000u) * 1e-6f;
        const float b = static_cast<float>(rng() % 1000u) * 1e-4f;
        u32 ab[2];
        std::memcpy(&ab[0], &a, 4);
        std::memcpy(&ab[1], &b, 4);
        chain.insert(chain.end(), {0x09000032u, mask_token, 0u, select_token, 0u,
                                   0x00004001u, ab[0], 0x00004001u, ab[1]});
    }
    return chain;
}

/// cs_madd with @p count dependent mads on r0.y inserted after its ld_raw
/// (whose r0.y the store writes out): a program the size of a real
/// material's, so its translation and Metal compile cost what a game's do.
/// Frame::shader_a holds it up to 3000.
std::vector<u8> cs_with_chain(u32 seed, u32 count) {
    if (count > 3000) return {};
    return edit_shex(g_sh_cs_madd, [&](std::vector<u32>& tokens) {
        const usize at = after_opcode(tokens, 165u);  // ld_raw
        if (at == 0) return false;
        const auto chain = mad_chain(seed, count, 0x00100022u, 0x0010001Au);
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(at),
                      chain.begin(), chain.end());
        return true;
    });
}

/// ps_color writing o0.x through @p count dependent mads of v1.x (after its
/// `mov o0, v1`), with the dcl_temps that needs: a distinct pixel shader
/// for every seed.
std::vector<u8> ps_with_chain(u32 seed, u32 count) {
    if (count > 3000) return {};
    return edit_shex(g_sh_ps_color, [&](std::vector<u32>& tokens) {
        const usize dcl = after_opcode(tokens, 101u);  // dcl_output
        if (dcl == 0) return false;
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(dcl),
                      {0x02000068u, 1u});  // dcl_temps 1
        const usize at = after_opcode(tokens, 54u);  // mov o0, v1
        if (at == 0) return false;
        std::vector<u32> body = {0x05000036u, 0x00100012u, 0u, 0x0010100Au, 1u};
        const auto chain = mad_chain(seed, count, 0x00100012u, 0x0010000Au);
        body.insert(body.end(), chain.begin(), chain.end());
        body.insert(body.end(), {0x05000036u, 0x00102012u, 0u, 0x0010000Au, 0u});
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(at), body.begin(),
                      body.end());
        return true;
    });
}

/// A distinct (x, y) for variant @p index, starting at a per-run offset so
/// the Metal compiler service's own cache does not answer from an earlier
/// run of this test.
struct Groups {
    u32 salt;
    [[nodiscard]] std::pair<u32, u32> of(u32 index) const {
        const u32 n = (salt + index) % 1024u;
        return {1u + n % 32u, 1u + n / 32u};
    }
};

class D3D12ConcurrentPsoTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        ::setenv("VITREO_D3D12", "1", 1);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
        h = std::make_unique<Harness>();
        Frame f = h->frame(0);
        h->device = h->made(
            f, h->call(f, "D3D12CreateDevice",
                       {0, D3D_FEATURE_LEVEL_11_0, h->iid(f, IID_ID3D12DEVICE),
                        f.out()}));
        ASSERT_NE(h->device, 0u);
        // cs_madd: constants at b0, a raw UAV at u2.
        RootSignatureDesc compute;
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
        compute.parameters = {constants, table};
        compute_root = h->root_signature(f, compute);
        ASSERT_NE(compute_root, 0u);
        RootSignatureDesc graphics;
        graphics.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        graphics_root = h->root_signature(f, graphics);
        ASSERT_NE(graphics_root, 0u);
    }

    GuestAddr create_compute(Frame& f, const std::vector<u8>& cs) {
        (void)h->memory.write_bytes(f.shader_a(), cs.data(), cs.size());
        const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[true];
        h->zero(f.desc(), cl.size);
        (void)h->memory.write_u64(f.desc() + cl.pRootSignature, compute_root);
        (void)h->memory.write_u64(f.desc() + cl.CS_pShaderBytecode, f.shader_a());
        (void)h->memory.write_u64(f.desc() + cl.CS_BytecodeLength, cs.size());
        return h->made(f, h->com(f, h->device,
                                 DeviceSlot::CreateComputePipelineState,
                                 {f.desc(), h->iid(f, IID_ID3D12PIPELINESTATE),
                                  f.out()}));
    }

    /// vs_color and @p ps (ps_color when empty) with render target
    /// @p variant's format and state.
    GuestAddr create_graphics(Frame& f, u32 variant,
                              const std::vector<u8>& ps = {}) {
        static constexpr u32 kFormats[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R11G11B10_FLOAT,
            DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        (void)h->memory.write_bytes(f.shader_a(), g_sh_vs_color,
                                    sizeof(g_sh_vs_color));
        const std::vector<u8> pixel =
            ps.empty() ? std::vector<u8>(g_sh_ps_color,
                                         g_sh_ps_color + sizeof(g_sh_ps_color))
                       : ps;
        (void)h->memory.write_bytes(f.shader_b(), pixel.data(), pixel.size());
        (void)h->memory.write_bytes(f.names(),
                                    reinterpret_cast<const u8*>("POSITION"), 9);
        (void)h->memory.write_bytes(f.names() + 0x20,
                                    reinterpret_cast<const u8*>("COLOR"), 6);
        const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[true];
        h->zero(f.elements(), static_cast<usize>(il.size) * 2u);
        (void)h->memory.write_u64(f.elements() + il.SemanticName, f.names());
        (void)h->memory.write_u32(f.elements() + il.Format, DXGI_FORMAT_R32G32_FLOAT);
        (void)h->memory.write_u64(f.elements() + il.size + il.SemanticName,
                                  f.names() + 0x20);
        (void)h->memory.write_u32(f.elements() + il.size + il.Format,
                                  DXGI_FORMAT_R32G32B32A32_FLOAT);
        (void)h->memory.write_u32(f.elements() + il.size + il.AlignedByteOffset, 8);
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[true];
        const GuestAddr d = f.desc();
        h->zero(d, gl.size);
        (void)h->memory.write_u64(d + gl.pRootSignature, graphics_root);
        (void)h->memory.write_u64(d + gl.VS_pShaderBytecode, f.shader_a());
        (void)h->memory.write_u64(d + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
        (void)h->memory.write_u64(d + gl.PS_pShaderBytecode, f.shader_b());
        (void)h->memory.write_u64(d + gl.PS_BytecodeLength, pixel.size());
        const GuestAddr rt0 = d + gl.BlendState_RenderTarget;
        const bool blended = (variant & 8u) != 0;
        (void)h->memory.write_u32(rt0 + blend.BlendEnable, blended ? 1u : 0u);
        (void)h->memory.write_u32(rt0 + blend.SrcBlend,
                                  blended ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE);
        (void)h->memory.write_u32(rt0 + blend.DestBlend,
                                  blended ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO);
        (void)h->memory.write_u32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
        (void)h->memory.write_u32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
        (void)h->memory.write_u32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
        (void)h->memory.write_u32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
        (void)h->memory.write_u32(rt0 + blend.RenderTargetWriteMask,
                                  D3D12_COLOR_WRITE_ENABLE_ALL);
        (void)h->memory.write_u32(d + gl.SampleMask, 0xFFFFFFFFu);
        (void)h->memory.write_u32(d + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        (void)h->memory.write_u32(d + gl.RasterizerState_CullMode,
                                  (variant & 16u) != 0 ? D3D12_CULL_MODE_BACK
                                                       : D3D12_CULL_MODE_NONE);
        (void)h->memory.write_u32(d + gl.RasterizerState_DepthClipEnable, 1);
        (void)h->memory.write_u64(d + gl.InputLayout_pInputElementDescs, f.elements());
        (void)h->memory.write_u32(d + gl.InputLayout_NumElements, 2);
        (void)h->memory.write_u32(d + gl.PrimitiveTopologyType,
                                  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        (void)h->memory.write_u32(d + gl.NumRenderTargets, 1);
        (void)h->memory.write_u32(d + gl.RTVFormats, kFormats[variant % 8u]);
        (void)h->memory.write_u32(d + gl.SampleDesc_Count, 1);
        return h->made(f, h->com(f, h->device,
                                 DeviceSlot::CreateGraphicsPipelineState,
                                 {d, h->iid(f, IID_ID3D12PIPELINESTATE), f.out()}));
    }

    /// What a build produced, read under the frontend lock.
    struct Built {
        bool live{false};
        bool backend_valid{false};
        u64 digest{0};
        std::string vs, ps, cs;
        u32 threads[3]{};
        bool operator==(const Built&) const = default;
    };
    Built built(GuestAddr address) {
        Frontend& frontend = h->manager->frontend();
        FrontendLock lock(frontend);
        Built out;
        const PipelineState* pso =
            frontend.find<PipelineState>(address, Kind::PipelineState);
        if (pso == nullptr) return out;
        out.live = true;
        out.backend_valid = pso->compute ? pso->compute_pipeline != nullptr &&
                                               pso->compute_pipeline->valid()
                                         : true;
        out.digest = pso->cache_digest;
        out.vs = pso->vs.msl;
        out.ps = pso->ps.msl;
        out.cs = pso->cs.msl;
        out.threads[0] = pso->cs.threads_x;
        out.threads[1] = pso->cs.threads_y;
        out.threads[2] = pso->cs.threads_z;
        return out;
    }

    /// Run @p work(thread, frame) on kThreads threads released together.
    template <typename Work>
    void on_threads(Work work) {
        std::barrier start(static_cast<std::ptrdiff_t>(kThreads));
        std::vector<std::thread> threads;
        for (usize t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                Frame f = h->frame(1 + t);
                start.arrive_and_wait();
                work(t, f);
            });
        }
        for (auto& thread : threads) thread.join();
    }

    std::unique_ptr<Harness> h;
    GuestAddr compute_root{0};
    GuestAddr graphics_root{0};
};

// N different compute and graphics pipelines from kThreads threads at once
// come out exactly as the same descriptions built one after another.
TEST_F(D3D12ConcurrentPsoTest, ConcurrentBuildsMatchSerialBuilds) {
    constexpr u32 kPerThread = 3;
    constexpr u32 kCount = static_cast<u32>(kThreads) * kPerThread;
    const Groups groups{static_cast<u32>(std::random_device{}())};
    std::vector<std::vector<u8>> shaders, pixels;
    for (u32 i = 0; i < kCount; ++i) {
        const auto [x, y] = groups.of(i);
        shaders.push_back(cs_with_group(x, y));
        ASSERT_FALSE(shaders.back().empty());
        pixels.push_back(ps_with_chain(groups.salt + i, 16));
        ASSERT_FALSE(pixels.back().empty());
    }
    // Serial: one thread, every description.
    std::vector<GuestAddr> serial_compute(kCount), serial_graphics(kCount);
    {
        Frame f = h->frame(0);
        for (u32 i = 0; i < kCount; ++i) {
            serial_compute[i] = create_compute(f, shaders[i]);
            serial_graphics[i] = create_graphics(f, i, pixels[i]);
            ASSERT_NE(serial_compute[i], 0u) << i;
            ASSERT_NE(serial_graphics[i], 0u) << i;
        }
    }
    // Concurrent: each thread a share of both kinds, interleaved.
    std::vector<GuestAddr> compute(kCount), graphics(kCount);
    on_threads([&](usize t, Frame& f) {
        for (u32 k = 0; k < kPerThread; ++k) {
            const u32 i = static_cast<u32>(t) * kPerThread + k;
            compute[i] = create_compute(f, shaders[i]);
            graphics[i] = create_graphics(f, i, pixels[i]);
        }
    });
    for (u32 i = 0; i < kCount; ++i) {
        ASSERT_NE(compute[i], 0u) << i;
        ASSERT_NE(graphics[i], 0u) << i;
        const Built a = built(serial_compute[i]);
        const Built b = built(compute[i]);
        EXPECT_TRUE(a.live && a.backend_valid) << i;
        EXPECT_TRUE(b == a) << "compute " << i;
        const auto [x, y] = groups.of(i);
        EXPECT_EQ(b.threads[0], x) << i;
        EXPECT_EQ(b.threads[1], y) << i;
        const Built c = built(serial_graphics[i]);
        const Built d = built(graphics[i]);
        EXPECT_TRUE(c.live && !c.vs.empty() && !c.ps.empty()) << i;
        EXPECT_TRUE(d == c) << "graphics " << i;
    }
    // Every object is its own (no two creations published at one address).
    std::vector<GuestAddr> all = compute;
    all.insert(all.end(), graphics.begin(), graphics.end());
    all.insert(all.end(), serial_compute.begin(), serial_compute.end());
    all.insert(all.end(), serial_graphics.begin(), serial_graphics.end());
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
}

// Builds outside the frontend lock overlap: kThreads distinct compute
// pipelines (each a translation and a Metal compile no earlier run has
// cached) take well under the time the same number take one after another.
TEST_F(D3D12ConcurrentPsoTest, ConcurrentBuildsOverlap) {
    // Long programs (a material-sized chain), every one new to Metal.
    constexpr u32 kChain = 2000;
    const u32 seed = static_cast<u32>(std::random_device{}());
    std::vector<std::vector<u8>> serial_set, concurrent_set;
    for (u32 i = 0; i < kThreads; ++i) {
        serial_set.push_back(cs_with_chain(seed + i, kChain));
        concurrent_set.push_back(
            cs_with_chain(seed + static_cast<u32>(kThreads) + i, kChain));
        ASSERT_FALSE(serial_set.back().empty());
        ASSERT_FALSE(concurrent_set.back().empty());
    }
    using Clock = std::chrono::steady_clock;
    Frame f = h->frame(0);
    const auto s0 = Clock::now();
    for (const auto& cs : serial_set) ASSERT_NE(create_compute(f, cs), 0u);
    const double serial_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - s0).count();

    Frontend& frontend = h->manager->frontend();
    frontend.m_builds_in_flight_peak.store(0);
    std::atomic<u32> failures{0};
    const auto c0 = Clock::now();
    on_threads([&](usize t, Frame& tf) {
        if (create_compute(tf, concurrent_set[t]) == 0) failures.fetch_add(1);
    });
    const double concurrent_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
    EXPECT_EQ(failures.load(), 0u);
    const u32 peak = frontend.m_builds_in_flight_peak.load();
    std::fprintf(stderr,
                 "[d3d12-concurrent-pso] %zu compute PSOs: serial %.1f ms, "
                 "%zu threads %.1f ms (%.2fx), peak builds in flight %u\n",
                 kThreads, serial_ms, kThreads, concurrent_ms,
                 concurrent_ms > 0.0 ? serial_ms / concurrent_ms : 0.0, peak);
    EXPECT_GT(peak, 1u);
    // Only meaningful when the compiles cost something (a warm compiler
    // service answers in microseconds).
    if (serial_ms > 100.0) EXPECT_LT(concurrent_ms, serial_ms * 0.75);
}

// While a worker builds a long pipeline, another thread's D3D12 calls (the
// render thread's, in a game) do not wait for it.
TEST_F(D3D12ConcurrentPsoTest, OtherCallsDoNotWaitForABuild) {
    Frontend& frontend = h->manager->frontend();
    const std::vector<u8> cs =
        cs_with_chain(static_cast<u32>(std::random_device{}()), 2500);
    ASSERT_FALSE(cs.empty());
    std::atomic<bool> finished{false};
    double build_ms = 0.0;
    std::thread worker([&] {
        Frame f = h->frame(1);
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_NE(create_compute(f, cs), 0u);
        build_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
        finished = true;
    });
    while (frontend.m_builds_in_flight.load() == 0 && !finished.load()) {
        std::this_thread::yield();
    }
    Frame f = h->frame(0);
    double worst_ms = 0.0;
    u32 calls = 0;
    while (!finished.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(h->com(f, h->device, DeviceSlot::GetNodeCount, {}), 1u);
        worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
        ++calls;
    }
    worker.join();
    std::fprintf(stderr,
                 "[d3d12-concurrent-pso] build %.1f ms; %u calls meanwhile, "
                 "slowest %.3f ms\n",
                 build_ms, calls, worst_ms);
    EXPECT_GT(calls, 0u);
    // Under the old device-wide lock every call waited the whole build.
    if (build_ms > 50.0) EXPECT_LT(worst_ms, build_ms / 4.0);
}

// A graphics pipeline's Metal render pipeline is formed at its first draw,
// under the frontend lock (pipeline_variant). Its shader functions were
// compiled when it was created, on the creating thread, so that draw only
// forms the pipeline state.
TEST_F(D3D12ConcurrentPsoTest, FirstDrawFindsItsShadersCompiled) {
    Frontend& frontend = h->manager->frontend();
    const u32 seed = static_cast<u32>(std::random_device{}());
    Frame f = h->frame(0);
    constexpr u32 kPipelines = 4;
    double create_ms = 0.0;
    double draw_ms = 0.0;
    for (u32 i = 0; i < kPipelines; ++i) {
        const std::vector<u8> ps = ps_with_chain(seed + i, 2000);
        ASSERT_FALSE(ps.empty());
        const auto t0 = std::chrono::steady_clock::now();
        const GuestAddr address = create_graphics(f, i, ps);
        const auto t1 = std::chrono::steady_clock::now();
        ASSERT_NE(address, 0u);
        FrontendLock lock(frontend);
        PipelineState* pso =
            frontend.find<PipelineState>(address, Kind::PipelineState);
        ASSERT_NE(pso, nullptr);
        std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
        strides[0] = 24;
        const auto t2 = std::chrono::steady_clock::now();
        EXPECT_NE(frontend.pipeline_variant(*pso, strides, false), nullptr);
        const auto t3 = std::chrono::steady_clock::now();
        create_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        draw_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();
    }
    std::fprintf(stderr,
                 "[d3d12-concurrent-pso] graphics PSO: create %.1f ms, first "
                 "draw's pipeline (under the lock) %.1f ms, mean of %u\n",
                 create_ms / kPipelines, draw_ms / kPipelines, kPipelines);
}

// CreateRootSignature only waits for the lock; it is not slow itself, even
// with many live objects (its same-blob lookup walks them).
TEST_F(D3D12ConcurrentPsoTest, RootSignatureCreationIsCheap) {
    Frame f = h->frame(0);
    constexpr u32 kLive = 4000;
    for (u32 i = 0; i < kLive; ++i) {
        ASSERT_NE(h->made(f, h->com(f, h->device, DeviceSlot::CreateFence,
                                    {0, 0, h->iid(f, IID_ID3D12FENCE), f.out()})),
                  0u);
    }
    constexpr u32 kRoots = 256;
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRoots; ++i) {
        RootSignatureDesc rs;
        RootParameter constants;
        constants.parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.num_32bit_values = 1 + i % 32;
        constants.shader_register = i / 32;
        rs.parameters = {constants};
        ASSERT_NE(h->root_signature(f, rs), 0u) << i;
    }
    const double each_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count() /
                           kRoots;
    std::fprintf(stderr,
                 "[d3d12-concurrent-pso] CreateRootSignature with %u live "
                 "objects: %.3f ms each\n",
                 kLive, each_ms);
    EXPECT_LT(each_ms, 5.0);
}

// A thread that holds the frontend lock more than once (a guest callback
// made under it re-entered the frontend) builds with the lock held: no other
// thread can enter meanwhile, and the build still succeeds.
TEST_F(D3D12ConcurrentPsoTest, ReentrantHolderBuildsUnderTheLock) {
    Frontend& frontend = h->manager->frontend();
    const auto [x, y] = Groups{7u}.of(0);
    const std::vector<u8> cs = cs_with_group(x, y);
    Frame f = h->frame(0);
    std::atomic<bool> entered{false};
    std::thread other;
    GuestAddr pso = 0;
    {
        FrontendLock outer(frontend);  // depth 1 here, 2 inside method()
        other = std::thread([&] {
            Frame tf = h->frame(1);
            // Blocks on the frontend lock until the outer frame lets go.
            (void)h->com(tf, h->device, DeviceSlot::GetNodeCount, {});
            entered = true;
        });
        pso = create_compute(f, cs);
        EXPECT_FALSE(entered.load()) << "the lock was released while held twice";
    }
    other.join();
    EXPECT_TRUE(entered.load());
    EXPECT_NE(pso, 0u);
    EXPECT_TRUE(built(pso).backend_valid);
}

}  // namespace
}  // namespace vitreo::winapi
