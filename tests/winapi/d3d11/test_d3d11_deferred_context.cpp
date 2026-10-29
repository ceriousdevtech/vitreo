// SPDX-License-Identifier: LGPL-2.1-only
//
// Deferred contexts: own COM object, creation rules, Map rules, replay.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// --- Deferred context: its own COM object ----------------------------------
//
// Expected values: tools/oracle_probe/d3d11_a12_object_model_probe64.expected
// ([qi] deferred_context, [rc], [pd] share, [def]) and
// d3d11_deferred_rules_probe64.expected, both Windows. IIDs are the SDK's
// (mingw d3d11.h / d3d11_1.h / d3d11_4.h DEFINE_GUIDs), not Vitreo's table.


struct SdkGuid {
    u32 d1;
    u16 d2, d3;
    std::array<u8, 8> d4;
};
constexpr SdkGuid kSdkIUnknown{0x00000000, 0x0000, 0x0000,
                               {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr SdkGuid kSdkContext{0xc0bfa96c, 0xe089, 0x44fb,
                              {0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda}};
constexpr SdkGuid kSdkContext1{0xbb2c6faa, 0xb5fb, 0x4082,
                               {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
constexpr SdkGuid kSdkContext4{0x917600da, 0xf58c, 0x4c33,
                               {0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24}};
constexpr SdkGuid kSdkAnnotation{0xb2daad8b, 0x03d4, 0x4dbf,
                                 {0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab}};
constexpr SdkGuid kSdkMultithread{0x9b7e4e00, 0x342c, 0x4106,
                                  {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};

void write_sdk_guid(GuestMemory& memory, GuestAddr address, const SdkGuid& g) {
    ASSERT_TRUE(memory.write_u32(address, g.d1).is_ok());
    ASSERT_TRUE(memory.write_u16(address + 4, g.d2).is_ok());
    ASSERT_TRUE(memory.write_u16(address + 6, g.d3).is_ok());
    ASSERT_TRUE(memory.write_bytes(address + 8, g.d4.data(), 8).is_ok());
}

constexpr u32 kDeferredMapWithoutInitialDiscard = 0x887C0004u;  // winerror.h
constexpr u32 kDxgiErrorNotFound = 0x887A0002u;


TEST_F(D3D11ManagerTest, DeferredContextIsItsOwnComObject) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr IID = DATA_BASE + 0x1010;
    constexpr GuestAddr OUT = DATA_BASE + 0x1020;
    constexpr GuestAddr PD_GUID = DATA_BASE + 0x1030;
    constexpr GuestAddr PD_DATA = DATA_BASE + 0x1040;
    constexpr GuestAddr PD_SIZE = DATA_BASE + 0x1048;

    const auto context_call = [&](GuestAddr self, usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto count_of = [&](GuestAddr self, bool is_device) {
        set_arg(0, self);
        if (is_device) d3d11.impl_DeviceMethod(marshaler, 1);
        else d3d11.impl_ContextMethod(marshaler, 1);
        const u32 count = static_cast<u32>(cpu.gpr.rax);
        set_arg(0, self);
        if (is_device) d3d11.impl_DeviceMethod(marshaler, 2);
        else d3d11.impl_ContextMethod(marshaler, 2);
        return count - 1;
    };

    const u32 device_before = count_of(device, true);
    const u32 immediate_before = count_of(immediate, false);
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();
    ASSERT_NE(deferred, 0u);
    // [create] deferred_context device-refs+1.
    EXPECT_EQ(count_of(device, true), device_before + 1);

    // [def] GetType: 1 (D3D11_DEVICE_CONTEXT_DEFERRED); immediate 0.
    EXPECT_EQ(context_call(deferred, 112, {}), 1u);
    EXPECT_EQ(context_call(immediate, 112, {}), 0u);

    // [def] deferred AddRef=2 Release=1, and the immediate count is its own.
    EXPECT_EQ(context_call(deferred, 1, {}), 2u);
    EXPECT_EQ(context_call(deferred, 2, {}), 1u);
    EXPECT_EQ(count_of(immediate, false), immediate_before);

    // [qi] deferred IUnknown == immediate IUnknown: 0; QI(Context1) is the
    // deferred context itself.
    for (const SdkGuid* iid : {&kSdkIUnknown, &kSdkContext, &kSdkContext1,
                               &kSdkContext4}) {
        write_sdk_guid(memory, IID, *iid);
        ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
        EXPECT_EQ(context_call(deferred, 0, {IID, OUT}), d3d11::S_OK);
        EXPECT_EQ(memory.read_u64(OUT).value(), deferred);
        EXPECT_EQ(context_call(deferred, 2, {}), 1u);
    }
    // Annotation is a facet with the context's identity and count.
    write_sdk_guid(memory, IID, kSdkAnnotation);
    EXPECT_EQ(context_call(deferred, 0, {IID, OUT}), d3d11::S_OK);
    const GuestAddr annotation = memory.read_u64(OUT).value();
    ASSERT_NE(annotation, 0u);
    EXPECT_NE(annotation, deferred);
    EXPECT_EQ(context_call(deferred, 1, {}), 3u) << "the facet shares the count";
    EXPECT_EQ(context_call(deferred, 2, {}), 2u);
    write_sdk_guid(memory, IID, kSdkIUnknown);
    set_arg(0, annotation);
    set_arg(1, IID);
    set_arg(2, OUT);
    d3d11.impl_AnnotationMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(OUT).value(), deferred);
    set_arg(0, annotation);
    d3d11.impl_AnnotationMethod(marshaler, 2);
    set_arg(0, annotation);
    d3d11.impl_AnnotationMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    // Not answered by a deferred context (a12 [qi] answers list).
    write_sdk_guid(memory, IID, kSdkMultithread);
    ASSERT_TRUE(memory.write_u64(OUT, 0xA5A5).is_ok());
    EXPECT_EQ(context_call(deferred, 0, {IID, OUT}), d3d11::E_NOINTERFACE);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);

    // [pd] share: set on deferred context, get on immediate -> NOT_FOUND.
    write_sdk_guid(memory, PD_GUID, kSdkContext4);
    ASSERT_TRUE(memory.write_u32(PD_DATA, 0x11223344u).is_ok());
    EXPECT_EQ(context_call(deferred, 5, {PD_GUID, 4, PD_DATA}), d3d11::S_OK);
    ASSERT_TRUE(memory.write_u32(PD_SIZE, 4).is_ok());
    EXPECT_EQ(context_call(immediate, 4, {PD_GUID, PD_SIZE, PD_DATA}),
              kDxgiErrorNotFound);
    ASSERT_TRUE(memory.write_u32(PD_SIZE, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(PD_DATA, 0).is_ok());
    EXPECT_EQ(context_call(deferred, 4, {PD_GUID, PD_SIZE, PD_DATA}),
              d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(PD_DATA).value(), 0x11223344u);

    // [def] deferred last Release=0: it frees the context and drops its
    // device reference; the immediate context is untouched.
    EXPECT_EQ(context_call(deferred, 2, {}), 0u);
    EXPECT_EQ(count_of(device, true), device_before);
    EXPECT_EQ(count_of(immediate, false), immediate_before);
}

TEST_F(D3D11ManagerTest, DeferredContextCreationAndImmediateOnlyCalls) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr OUT = DATA_BASE + 0x1010;
    constexpr GuestAddr DATA = DATA_BASE + 0x1020;
    constexpr GuestAddr QUERY_DESC = DATA_BASE + 0x1030;
    constexpr GuestAddr QUERY_OUT = DATA_BASE + 0x1040;
    const auto context_call = [&](GuestAddr self, usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };

    // A SINGLETHREADED device: DXGI_ERROR_INVALID_CALL, out untouched.
    prepare_create();
    set_arg(3, d3d11::CREATE_DEVICE_SINGLETHREADED);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr single = memory.read_u64(DEVICE_OUT).value();
    ASSERT_TRUE(memory.write_u64(CTX_OUT, 0x5A5A5A50u).is_ok());
    set_arg(0, single);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    EXPECT_EQ(cpu.gpr.rax, DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u64(CTX_OUT).value(), 0x5A5A5A50u);

    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // flags=1: E_INVALIDARG, out untouched. NULL out: S_FALSE.
    set_arg(0, device);
    set_arg(1, 1);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(CTX_OUT).value(), 0x5A5A5A50u);
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, 0);
    d3d11.impl_DeviceMethod(marshaler, 27);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();

    // GetData on a deferred context: DXGI_ERROR_INVALID_CALL, data untouched.
    const std::array<u32, 2> event_desc{0, 0};  // D3D11_QUERY_EVENT
    ASSERT_TRUE(memory.write_bytes(
        QUERY_DESC, reinterpret_cast<const Byte*>(event_desc.data()),
        sizeof(event_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, QUERY_DESC);
    set_arg(2, QUERY_OUT);
    d3d11.impl_DeviceMethod(marshaler, 24);  // CreateQuery
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr query = memory.read_u64(QUERY_OUT).value();
    context_call(deferred, 28, {query});  // End
    ASSERT_TRUE(memory.write_u32(DATA, 7).is_ok());
    EXPECT_EQ(context_call(deferred, 29, {query, DATA, 4, 0}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u32(DATA).value(), 7u);
    EXPECT_EQ(context_call(deferred, 29, {query, 0, 0, 0}),
              DXGI_ERROR_INVALID_CALL);
    // The recorded End has not run: on the immediate context the query was
    // never issued (d3d11_deferred_rules_probe [query]).
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    EXPECT_EQ(context_call(immediate, 29, {query, DATA, 4, 0}),
              DXGI_ERROR_INVALID_CALL);

    // Learn: Signal / Wait "only applies to immediate-mode contexts";
    // measured DXGI_ERROR_INVALID_CALL.
    EXPECT_EQ(context_call(deferred, 147, {0, 5}), DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(context_call(deferred, 148, {0, 5}), DXGI_ERROR_INVALID_CALL);

    // FinishCommandList(..., NULL): E_POINTER.
    EXPECT_EQ(context_call(deferred, 114, {0, 0}), d3d11::E_POINTER);

    // SwapDeviceContextState on a deferred context: previous NULL, no-op.
    ASSERT_TRUE(memory.write_u64(OUT, 0x5A5A5A50u).is_ok());
    context_call(deferred, 131, {0x1234, OUT});
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, DeferredContextMapRules) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr DESC = DATA_BASE + 0x1010;
    constexpr GuestAddr OUT = DATA_BASE + 0x1040;
    constexpr GuestAddr MAPPED = DATA_BASE + 0x1050;
    constexpr GuestAddr LIST_OUT = DATA_BASE + 0x1068;
    const auto context_call = [&](GuestAddr self, usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto make_buffer = [&](std::array<u32, 6> desc) {
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return static_cast<GuestAddr>(memory.read_u64(OUT).value());
    };
    const GuestAddr dynamic_vb = make_buffer({64, 2, 0x1, 0x10000, 0, 0});
    const GuestAddr dynamic_cb = make_buffer({64, 2, 0x4, 0x10000, 0, 0});
    const GuestAddr default_vb = make_buffer({64, 0, 0x1, 0, 0, 0});
    const GuestAddr staging = make_buffer({64, 3, 0, 0x30000, 0, 0});
    const std::array<u32, 11> texture_desc{4, 4, 1, 1, 28, 1, 0, 2, 0x8,
                                           0x10000, 0};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dynamic_texture = memory.read_u64(OUT).value();
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();

    const auto map = [&](GuestAddr resource, u32 sub, u32 type, u32 flags) {
        const std::array<u8, 16> fill{0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
                                      0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
                                      0xAB, 0xAB, 0xAB, 0xAB};
        EXPECT_TRUE(memory.write_bytes(MAPPED, fill.data(), fill.size()).is_ok());
        return context_call(deferred, 14, {resource, sub, type, flags, MAPPED});
    };
    // NO_OVERWRITE before any DISCARD of the resource: 0x887C0004, pData NULL.
    EXPECT_EQ(map(dynamic_vb, 0, 5, 0), kDeferredMapWithoutInitialDiscard);
    EXPECT_EQ(memory.read_u64(MAPPED).value(), 0u);
    EXPECT_EQ(map(dynamic_cb, 0, 5, 0), kDeferredMapWithoutInitialDiscard);
    // DISCARD with a NULL pMappedResource: E_INVALIDARG.
    EXPECT_EQ(context_call(deferred, 14, {dynamic_vb, 0, 4, 0, 0}),
              d3d11::E_INVALIDARG);
    // DISCARD: fresh memory, both pitches 0 for a buffer.
    EXPECT_EQ(map(dynamic_vb, 0, 4, 0), d3d11::S_OK);
    const GuestAddr block = memory.read_u64(MAPPED).value();
    EXPECT_NE(block, 0u);
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(MAPPED + 12).value(), 0u);
    context_call(deferred, 15, {dynamic_vb, 0});
    // NO_OVERWRITE after it: the same memory.
    EXPECT_EQ(map(dynamic_vb, 0, 5, 0), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(MAPPED).value(), block);
    context_call(deferred, 15, {dynamic_vb, 0});
    // Everything else: E_INVALIDARG with pData NULL.
    for (const u32 type : {1u, 2u, 3u, 0u}) {
        EXPECT_EQ(map(dynamic_vb, 0, type, 0), d3d11::E_INVALIDARG) << type;
        EXPECT_EQ(memory.read_u64(MAPPED).value(), 0u);
    }
    EXPECT_EQ(map(dynamic_vb, 1, 4, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(dynamic_vb, 0, 4, 0x100000), d3d11::E_INVALIDARG)
        << "DO_NOT_WAIT";
    EXPECT_EQ(map(dynamic_vb, 0, 4, 0x1234), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(default_vb, 0, 4, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(staging, 0, 1, 0), d3d11::E_INVALIDARG) << "staging READ";
    EXPECT_EQ(map(staging, 0, 2, 0), d3d11::E_INVALIDARG) << "staging WRITE";
    EXPECT_EQ(map(staging, 0, 3, 0), d3d11::E_INVALIDARG);
    // A texture: NO_OVERWRITE is 0x887C0004 even after a DISCARD.
    EXPECT_EQ(map(dynamic_texture, 0, 5, 0), kDeferredMapWithoutInitialDiscard);
    EXPECT_EQ(map(dynamic_texture, 0, 4, 0), d3d11::S_OK);
    context_call(deferred, 15, {dynamic_texture, 0});
    EXPECT_EQ(map(dynamic_texture, 0, 5, 0), kDeferredMapWithoutInitialDiscard);
    // The initial-DISCARD requirement starts over at FinishCommandList.
    EXPECT_EQ(context_call(deferred, 114, {0, LIST_OUT}), d3d11::S_OK);
    EXPECT_EQ(map(dynamic_vb, 0, 5, 0), kDeferredMapWithoutInitialDiscard);
}

// The required property: nothing a deferred context records runs before
// ExecuteCommandList, and at Execute it runs with the values it was recorded
// with. Expected values: a12 [def] ("after deferred Clear, before Execute",
// "after deferred UpdateSubresource", "deferred Map+Copy") and
// d3d11_deferred_rules_probe [exec].
TEST_F(D3D11ManagerTest, DeferredWorkIsInvisibleBeforeExecuteAndCorrectAfter) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr DESC = DATA_BASE + 0x1010;
    constexpr GuestAddr OUT = DATA_BASE + 0x1040;
    constexpr GuestAddr MAPPED = DATA_BASE + 0x1050;
    constexpr GuestAddr LIST_OUT = DATA_BASE + 0x1068;
    constexpr GuestAddr BOX = DATA_BASE + 0x1070;
    constexpr GuestAddr COLOR_IN = DATA_BASE + 0x1090;
    constexpr GuestAddr SOURCE = DATA_BASE + 0x1100;
    const auto context_call = [&](GuestAddr self, usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto make = [&](usize slot, const auto& desc) {
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, slot);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return static_cast<GuestAddr>(memory.read_u64(OUT).value());
    };
    const GuestAddr target = make(5, std::array<u32, 11>{
        4, 4, 1, 1, 28, 1, 0, 0, 0x20, 0, 0});
    set_arg(0, device);
    set_arg(1, target);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);  // CreateRenderTargetView
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(OUT).value();
    const GuestAddr destination = make(3, std::array<u32, 6>{64, 0, 0x1, 0, 0, 0});
    const GuestAddr dynamic = make(3, std::array<u32, 6>{64, 2, 0x1, 0x10000, 0, 0});
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();
    const auto finish = [&] {
        EXPECT_EQ(context_call(deferred, 114, {0, LIST_OUT}), d3d11::S_OK);
        return static_cast<GuestAddr>(memory.read_u64(LIST_OUT).value());
    };

    const unsigned clears = backend.state.texture_clears;
    const unsigned updates = backend.state.buffer_updates;
    const unsigned copies = backend.state.buffer_copies;

    // 1. ClearRenderTargetView + UpdateSubresource with a box.
    const std::array<float, 4> green{0.0f, 1.0f, 0.0f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR_IN, reinterpret_cast<const Byte*>(green.data()),
        sizeof(green)).is_ok());
    context_call(deferred, 50, {rtv, COLOR_IN});
    std::array<u8, 64> source{};
    for (usize i = 0; i < source.size(); ++i) source[i] = static_cast<u8>(i);
    ASSERT_TRUE(memory.write_bytes(SOURCE, source.data(), source.size()).is_ok());
    const std::array<u32, 6> box{8, 0, 0, 24, 1, 1};
    ASSERT_TRUE(memory.write_bytes(
        BOX, reinterpret_cast<const Byte*>(box.data()), sizeof(box)).is_ok());
    context_call(deferred, 48, {destination, 0, BOX, SOURCE, 0, 0});
    // Learn (UpdateSubresource): the caller may change or free pSrcData as
    // soon as the call returns; the colour and the box are copied too.
    const std::array<u8, 64> scribble = [] {
        std::array<u8, 64> bytes{};
        bytes.fill(0xEE);
        return bytes;
    }();
    ASSERT_TRUE(memory.write_bytes(SOURCE, scribble.data(), 64).is_ok());
    ASSERT_TRUE(memory.write_bytes(COLOR_IN, scribble.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_bytes(BOX, scribble.data(), 24).is_ok());
    EXPECT_EQ(backend.state.texture_clears, clears)
        << "a recorded Clear must not run before ExecuteCommandList";
    EXPECT_EQ(backend.state.buffer_updates, updates)
        << "a recorded UpdateSubresource must not run before Execute";
    const GuestAddr list = finish();
    EXPECT_EQ(backend.state.texture_clears, clears);
    EXPECT_EQ(backend.state.buffer_updates, updates);

    context_call(immediate, 58, {list, 0});
    EXPECT_EQ(backend.state.texture_clears, clears + 1);
    EXPECT_EQ(backend.state.last_clear, green);
    ASSERT_EQ(backend.state.buffer_updates, updates + 1);
    EXPECT_EQ(backend.state.last_buffer_update_offset, 8u);
    // Learn ("Calling UpdateSubresource on a Deferred Context"): with
    // DriverCommandLists FALSE the box offset is applied to pSrcData too
    // (measured: a box at byte 8 reads pSrcData + 8).
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(source.begin() + 8, source.begin() + 24));
    // The list replays: executing it again does it all again.
    context_call(immediate, 58, {list, 0});
    EXPECT_EQ(backend.state.texture_clears, clears + 2);
    EXPECT_EQ(backend.state.buffer_updates, updates + 2);

    // 2. UpdateSubresource1 with a box reads pSrcData as given (measured).
    ASSERT_TRUE(memory.write_bytes(SOURCE, source.data(), source.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        BOX, reinterpret_cast<const Byte*>(box.data()), sizeof(box)).is_ok());
    context_call(deferred, 116, {destination, 0, BOX, SOURCE, 0, 0, 0});
    context_call(immediate, 58, {finish(), 0});
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(source.begin(), source.begin() + 16));

    // 3. Map(WRITE_DISCARD) + CopyResource: the immediate context sees
    // neither until the list executes.
    const unsigned updates_before_map = backend.state.buffer_updates;
    ASSERT_EQ(context_call(deferred, 14, {dynamic, 0, 4, 0, MAPPED}),
              d3d11::S_OK);
    const GuestAddr block = memory.read_u64(MAPPED).value();
    std::array<u8, 64> mapped_bytes{};
    mapped_bytes.fill(0x22);
    ASSERT_TRUE(memory.write_bytes(block, mapped_bytes.data(), 64).is_ok());
    context_call(deferred, 15, {dynamic, 0});
    context_call(deferred, 47, {destination, dynamic});
    EXPECT_EQ(backend.state.buffer_updates, updates_before_map);
    EXPECT_EQ(backend.state.buffer_copies, copies);
    const GuestAddr map_list = finish();
    EXPECT_EQ(backend.state.buffer_copies, copies);
    context_call(immediate, 58, {map_list, 0});
    ASSERT_EQ(backend.state.buffer_updates, updates_before_map + 1);
    EXPECT_EQ(backend.state.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(mapped_bytes.begin(), mapped_bytes.end()));
    EXPECT_EQ(backend.state.buffer_copies, copies + 1);
}

// FLOAT arguments ride in XMM on x64 and must survive recording; state is
// the list's own, and the executing context's comes back only with
// RestoreContextState TRUE.
TEST_F(D3D11ManagerTest, DeferredReplayKeepsFloatArgumentsAndStateRules) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr DESC = DATA_BASE + 0x1010;
    constexpr GuestAddr OUT = DATA_BASE + 0x1040;
    constexpr GuestAddr LIST_OUT = DATA_BASE + 0x1068;
    constexpr GuestAddr VIEWPORT = DATA_BASE + 0x1080;
    constexpr GuestAddr COUNT = DATA_BASE + 0x10A0;
    constexpr GuestAddr VIEWPORT_OUT = DATA_BASE + 0x10B0;
    const auto context_call = [&](GuestAddr self, usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const std::array<u32, 11> depth_desc{4, 4, 1, 1, 45, 1, 0, 0, 0x40, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(depth_desc.data()),
        sizeof(depth_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr depth = memory.read_u64(OUT).value();
    set_arg(0, device);
    set_arg(1, depth);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);  // CreateDepthStencilView
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(OUT).value();
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();

    // ClearDepthStencilView(pDSV, Flags, FLOAT Depth, UINT8 Stencil): Depth
    // is XMM3 on x64. Clobber XMM3 after the call; the replay must still
    // clear to 0.25.
    const float quarter = 0.25f;
    std::memcpy(&cpu.sse.xmm[3].u32_[0], &quarter, sizeof(quarter));
    context_call(deferred, 53, {dsv, 3, 0, 7});
    cpu.sse.xmm[3].u32_[0] = 0;
    backend.state.last_depth_clear_value = -1.0f;
    // A viewport recorded on the deferred context.
    const std::array<float, 6> viewport{1, 2, 30, 40, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        VIEWPORT, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    context_call(deferred, 44, {1, VIEWPORT});
    EXPECT_EQ(context_call(deferred, 114, {1, LIST_OUT}), d3d11::S_OK);
    const GuestAddr list = memory.read_u64(LIST_OUT).value();
    EXPECT_EQ(backend.state.last_depth_clear_value, -1.0f);
    // FinishCommandList(TRUE) keeps the deferred context's state (a12 [def]).
    ASSERT_TRUE(memory.write_u32(COUNT, 1).is_ok());
    context_call(deferred, 95, {COUNT, VIEWPORT_OUT});
    EXPECT_EQ(memory.read_u32(COUNT).value(), 1u);

    // The immediate context's own viewport, restored by Execute(TRUE).
    const std::array<float, 6> own{5, 6, 70, 80, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        VIEWPORT, reinterpret_cast<const Byte*>(own.data()),
        sizeof(own)).is_ok());
    context_call(immediate, 44, {1, VIEWPORT});
    context_call(immediate, 58, {list, 1});
    EXPECT_EQ(backend.state.last_depth_clear_value, 0.25f);
    EXPECT_EQ(backend.state.last_stencil_clear_value, 7u);
    ASSERT_TRUE(memory.write_u32(COUNT, 1).is_ok());
    context_call(immediate, 95, {COUNT, VIEWPORT_OUT});
    EXPECT_EQ(memory.read_u32(COUNT).value(), 1u);
    float width = 0.0f;
    ASSERT_TRUE(memory.read_bytes(VIEWPORT_OUT + 8,
                                  reinterpret_cast<Byte*>(&width),
                                  sizeof(width)).is_ok());
    EXPECT_EQ(width, 70.0f) << "Execute(RestoreContextState=TRUE)";
    // Execute(FALSE): the default state afterwards.
    context_call(immediate, 58, {list, 0});
    ASSERT_TRUE(memory.write_u32(COUNT, 1).is_ok());
    context_call(immediate, 95, {COUNT, VIEWPORT_OUT});
    EXPECT_EQ(memory.read_u32(COUNT).value(), 0u)
        << "Execute(RestoreContextState=FALSE)";
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
