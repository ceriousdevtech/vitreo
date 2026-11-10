/// @file test_d3d9_thunks.cpp
/// @brief Export-thunk table tests.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST_F(D3D9ManagerTest, ThunksCoverEveryVtableSlotOfEveryInterface) {
    // The eight non-PIX entry points of d3d9_exports_probe and the seven
    // D3DPERF_* exports.
    usize expected = 15;
    for (usize index = 0; index < D3D9_INTERFACE_COUNT; ++index) {
        expected += d3d9_interface_method_count(
            static_cast<D3D9InterfaceId>(index));
    }
    EXPECT_EQ(exports_.entries.size(), expected);

    // Every export name must be unique: two slots sharing a name would make
    // the registry hand the same thunk to both vtable entries.
    std::vector<std::string> names;
    names.reserve(exports_.entries.size());
    for (const ExportEntry& entry : exports_.entries) {
        names.push_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

TEST_F(D3D9ManagerTest, EveryVtableSlotHasTheStdcallCleanupOfItsSignature) {
    // A wrong callee-pop size corrupts the guest stack on every call, so this
    // checks the whole table rather than a sample.
    for (usize index = 0; index < D3D9_INTERFACE_COUNT; ++index) {
        const auto iface = static_cast<D3D9InterfaceId>(index);
        usize count = 0;
        const D3D9MethodDesc* methods = d3d9_interface_methods(iface, count);
        ASSERT_NE(methods, nullptr);
        for (usize slot = 0; slot < count; ++slot) {
            const ExportEntry& entry =
                export_named(d3d9_method_export_name(iface, slot));
            EXPECT_EQ(entry.stdcall_bytes, methods[slot].stdcall_bytes)
                << d3d9_interface_name(iface) << "::" << methods[slot].name;
            // `this` alone is four bytes; nothing can pop less.
            EXPECT_GE(entry.stdcall_bytes, 4u);
            EXPECT_EQ(entry.stdcall_bytes % 4u, 0u);
        }
    }
}

// Names, ordinals and argument bytes of the non-PIX entry points, from the
// Windows 11 SysWOW64/System32 d3d9.dll export tables and prologues
// (tools/oracle_probe/d3d9_exports_probe{32,64}.winoracle-gpu.expected).
// An import by ordinal binds to whatever sits at that ordinal here.
TEST_F(D3D9ManagerTest, EntryPointsMatchD3D9DllNamesOrdinalsAndStdcallBytes) {
    struct Expected { const char* name; u32 ordinal; u16 bytes; };
    constexpr Expected expected[] = {
        {"Direct3DCreate9On12", 20, 12},    // (UINT, D3D9ON12_ARGS*, UINT)
        {"Direct3DCreate9On12Ex", 21, 16},  // (..., IDirect3D9Ex**)
        {"Direct3DShaderValidatorCreate9", 24, 0},
        {"DebugSetLevel", 34, 0},           // plain ret
        {"DebugSetMute", 35, 0},            // plain ret
        {"Direct3D9EnableMaximizedWindowedModeShim", 36, 4},  // ret 4
        {"Direct3DCreate9", 37, 4},         // (UINT)
        {"Direct3DCreate9Ex", 38, 8},       // (UINT, IDirect3D9Ex**)
    };
    for (const Expected& e : expected) {
        SCOPED_TRACE(e.name);
        const ExportEntry& entry = export_named(e.name);
        EXPECT_EQ(entry.ordinal, e.ordinal);
        EXPECT_EQ(entry.stdcall_bytes, e.bytes);
    }
}

// Measured: 31 and 32 succeed with or without the D3D_DEBUG_INFO bit;
// 0, 1, 30, 33 and 0x80000021 fail (NULL / D3DERR_NOTAVAILABLE).
TEST_F(D3D9ManagerTest, FactoriesAcceptOnlyTheShippedSdkVersions) {
    for (const u32 version : {31u, 32u, 0x80000020u, 0x8000001Fu}) {
        EXPECT_TRUE(d3d9_sdk_version_supported(version)) << version;
    }
    for (const u32 version : {0u, 1u, 30u, 33u, 0x80000021u, 0x80000000u}) {
        EXPECT_FALSE(d3d9_sdk_version_supported(version)) << version;
    }
}

TEST_F(D3D9ManagerTest, Direct3DCreate9ExRejectsABadSdkVersionAndClearsOut) {
    constexpr GuestAddr kOut = SCRATCH_ADDR + 0x900;
    ASSERT_TRUE(memory_.write_u32(kOut, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, 33u).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, kOut).is_ok());
    export_named("Direct3DCreate9Ex").handler(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(kOut).value(), 0u);
}

TEST_F(D3D9ManagerTest, Direct3DCreate9On12RefusesACallerSuppliedD3D12Device) {
    // One x86 D3D9ON12_ARGS (24 bytes): Enable9On12, pD3D12Device, ...
    constexpr GuestAddr kArgs = SCRATCH_ADDR + 0x940;
    ASSERT_TRUE(memory_.write_u32(kArgs, 1u).is_ok());
    ASSERT_TRUE(memory_.write_u32(kArgs + 4, 0x00300000u).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, 32u).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, kArgs).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 1u).is_ok());
    cpu_.gpr.rax = 0x5A5A5A5Au;
    export_named("Direct3DCreate9On12").handler(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0u);
    // An unsupported SDK version is NULL whatever the list says.
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, 0u).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, 0u).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 0u).is_ok());
    export_named("Direct3DCreate9On12").handler(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0u);
}

TEST_F(D3D9ManagerTest, EnableMaximizedWindowedModeShimReturnsZero) {
    for (const u32 value : {0u, 1u, 2u}) {
        ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, value).is_ok());
        cpu_.gpr.rax = 0x5A5A5A5Au;
        export_named("Direct3D9EnableMaximizedWindowedModeShim").handler(marshaler_);
        EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0u) << value;
    }
}

// The complete D3DPERF_* (PIX) export set. Names and ordinals come from the
// SysWOW64 and System32 d3d9.dll export tables; argument bytes from the WINAPI
// declarations in SDK shared\d3d9.h. A missing one is bound to a fail stub
// that cannot know its arity, so its arguments stay on an x86 caller's stack.
TEST_F(D3D9ManagerTest, D3DPerfExportsMatchD3D9DllNamesOrdinalsAndStdcallBytes) {
    struct Expected { const char* name; u32 ordinal; u16 bytes; };
    constexpr Expected expected[] = {
        {"D3DPERF_BeginEvent", 27, 8},      // (D3DCOLOR, LPCWSTR)
        {"D3DPERF_EndEvent", 28, 0},        // (void)
        {"D3DPERF_GetStatus", 29, 0},       // (void)
        {"D3DPERF_QueryRepeatFrame", 30, 0},// (void)
        {"D3DPERF_SetMarker", 31, 8},       // (D3DCOLOR, LPCWSTR)
        {"D3DPERF_SetOptions", 32, 4},      // (DWORD)
        {"D3DPERF_SetRegion", 33, 8},       // (D3DCOLOR, LPCWSTR)
    };
    usize perf_exports = 0;
    for (const ExportEntry& entry : exports_.entries) {
        if (entry.name.rfind("D3DPERF_", 0) == 0) ++perf_exports;
    }
    EXPECT_EQ(perf_exports, std::size(expected));
    for (const Expected& e : expected) {
        SCOPED_TRACE(e.name);
        const ExportEntry& entry = export_named(e.name);
        EXPECT_EQ(entry.ordinal, e.ordinal);
        EXPECT_EQ(entry.stdcall_bytes, e.bytes);
    }
}

// Return values with no profiler attached, as measured on Windows
// (tools/oracle_probe/d3d9_perf_probe{32,64}.expected): GetStatus 0,
// QueryRepeatFrame FALSE, BeginEvent/EndEvent -1 however they nest, and
// SetOptions changes none of that.
TEST_F(D3D9ManagerTest, D3DPerfReturnsMatchWindowsWithNoProfiler) {
    const auto call = [this](const char* name, std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            EXPECT_TRUE(memory_.write_u32(
                cpu_.gpr.rsp + 4 + index * sizeof(u32), argument).is_ok());
            ++index;
        }
        cpu_.gpr.rax = 0x5A5A5A5Au;
        export_named(name).handler(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    constexpr u32 kMinusOne = 0xFFFFFFFFu;
    EXPECT_EQ(call("D3DPERF_GetStatus", {}), 0u);
    EXPECT_EQ(call("D3DPERF_QueryRepeatFrame", {}), 0u);
    EXPECT_EQ(call("D3DPERF_BeginEvent", {0xFF00FF00u, 0}), kMinusOne);
    EXPECT_EQ(call("D3DPERF_BeginEvent", {0xFF0000FFu, 0}), kMinusOne);
    EXPECT_EQ(call("D3DPERF_EndEvent", {}), kMinusOne);
    EXPECT_EQ(call("D3DPERF_EndEvent", {}), kMinusOne);
    EXPECT_EQ(call("D3DPERF_EndEvent", {}), kMinusOne);  // unbalanced
    (void)call("D3DPERF_SetMarker", {0xFFFFFFFFu, 0});
    (void)call("D3DPERF_SetRegion", {0xFFFFFFFFu, 0});
    (void)call("D3DPERF_SetOptions", {1});
    EXPECT_EQ(call("D3DPERF_GetStatus", {}), 0u);
    EXPECT_EQ(call("D3DPERF_QueryRepeatFrame", {}), 0u);
    EXPECT_EQ(call("D3DPERF_BeginEvent", {0, 0}), kMinusOne);
    EXPECT_EQ(call("D3DPERF_EndEvent", {}), kMinusOne);
    (void)call("D3DPERF_SetOptions", {0});
    EXPECT_EQ(call("D3DPERF_GetStatus", {}), 0u);
}

TEST_F(D3D9ManagerTest, D3D9MethodsUseExactX86StdcallCleanup) {
    constexpr std::array<u16, D3D9_METHOD_COUNT> expected{{
        12, 4, 4, 8, 4, 16, 12, 20, 12,
        24, 28, 28, 24, 20, 16, 8, 28,
    }};
    for (usize slot = 0; slot < D3D9_METHOD_COUNT; ++slot) {
        EXPECT_EQ(
            export_named(d3d9_method_export_name(D3D9InterfaceId::Direct3D9, slot)).stdcall_bytes,
            expected[slot]);
    }
}
