// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_pso_refusal.cpp
/// @brief Graphics pipeline descriptions with fields the pipeline does not
/// use, and the record a refused pipeline leaves.
///
/// Learn (D3D12_BLEND_DESC::IndependentBlendEnable): FALSE uses only
/// RenderTarget[0]; RenderTarget[1..7] are ignored. Measured on WARP and an
/// RTX 4080 SUPER (tools/oracle_probe/d3d12_a71_pso_unused_state_probe): an
/// invalid RenderTarget[1] is S_OK without independent blending and
/// E_INVALIDARG with it, whatever NumRenderTargets is; a depth-only
/// description (NumRenderTargets 0, D16 depth, IndependentBlendEnable,
/// FrontCounterClockwise with CullMode NONE, MultisampleEnable over one
/// sample) is S_OK. VITREO_D3D12_DUMP_REFUSED writes a refused pipeline's
/// description, shaders and reason.

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
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

constexpr GuestAddr kBase = 0x100000;
constexpr GuestAddr kStack = kBase + 0x3F000;
constexpr GuestAddr kIid = kBase + 0x100;
constexpr GuestAddr kOut = kBase + 0x200;
constexpr GuestAddr kDesc = kBase + 0x400;
constexpr GuestAddr kElements = kBase + 0xC00;
constexpr GuestAddr kNames = kBase + 0xD00;
constexpr GuestAddr kVs = kBase + 0x2000;
constexpr GuestAddr kPs = kBase + 0x20000;
constexpr GuestAddr kBlob = kBase + 0x38000;

class D3D12PsoRefusalTest : public ::testing::Test {
public:  // the refusal cases edit the description through it
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        ::setenv("VITREO_D3D12", "1", 1);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
        ASSERT_TRUE(memory.map_region(kBase, 0x40000, MemoryProtection::ReadWrite).is_ok());
        manager = std::make_unique<D3D12Manager>(memory, true,
                                                 graphics::MetalDevice::instance());
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
        device = made(call("D3D12CreateDevice",
                           {0, D3D_FEATURE_LEVEL_11_0, iid(IID_ID3D12DEVICE), kOut}));
        ASSERT_NE(device, 0u);
        RootSignatureDesc rs;
        rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        std::vector<u8> blob;
        std::string error;
        ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK)
            << error;
        (void)memory.write_bytes(kBlob, blob.data(), blob.size());
        root = made(com(device, DeviceSlot::CreateRootSignature,
                        {0, kBlob, blob.size(), iid(IID_ID3D12ROOTSIGNATURE), kOut}));
        ASSERT_NE(root, 0u);
    }

    u64 call(const std::string& name, const std::vector<u64>& args) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) return ~0ull;
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
        for (usize i = 0; i < args.size(); ++i) {
            if (i < 4) {
                *regs[i] = args[i];
            } else {
                (void)memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        found->second->handler(m);
        return cpu.gpr.rax;
    }

    u64 com(GuestAddr object, usize slot, std::vector<u64> args) {
        const u64 vtable = memory.read_u64(object).value();
        const u64 thunk = memory.read_u64(vtable + slot * 8).value();
        const auto name = by_thunk.find(thunk);
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), object);
        return call(name->second, args);
    }

    GuestAddr iid(const GuidBytes& value) {
        (void)memory.write_bytes(kIid, value.data(), value.size());
        return kIid;
    }

    GuestAddr made(u64 hr) { return hr == S_OK ? memory.read_u64(kOut).value() : 0; }

    /// vs_color + ps_color into one RGBA8 target, every blend description
    /// valid and blending off (d3d12_probe_common.h gfx_desc).
    void color_desc() {
        (void)memory.write_bytes(kVs, g_sh_vs_color, sizeof(g_sh_vs_color));
        (void)memory.write_bytes(kPs, g_sh_ps_color, sizeof(g_sh_ps_color));
        (void)memory.write_bytes(kNames, reinterpret_cast<const u8*>("POSITION"), 9);
        (void)memory.write_bytes(kNames + 0x20, reinterpret_cast<const u8*>("COLOR"), 6);
        const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[true];
        const std::vector<u8> zero(0x800, 0);
        (void)memory.write_bytes(kElements, zero.data(), static_cast<usize>(il.size) * 2u);
        (void)memory.write_u64(kElements + il.SemanticName, kNames);
        (void)memory.write_u32(kElements + il.Format, DXGI_FORMAT_R32G32_FLOAT);
        (void)memory.write_u64(kElements + il.size + il.SemanticName, kNames + 0x20);
        (void)memory.write_u32(kElements + il.size + il.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        (void)memory.write_u32(kElements + il.size + il.AlignedByteOffset, 8);
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[true];
        (void)memory.write_bytes(kDesc, zero.data(), gl.size);
        (void)memory.write_u64(kDesc + gl.pRootSignature, root);
        (void)memory.write_u64(kDesc + gl.VS_pShaderBytecode, kVs);
        (void)memory.write_u64(kDesc + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
        (void)memory.write_u64(kDesc + gl.PS_pShaderBytecode, kPs);
        (void)memory.write_u64(kDesc + gl.PS_BytecodeLength, sizeof(g_sh_ps_color));
        for (u32 index = 0; index < 8; ++index) {
            const GuestAddr rt = kDesc + gl.BlendState_RenderTarget + index * blend.size;
            (void)memory.write_u32(rt + blend.SrcBlend, D3D12_BLEND_ONE);
            (void)memory.write_u32(rt + blend.DestBlend, D3D12_BLEND_ZERO);
            (void)memory.write_u32(rt + blend.BlendOp, D3D12_BLEND_OP_ADD);
            (void)memory.write_u32(rt + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
            (void)memory.write_u32(rt + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
            (void)memory.write_u32(rt + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
            (void)memory.write_u32(rt + blend.LogicOp, D3D12_LOGIC_OP_NOOP);
            (void)memory.write_u8(rt + blend.RenderTargetWriteMask,
                                  D3D12_COLOR_WRITE_ENABLE_ALL);
        }
        (void)memory.write_u32(kDesc + gl.SampleMask, 0xFFFFFFFFu);
        (void)memory.write_u32(kDesc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        (void)memory.write_u32(kDesc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
        (void)memory.write_u32(kDesc + gl.RasterizerState_DepthClipEnable, 1);
        (void)memory.write_u32(kDesc + gl.DepthStencilState_DepthWriteMask,
                               D3D12_DEPTH_WRITE_MASK_ALL);
        (void)memory.write_u32(kDesc + gl.DepthStencilState_DepthFunc,
                               D3D12_COMPARISON_FUNC_LESS);
        (void)memory.write_u64(kDesc + gl.InputLayout_pInputElementDescs, kElements);
        (void)memory.write_u32(kDesc + gl.InputLayout_NumElements, 2);
        (void)memory.write_u32(kDesc + gl.PrimitiveTopologyType,
                               D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        (void)memory.write_u32(kDesc + gl.NumRenderTargets, 1);
        (void)memory.write_u32(kDesc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
        (void)memory.write_u32(kDesc + gl.SampleDesc_Count, 1);
    }

    /// The depth-only pass: no pixel shader, no render target, D16 depth
    /// written ALWAYS, FrontCounterClockwise with CullMode NONE,
    /// MultisampleEnable over one sample, IndependentBlendEnable.
    void depth_only_desc() {
        color_desc();
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        (void)memory.write_u64(kDesc + gl.PS_pShaderBytecode, 0);
        (void)memory.write_u64(kDesc + gl.PS_BytecodeLength, 0);
        (void)memory.write_u32(kDesc + gl.NumRenderTargets, 0);
        (void)memory.write_u32(kDesc + gl.RTVFormats, DXGI_FORMAT_UNKNOWN);
        (void)memory.write_u32(kDesc + gl.DSVFormat, DXGI_FORMAT_D16_UNORM);
        (void)memory.write_u32(kDesc + gl.DepthStencilState_DepthEnable, 1);
        (void)memory.write_u32(kDesc + gl.DepthStencilState_DepthFunc,
                               D3D12_COMPARISON_FUNC_ALWAYS);
        (void)memory.write_u32(kDesc + gl.RasterizerState_FrontCounterClockwise, 1);
        (void)memory.write_u32(kDesc + gl.RasterizerState_MultisampleEnable, 1);
        independent(true);
    }

    void independent(bool on) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        (void)memory.write_u32(kDesc + gl.BlendState_IndependentBlendEnable, on ? 1u : 0u);
    }

    /// RenderTarget[@p index]: BlendEnable with SrcBlend 0.
    void bad_blend(u32 index) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[true];
        const GuestAddr rt = kDesc + gl.BlendState_RenderTarget + index * blend.size;
        (void)memory.write_u32(rt + blend.BlendEnable, 1);
        (void)memory.write_u32(rt + blend.SrcBlend, 0);
    }

    /// RenderTarget[@p index]: BlendEnable with SrcBlendAlpha SRC_COLOR.
    void colour_alpha_factor(u32 index) {
        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[true];
        const GuestAddr rt = kDesc + gl.BlendState_RenderTarget + index * blend.size;
        (void)memory.write_u32(rt + blend.BlendEnable, 1);
        (void)memory.write_u32(rt + blend.SrcBlendAlpha, D3D12_BLEND_SRC_COLOR);
    }

    u32 create() {
        (void)memory.write_u64(kOut, 0);
        return static_cast<u32>(com(device, DeviceSlot::CreateGraphicsPipelineState,
                                    {kDesc, iid(IID_ID3D12PIPELINESTATE), kOut}));
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    GuestAddr device{0};
    GuestAddr root{0};
};

// a71 "NumRT 1, not independent, RT[1] BlendEnable SrcBlend 0": S_OK;
// "NumRT 1, independent, RT[1] BlendEnable SrcBlend 0": E_INVALIDARG.
TEST_F(D3D12PsoRefusalTest, RenderTargets1To7AreIgnoredWithoutIndependentBlend) {
    color_desc();
    bad_blend(1);
    independent(false);
    EXPECT_EQ(create(), S_OK);
    independent(true);
    EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG));

    color_desc();
    for (u32 index = 1; index < 8; ++index) bad_blend(index);
    independent(false);
    EXPECT_EQ(create(), S_OK);  // a71 "RT[1..7] BlendEnable SrcBlend 0"

    color_desc();
    colour_alpha_factor(1);
    independent(false);
    EXPECT_EQ(create(), S_OK);  // a71 "not independent, RT[1] SrcBlendAlpha SRC_COLOR"
    independent(true);
    EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG));
}

// RenderTarget[0] is used either way: it is validated with and without
// independent blending (a71 "RT[0] BlendEnable SrcBlend 0" rows).
TEST_F(D3D12PsoRefusalTest, RenderTarget0IsAlwaysValidated) {
    color_desc();
    bad_blend(0);
    independent(false);
    EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG));
    independent(true);
    EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG));
}

// a71: the depth-only description is S_OK, with or without independent
// blending; with it, a target past NumRenderTargets 0 is still validated.
TEST_F(D3D12PsoRefusalTest, DepthOnlyDescription) {
    depth_only_desc();
    EXPECT_EQ(create(), S_OK);
    depth_only_desc();
    independent(false);
    EXPECT_EQ(create(), S_OK);
    depth_only_desc();
    colour_alpha_factor(3);
    EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG));  // "RT[3] SrcBlendAlpha SRC_COLOR"
}

// a71 "NumRT 1 RGBA16F, DSV UNKNOWN, independent, FrontCCW, MSAA enable at 1
// sample": S_OK.
TEST_F(D3D12PsoRefusalTest, Rgba16FloatTargetWithFrontCcwAndMultisampleEnable) {
    color_desc();
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
    (void)memory.write_u32(kDesc + gl.RTVFormats, DXGI_FORMAT_R16G16B16A16_FLOAT);
    (void)memory.write_u32(kDesc + gl.RasterizerState_FrontCounterClockwise, 1);
    (void)memory.write_u32(kDesc + gl.RasterizerState_MultisampleEnable, 1);
    independent(true);
    EXPECT_EQ(create(), S_OK);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// VITREO_D3D12_DUMP_REFUSED: one refused pipeline leaves its description,
// root signature, each shader blob and the reason it was refused for.
TEST_F(D3D12PsoRefusalTest, DumpRefusedWritesDescriptionShadersAndReason) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("vitreo-d3d12-refused-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    ::setenv("VITREO_D3D12_DUMP_REFUSED", dir.c_str(), 1);
    color_desc();
    bad_blend(5);
    independent(true);
    const u32 hr = create();
    ::unsetenv("VITREO_D3D12_DUMP_REFUSED");
    ASSERT_EQ(hr, static_cast<u32>(E_INVALIDARG));
    std::string reason;
    usize desc_bytes = 0;
    bool vs = false;
    bool ps = false;
    bool rootsig = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".reason.txt")) reason = read_text(entry.path());
        if (name.ends_with(".desc.bin")) desc_bytes = fs::file_size(entry.path());
        vs |= name.find(".vs-") != std::string::npos && name.ends_with(".dxbc");
        ps |= name.find(".ps-") != std::string::npos && name.ends_with(".dxbc");
        rootsig |= name.find(".rootsig-") != std::string::npos;
    }
    fs::remove_all(dir);
    EXPECT_NE(reason.find("hr: 0x80070057"), std::string::npos) << reason;
    EXPECT_NE(reason.find("RenderTarget[5] blend factors"), std::string::npos) << reason;
    EXPECT_EQ(desc_bytes, layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].size);
    EXPECT_TRUE(vs);
    EXPECT_TRUE(ps);
    EXPECT_TRUE(rootsig);
}

// Every refusal is attributed: the record a pipeline creation keeps is
// empty only while nothing refused it.
TEST_F(D3D12PsoRefusalTest, EveryRefusalPathRecordsAReason) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("vitreo-d3d12-refused-all-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    ::setenv("VITREO_D3D12_DUMP_REFUSED", dir.c_str(), 1);
    const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
    struct Case {
        const char* what;
        void (*edit)(D3D12PsoRefusalTest&);
    };
    const Case cases[] = {
        {"NumRenderTargets 9",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true]
                                                  .NumRenderTargets,
                                      9);
         }},
        {"RTVFormats[1] past NumRenderTargets",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(
                 kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].RTVFormats + 4,
                 DXGI_FORMAT_R8G8B8A8_UNORM);
         }},
        {"no input layout",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(
                 kDesc +
                     layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].InputLayout_NumElements,
                 0);
         }},
        {"topology type PATCH",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(
                 kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].PrimitiveTopologyType,
                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
         }},
        {"SampleDesc 3",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(
                 kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].SampleDesc_Count, 3);
         }},
        {"NodeMask 2",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u32(
                 kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].NodeMask, 2);
         }},
        {"PS bytecode in the VS slot",
         [](D3D12PsoRefusalTest& t) {
             const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
             (void)t.memory.write_u64(kDesc + l.VS_pShaderBytecode, kPs);
             (void)t.memory.write_u64(kDesc + l.VS_BytecodeLength, sizeof(g_sh_ps_color));
         }},
        {"NULL root signature",
         [](D3D12PsoRefusalTest& t) {
             (void)t.memory.write_u64(
                 kDesc + layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true].pRootSignature, 0);
         }},
    };
    (void)gl;
    for (const Case& c : cases) {
        color_desc();
        c.edit(*this);
        EXPECT_EQ(create(), static_cast<u32>(E_INVALIDARG)) << c.what;
    }
    ::unsetenv("VITREO_D3D12_DUMP_REFUSED");
    usize reasons = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.path().filename().string().ends_with(".reason.txt")) continue;
        ++reasons;
        const std::string text = read_text(entry.path());
        EXPECT_EQ(text.find("no refusal reason was recorded"), std::string::npos) << text;
    }
    fs::remove_all(dir);
    EXPECT_EQ(reasons, std::size(cases));
}

}  // namespace
}  // namespace vitreo::winapi
