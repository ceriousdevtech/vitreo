// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d10.cpp
/// @brief d3d10.dll / d3d10_1.dll / d3d10core.dll -- the module surface: export
/// names, ordinals and x86 cleanup, the state-block mask functions, the
/// runtime version and the core thunks, and the VITREO_D3D10=0 opt-out.
///
/// Expected values: dumpbin of the real DLLs and SDK import libraries on
/// Windows, and tools/oracle_probe/d3d10_contract_probe64.expected. The
/// device and its objects are tested beside D3D11's (test_d3d11.cpp,
/// D3D10DeviceTest), whose fixture drives them through their vtables.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>

namespace vitreo::winapi {
namespace {

/// The module surface touches nothing but `available()` and the device name.
class FakeBackend final : public graphics::GraphicsBackendDevice {
public:
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override {
        return "Fake";
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc&, const void*) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D>
    create_texture_2d(const graphics::BackendTexture2DDesc&, const void*,
                      unsigned) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(
        const graphics::BackendComputePipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(
        const graphics::BackendRenderPassDesc&) override {
        return nullptr;
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall&) override {
        return false;
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*,
                                   const graphics::BackendBuffer*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d(
        graphics::BackendTexture2D*,
        const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override {}
};

class D3D10ModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK;
        marshaler.attach_cpu(cpu);
    }

    void set_arg(usize index, u64 value) {
        switch (index) {  // x64 fastcall: RCX, RDX, R8, R9, then the stack.
        case 0: cpu.gpr.rcx = value; break;
        case 1: cpu.gpr.rdx = value; break;
        case 2: cpu.gpr.r8 = value; break;
        case 3: cpu.gpr.r9 = value; break;
        default:
            (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (index - 4) * 8, value);
            break;
        }
    }

    /// Call export @p name of @p exports with @p args.
    u64 call(const DllExports& exports, const char* name,
             std::initializer_list<u64> args) {
        usize index = 0;
        for (const u64 value : args) set_arg(index++, value);
        for (const auto& entry : exports.entries) {
            if (entry.name == name) {
                cpu.gpr.rax = 0xDEADBEEFull;
                call_export(entry, marshaler);
                return cpu.gpr.rax;
            }
        }
        ADD_FAILURE() << name << " is not exported";
        return 0;
    }

    [[nodiscard]] const ExportEntry* find(const DllExports& exports,
                                          const char* name) const {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    std::array<u8, 76> read_mask(GuestAddr at) {
        std::array<u8, 76> bytes{};
        EXPECT_TRUE(memory.read_bytes(at, bytes.data(), bytes.size()).is_ok());
        return bytes;
    }

    static constexpr GuestAddr DATA_BASE = 0x40000;
    static constexpr GuestAddr STACK = DATA_BASE + 0x3000;
    static constexpr GuestAddr MASK_A = DATA_BASE + 0x100;
    static constexpr GuestAddr MASK_B = DATA_BASE + 0x200;
    static constexpr GuestAddr MASK_R = DATA_BASE + 0x300;
    static constexpr GuestAddr OUT = DATA_BASE + 0x400;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    FakeBackend backend;
    DxgiManager dxgi{memory, true, dxgi::AdapterInfo{.description = "Fake"}};
    D3D11Manager manager{memory, true, backend, dxgi};
};

// dumpbin /exports of System32 and SysWOW64 d3d10.dll / d3d10_1.dll (the
// same 29 names at the same ordinals, alphabetical), and the x86 cleanup of
// each from the SDK import libraries' decorated names (_Name@N).
struct ExpectedExport {
    const char* name;
    u16 ordinal;
    u16 stdcall_bytes;
};
constexpr std::array<ExpectedExport, 29> kD3D10Exports{{
    {"D3D10CompileEffectFromMemory", 1, 36},
    {"D3D10CompileShader", 2, 40},
    {"D3D10CreateBlob", 3, 8},
    {"D3D10CreateDevice", 4, 24},
    {"D3D10CreateDeviceAndSwapChain", 5, 32},
    {"D3D10CreateEffectFromMemory", 6, 24},
    {"D3D10CreateEffectPoolFromMemory", 7, 20},
    {"D3D10CreateStateBlock", 8, 12},
    {"D3D10DisassembleEffect", 9, 12},
    {"D3D10DisassembleShader", 10, 20},
    {"D3D10GetGeometryShaderProfile", 11, 4},
    {"D3D10GetInputAndOutputSignatureBlob", 12, 12},
    {"D3D10GetInputSignatureBlob", 13, 12},
    {"D3D10GetOutputSignatureBlob", 14, 12},
    {"D3D10GetPixelShaderProfile", 15, 4},
    {"D3D10GetShaderDebugInfo", 16, 12},
    {"D3D10GetVersion", 17, 0},
    {"D3D10GetVertexShaderProfile", 18, 4},
    {"D3D10PreprocessShader", 19, 28},
    {"D3D10ReflectShader", 20, 12},
    {"D3D10RegisterLayers", 21, 0},
    {"D3D10StateBlockMaskDifference", 22, 12},
    {"D3D10StateBlockMaskDisableAll", 23, 4},
    {"D3D10StateBlockMaskDisableCapture", 24, 16},
    {"D3D10StateBlockMaskEnableAll", 25, 4},
    {"D3D10StateBlockMaskEnableCapture", 26, 16},
    {"D3D10StateBlockMaskGetSetting", 27, 12},
    {"D3D10StateBlockMaskIntersect", 28, 12},
    {"D3D10StateBlockMaskUnion", 29, 12},
}};

TEST_F(D3D10ModuleTest, ExportTablesAreTheWindowsModules) {
    const auto d3d10 = create_d3d10_thunks(manager);
    const auto d3d10_1 = create_d3d10_1_thunks(manager);
    ASSERT_EQ(d3d10.entries.size(), 29u);
    ASSERT_EQ(d3d10_1.entries.size(), 29u);
    for (const ExpectedExport& expected : kD3D10Exports) {
        const ExportEntry* entry = find(d3d10, expected.name);
        ASSERT_NE(entry, nullptr) << expected.name;
        EXPECT_EQ(entry->ordinal, expected.ordinal) << expected.name;
        EXPECT_EQ(entry->stdcall_bytes, expected.stdcall_bytes)
            << expected.name;
        // d3d10_1.dll: the same, with the `1` creation pair at 4 and 5
        // (_D3D10CreateDevice1@28, _D3D10CreateDeviceAndSwapChain1@36).
        std::string name = expected.name;
        u16 bytes = expected.stdcall_bytes;
        if (name == "D3D10CreateDevice") {
            name = "D3D10CreateDevice1";
            bytes = 28;
        } else if (name == "D3D10CreateDeviceAndSwapChain") {
            name = "D3D10CreateDeviceAndSwapChain1";
            bytes = 36;
        }
        const ExportEntry* entry1 = find(d3d10_1, name.c_str());
        ASSERT_NE(entry1, nullptr) << name;
        EXPECT_EQ(entry1->ordinal, expected.ordinal) << name;
        EXPECT_EQ(entry1->stdcall_bytes, bytes) << name;
    }
    // Neither module exports the other's creation functions (measured:
    // GetProcAddress NULL).
    EXPECT_EQ(find(d3d10, "D3D10CreateDevice1"), nullptr);
    EXPECT_EQ(find(d3d10_1, "D3D10CreateDevice"), nullptr);

    // d3d10core.dll / d3d10_1core.dll: 40 exports each.
    const auto core = create_d3d10core_thunks(false);
    const auto core1 = create_d3d10core_thunks(true);
    EXPECT_EQ(core.entries.size(), 40u);
    EXPECT_EQ(core1.entries.size(), 40u);
    const std::array<ExpectedExport, 8> core_samples{{
        {"D3D10CoreCreateDevice", 14, 20},
        {"D3D10CoreGetSupportedVersions", 15, 12},
        {"D3D10CoreGetVersion", 16, 0},
        {"D3D10CoreRegisterLayers", 17, 0},
        {"D3DKMTCloseAdapter", 1, 4},
        {"D3DKMTQueryAdapterInfo", 7, 4},
        {"OpenAdapter10", 12, 4},
        {"D3DKMTWaitForVerticalBlankEvent", 40, 4},
    }};
    for (const ExpectedExport& expected : core_samples) {
        const ExportEntry* entry = find(core, expected.name);
        ASSERT_NE(entry, nullptr) << expected.name;
        EXPECT_EQ(entry->ordinal, expected.ordinal) << expected.name;
        EXPECT_EQ(entry->stdcall_bytes, expected.stdcall_bytes)
            << expected.name;
    }
    const ExportEntry* create1 = find(core1, "D3D10CoreCreateDevice1");
    ASSERT_NE(create1, nullptr);
    EXPECT_EQ(create1->ordinal, 14u);
    EXPECT_EQ(create1->stdcall_bytes, 28u);  // "ret 1Ch"
    std::array<bool, 41> ordinals{};
    for (const auto& entry : core.entries) {
        ASSERT_GE(entry.ordinal, 1u);
        ASSERT_LE(entry.ordinal, 40u);
        EXPECT_FALSE(ordinals[entry.ordinal]) << "ordinal " << entry.ordinal;
        ordinals[entry.ordinal] = true;
    }
}

TEST_F(D3D10ModuleTest, VersionRegisterLayersAndTheCoreThunks) {
    const auto d3d10 = create_d3d10_thunks(manager);
    // d3d10core!D3D10CoreGetVersion: "mov rax,0A000100041770h".
    EXPECT_EQ(call(d3d10, "D3D10GetVersion", {}), 0x000A000100041770ull);
    EXPECT_EQ(call(d3d10, "D3D10RegisterLayers", {}), 0x80004001u);
    const auto core = create_d3d10core_thunks(false);
    EXPECT_EQ(call(core, "D3D10CoreCreateDevice", {0}), 0x80004001u);
    EXPECT_EQ(call(core, "D3D10CoreGetVersion", {}), 0x000A000100041770ull);
    EXPECT_EQ(call(core, "D3DKMTCloseAdapter", {0}), 0u);
    EXPECT_EQ(call(core, "D3DKMTOpenResource", {OUT}), 0xC000000Du);
    // D3DKMTGetDeviceState: type 1 answers D3DKMT_DEVICEEXECUTION_ACTIVE,
    // an unknown type STATUS_UNSUCCESSFUL (measured).
    ASSERT_TRUE(memory.write_u32(OUT + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(OUT + 8, 0).is_ok());
    EXPECT_EQ(call(core, "D3DKMTGetDeviceState", {OUT}), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 8).value(), 1u);
    ASSERT_TRUE(memory.write_u32(OUT + 4, 7).is_ok());
    EXPECT_EQ(call(core, "D3DKMTGetDeviceState", {OUT}), 0xC0000001u);
}

// d3d10_contract_probe64.expected [sb].
TEST_F(D3D10ModuleTest, StateBlockMaskFunctionsMatchWindows) {
    const auto d3d10 = create_d3d10_thunks(manager);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableAll", {MASK_A}), 0u);
    for (const u8 byte : read_mask(MASK_A)) EXPECT_EQ(byte, 0xFFu);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskDisableAll", {MASK_A}), 0u);
    for (const u8 byte : read_mask(MASK_A)) EXPECT_EQ(byte, 0u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableAll", {0}), 0x80070057u);

    // "enable(type 6, 3, 5)": VS samplers 3..7 -> byte 1 = 0xf8.
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 6, 3, 5}),
              0u);
    EXPECT_EQ(read_mask(MASK_A)[1], 0xF8u);
    // One past the last entry, a zero count, type 0 and 25: E_INVALIDARG,
    // nothing written.
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 6, 16, 1}),
              0x80070057u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 7, 127, 2}),
              0x80070057u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 7, 0, 0}),
              0x80070057u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 0, 0, 1}),
              0x80070057u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 25, 0, 1}),
              0x80070057u);
    // VS constant buffer 13 -> byte 20 bit 5; 14 is past the 14 slots.
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 8, 13, 1}),
              0u);
    EXPECT_EQ(read_mask(MASK_A)[20], 0x20u);
    // Each single state's byte (type 1 SO -> 74, 24 predication -> 75).
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 1, 0, 1}),
              0u);
    EXPECT_EQ(read_mask(MASK_A)[74], 1u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableCapture",
                   {MASK_A, 1, 0, 2}),
              0x80070057u);

    // GetSetting answers the raw bit: "get(PS SRV 7)=128 ... get(17)=2".
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskEnableAll", {MASK_A}), 0u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskDisableCapture",
                   {MASK_A, 15, 8, 9}),
              0u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskGetSetting", {MASK_A, 15, 7}),
              128u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskGetSetting", {MASK_A, 15, 8}),
              0u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskGetSetting", {MASK_A, 15, 17}),
              2u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskGetSetting", {MASK_A, 15, 200}),
              0u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskGetSetting", {0, 5, 0}), 0u);

    // Union / Intersect / Difference: OR / AND / XOR.
    (void)call(d3d10, "D3D10StateBlockMaskDisableAll", {MASK_A});
    (void)call(d3d10, "D3D10StateBlockMaskDisableAll", {MASK_B});
    (void)call(d3d10, "D3D10StateBlockMaskEnableCapture", {MASK_A, 5, 0, 1});
    (void)call(d3d10, "D3D10StateBlockMaskEnableCapture", {MASK_A, 14, 0, 4});
    (void)call(d3d10, "D3D10StateBlockMaskEnableCapture", {MASK_B, 14, 2, 4});
    (void)call(d3d10, "D3D10StateBlockMaskEnableCapture", {MASK_B, 24, 0, 1});
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskUnion", {MASK_A, MASK_B, MASK_R}),
              0u);
    auto result = read_mask(MASK_R);
    EXPECT_EQ(result[0], 0x01u);
    EXPECT_EQ(result[43], 0x3Fu);
    EXPECT_EQ(result[75], 0x01u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskIntersect",
                   {MASK_A, MASK_B, MASK_R}),
              0u);
    result = read_mask(MASK_R);
    EXPECT_EQ(result[0], 0x00u);
    EXPECT_EQ(result[43], 0x0Cu);
    EXPECT_EQ(result[75], 0x00u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskDifference",
                   {MASK_A, MASK_B, MASK_R}),
              0u);
    result = read_mask(MASK_R);
    EXPECT_EQ(result[0], 0x01u);
    EXPECT_EQ(result[43], 0x33u);
    EXPECT_EQ(result[75], 0x01u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskUnion", {0, MASK_B, MASK_R}),
              0x80070057u);
    EXPECT_EQ(call(d3d10, "D3D10StateBlockMaskDifference", {MASK_A, MASK_B, 0}),
              0x80070057u);
}

// The fail-loudly exports NULL their out-parameters and answer E_NOTIMPL.
TEST_F(D3D10ModuleTest, UnprovidedExportsFailLoudly) {
    const auto d3d10 = create_d3d10_thunks(manager);
    ASSERT_TRUE(memory.write_u64(OUT, 0xDEADBEEFull).is_ok());
    EXPECT_EQ(call(d3d10, "D3D10ReflectShader", {DATA_BASE, 16, OUT}),
              0x80004001u);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    ASSERT_TRUE(memory.write_u64(OUT, 0xDEADBEEFull).is_ok());
    EXPECT_EQ(call(d3d10, "D3D10CreateEffectFromMemory",
                   {DATA_BASE, 16, 0, 0, 0, OUT}),
              0x80004001u);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
}

// Device creation is on by default; VITREO_D3D10=0 turns it off, and then
// D3D10CreateDevice answers DXGI_ERROR_UNSUPPORTED -- what a machine with no
// D3D10-capable adapter answers -- after the parameter rules. The CTest
// vitreo_d3d10_disabled_tests runs this with the variable set.
TEST_F(D3D10ModuleTest, OptOutRefusesDevicesAsUnsupported) {
    const auto d3d10 = create_d3d10_thunks(manager);
    if (d3d10::d3d10_enabled()) {
        GTEST_SKIP() << "VITREO_D3D10=0 is not set in this environment";
    }
    ASSERT_TRUE(memory.write_u64(OUT, 0xDEADBEEFull).is_ok());
    EXPECT_EQ(call(d3d10, "D3D10CreateDevice", {0, 0, 0, 0, 29, OUT}),
              0x887A0004u);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    // The SDK version is still checked first (measured E_INVALIDARG).
    EXPECT_EQ(call(d3d10, "D3D10CreateDevice", {0, 0, 0, 0, 28, OUT}),
              0x80070057u);
}

} // namespace
} // namespace vitreo::winapi
