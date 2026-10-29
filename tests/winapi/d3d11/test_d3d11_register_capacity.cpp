// SPDX-License-Identifier: LGPL-2.1-only
//
// Register-space capacity, high registers, Context1 slots, fences and indirect arguments.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Register-space capacity
// ---------------------------------------------------------------------------


/// Wrap an SM5 token stream in a minimal DXBC container.
///
/// The parser zeroes the checksum rather than verifying it, so a hand-built
/// container only needs the magic, a truthful total size, and one SHEX chunk.
[[nodiscard]] std::vector<u8> dxbc_container(const std::vector<u32>& tokens) {
    const u32 code_bytes = static_cast<u32>(tokens.size() * sizeof(u32));
    // Header (32) + one chunk offset (4) + chunk header (8) + code.
    constexpr u32 HEADER_BYTES = 32;
    const u32 chunk_offset = HEADER_BYTES + 4;
    const u32 total = chunk_offset + 8 + code_bytes;
    std::vector<u8> blob;
    const auto push = [&](u32 word) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            blob.push_back(static_cast<u8>((word >> (byte * 8)) & 0xFFu));
        }
    };
    push(0x43425844u);                       // "DXBC"
    for (int i = 0; i < 4; ++i) push(0u);    // checksum
    push(1u);                                // one
    push(total);
    push(1u);                                // chunk count
    push(chunk_offset);
    push(0x58454853u);                       // "SHEX"
    push(code_bytes);
    for (const u32 token : tokens) push(token);
    return blob;
}

/// A compute shader that samples t`srv_register` through s0 and stores to u0.
///
/// Modelled on a real dxc-emitted sample_l kernel; only the t# register number
/// varies, which is exactly the axis that decides whether the shader fits the
/// backend's flat binding capacity.
[[nodiscard]] std::vector<u8> compute_sampling_srv(u32 srv_register) {
    const std::vector<u32> tokens{
        0x00050050u, 41u,
        0x0100086Au,
        0x02000068u, 1u,                      // dcl_temps 1
        0x0400009Bu, 1u, 1u, 1u,              // dcl_thread_group 1,1,1
        0x04001858u, 0x00107000u, srv_register, 0x00005555u,
        0x0300005Au, 0x00106000u, 0u,         // sampler s0
        0x0400189Cu, 0x0011E000u, 0u, 0x00005555u,  // RWTexture2D u0
        0x0E000048u,                          // sample_l
        0x001000F2u, 0u,
        0x00004002u, 0x3F000000u, 0x3F000000u, 0u, 0u,
        0x00107E46u, srv_register,
        0x00106000u, 0u,
        0x00004001u, 0u,
        0x060000A4u,                          // store_uav_typed
        0x0011E0F2u, 0u,
        0x00020E46u, 0x00100E46u, 0u,
        0x0100003Eu,                          // ret
    };
    return dxbc_container(tokens);
}


// A shader declaring a register past the backend's flat capacity cannot be
// served by the flat path at all -- the binding is dropped and the draw renders
// wrong with no diagnostic. Such a shader is compiled to read descriptor tables
// instead, and when the backend cannot provide those, creation FAILS.
//
// Failing is the point. A shader that renders silently wrong is harder to
// diagnose than one that refuses to be created, and the fake backend in these
// tests has no descriptor tables, so this is the honest outcome here.
TEST_F(D3D11ManagerTest, ShaderPastFlatCapacityNeedsDescriptorTables) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BYTECODE = DATA_BASE + 0x2000;
    constexpr GuestAddr SHADER_OUT = DATA_BASE + 0x2800;

    const auto create_cs = [&](u32 srv_register) {
        const auto blob = compute_sampling_srv(srv_register);
        EXPECT_TRUE(memory.write_bytes(
            BYTECODE, reinterpret_cast<const Byte*>(blob.data()),
            blob.size()).is_ok());
        EXPECT_TRUE(memory.write_u64(SHADER_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, BYTECODE);
        set_arg(2, blob.size());
        set_arg(3, 0);
        set_arg(4, SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 18);
    };

    ASSERT_FALSE(backend.supports_descriptor_tables())
        << "this test's premise is a backend without tables";

    // t7 is the highest register the flat path can reach, so this one is served
    // the way it always was.
    create_cs(graphics::BACKEND_MAX_TEXTURE_SLOTS - 1);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK)
        << "a shader inside the flat capacity must still compile";
    EXPECT_NE(memory.read_u64(SHADER_OUT).value(), 0u);

    // t8 is one past it. Without descriptor tables there is nowhere to bind it.
    create_cs(graphics::BACKEND_MAX_TEXTURE_SLOTS);
    EXPECT_NE(cpu.gpr.rax, d3d11::S_OK)
        << "a shader declaring t" << graphics::BACKEND_MAX_TEXTURE_SLOTS
        << " must not be created when it cannot be bound";
    EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);
}


// Learn, "Resource Limits (Direct3D 11)" for feature level 11: a stage addresses
// 128 shader-resource registers (D3D11_COMMONSHADER_INPUT_RESOURCE_REGISTER_COUNT),
// 14 constant-buffer API slots (15 hardware slots less the one reserved for the
// immediate constant buffer) and 16 samplers.
//
// These are the registers a GUEST may address, and they are independent of how
// many of them one draw can bind. The binding still has to be recorded, because
// the matching Get* call has to return it -- and the arrays used to be sized to
// the backend's flat capacity of 8 and 4, so the bounds check in every setter
// silently discarded anything above. A title binding t8 got no diagnostic, no
// error, and a wrong render.
TEST_F(D3D11ManagerTest, HighRegistersRoundTripThroughSetAndGet) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr TEX_DESC = DATA_BASE + 0x1000;
    constexpr GuestAddr TEX_OUT = DATA_BASE + 0x1080;
    constexpr GuestAddr SRV_OUT = DATA_BASE + 0x1090;
    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x10A0;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x10E0;
    constexpr GuestAddr SAMP_DESC = DATA_BASE + 0x1100;
    constexpr GuestAddr SAMP_OUT = DATA_BASE + 0x1180;
    constexpr GuestAddr BINDING = DATA_BASE + 0x1190;
    constexpr GuestAddr READBACK = DATA_BASE + 0x11A0;

    // A 4x4 BGRA8 shader-resource texture, and an SRV over it.
    // Width, Height, MipLevels, ArraySize, Format(B8G8R8A8_UNORM),
    // SampleDesc.Count, SampleDesc.Quality, Usage(DEFAULT),
    // BindFlags(SHADER_RESOURCE), CPUAccessFlags, MiscFlags.
    const std::array<u32, 11> tex{4, 4, 1, 1, 87, 1, 0, 0, 0x8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEX_DESC, reinterpret_cast<const Byte*>(tex.data()),
        sizeof(tex)).is_ok());
    set_arg(0, device);
    set_arg(1, TEX_DESC);
    set_arg(2, 0);
    set_arg(3, TEX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEX_OUT).value();
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr srv = memory.read_u64(SRV_OUT).value();
    ASSERT_NE(srv, 0u);

    // A constant buffer. ByteWidth is a multiple of 16, as CONSTANT_BUFFER
    // requires.
    const std::array<u32, 6> buf{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(buf.data()),
        sizeof(buf)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr cbuffer = memory.read_u64(BUF_OUT).value();
    ASSERT_NE(cbuffer, 0u);

    // A sampler: D3D11_FILTER_MIN_MAG_MIP_POINT, CLAMP on all axes.
    std::array<u32, 13> samp{};
    samp[1] = samp[2] = samp[3] = 3;  // Address U/V/W = CLAMP
    ASSERT_TRUE(memory.write_bytes(
        SAMP_DESC, reinterpret_cast<const Byte*>(samp.data()),
        sizeof(samp)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMP_DESC);
    set_arg(2, SAMP_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr sampler = memory.read_u64(SAMP_OUT).value();
    ASSERT_NE(sampler, 0u);

    struct Space {
        const char* name;
        usize setter;
        usize getter;
        GuestAddr resource;
        u32 top_register;
    };
    const std::array<Space, 9> spaces{{
        {"PS t#", 8, 73, srv, 127},
        {"VS t#", 25, 84, srv, 127},
        {"CS t#", 67, 105, srv, 127},
        {"VS b#", 7, 72, cbuffer, 13},
        {"PS b#", 16, 77, cbuffer, 13},
        // 109, not 115: see Context1SlotsDoNotAliasTheComputeGetters.
        {"CS b#", 71, 109, cbuffer, 13},
        {"PS s#", 10, 75, sampler, 15},
        {"VS s#", 26, 85, sampler, 15},
        {"CS s#", 70, 108, sampler, 15},
    }};
    // CS b# uses getter slot 109, not 115. The SDK's ID3D11DeviceContext1
    // vtable (d3d11_1.h) puts CSGetConstantBuffers at 109 and begins
    // ID3D11DeviceContext1 at 115 with CopySubresourceRegion1, so a readback
    // wired to 115 both stubbed out this getter and hijacked a copy call.
    static_assert(std::tuple_size_v<decltype(spaces)> == 9);

    for (const Space& space : spaces) {
        ASSERT_TRUE(memory.write_u64(BINDING, space.resource).is_ok());
        set_arg(0, context);
        set_arg(1, space.top_register);
        set_arg(2, 1);
        set_arg(3, BINDING);
        d3d11.impl_ContextMethod(marshaler, space.setter);

        ASSERT_TRUE(memory.write_u64(READBACK, 0).is_ok());
        set_arg(0, context);
        set_arg(1, space.top_register);
        set_arg(2, 1);
        set_arg(3, READBACK);
        d3d11.impl_ContextMethod(marshaler, space.getter);
        EXPECT_EQ(memory.read_u64(READBACK).value(), space.resource)
            << space.name << " register " << space.top_register
            << " did not survive a set/get round trip";
    }

    // One past the top is out of range in D3D, and must stay unbound rather than
    // wrap, clamp, or write past the array.
    for (const Space& space : spaces) {
        ASSERT_TRUE(memory.write_u64(BINDING, space.resource).is_ok());
        set_arg(0, context);
        set_arg(1, space.top_register + 1);
        set_arg(2, 1);
        set_arg(3, BINDING);
        d3d11.impl_ContextMethod(marshaler, space.setter);

        ASSERT_TRUE(memory.write_u64(READBACK, ~GuestAddr{0}).is_ok());
        set_arg(0, context);
        set_arg(1, space.top_register + 1);
        set_arg(2, 1);
        set_arg(3, READBACK);
        d3d11.impl_ContextMethod(marshaler, space.getter);
        EXPECT_EQ(memory.read_u64(READBACK).value(), 0u)
            << space.name << " accepted register "
            << (space.top_register + 1) << ", past the D3D11 space";
    }
}

// ID3D11DeviceContext1 begins at vtable slot 115 with CopySubresourceRegion1.
// The compute constant-buffer readback was wired there instead of to 109, so a
// guest calling CopySubresourceRegion1 got interface pointers written to an
// address taken from the copy's DstX coordinate, and no copy.
//
// Nothing on slot 115 may behave like a getter. The honest response to an
// unimplemented Context1 method is to do nothing observable, which for a void
// D3D method means leaving the caller's memory alone.
TEST_F(D3D11ManagerTest, Context1SlotsDoNotAliasTheComputeGetters) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1000;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1040;
    constexpr GuestAddr BINDING = DATA_BASE + 0x1050;
    constexpr GuestAddr READBACK = DATA_BASE + 0x1060;

    const std::array<u32, 6> buf{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(buf.data()),
        sizeof(buf)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr cbuffer = memory.read_u64(BUF_OUT).value();
    ASSERT_NE(cbuffer, 0u);

    // CSSetConstantBuffers(StartSlot=0, NumBuffers=1, ppConstantBuffers).
    ASSERT_TRUE(memory.write_u64(BINDING, cbuffer).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING);
    d3d11.impl_ContextMethod(marshaler, 71);

    // Slot 109 is the getter and must return it.
    ASSERT_TRUE(memory.write_u64(READBACK, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, READBACK);
    d3d11.impl_ContextMethod(marshaler, 109);
    EXPECT_EQ(memory.read_u64(READBACK).value(), cbuffer)
        << "CSGetConstantBuffers lives at vtable slot 109";

    // Slot 115 is CopySubresourceRegion1 and must not write the caller's memory.
    // Its arguments here are the ones a real copy would pass, and arg 3 is DstX --
    // a coordinate, not an output pointer.
    constexpr GuestAddr SENTINEL = 0xA5A5A5A5A5A5A5A5ull;
    ASSERT_TRUE(memory.write_u64(READBACK, SENTINEL).is_ok());
    set_arg(0, context);
    set_arg(1, cbuffer);   // pDstResource
    set_arg(2, 0);         // DstSubresource
    set_arg(3, READBACK);  // DstX, which the old wiring treated as ppOut
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, cbuffer);
    set_arg(7, 0);
    set_arg(8, 0);
    d3d11.impl_ContextMethod(marshaler, 115);
    EXPECT_EQ(memory.read_u64(READBACK).value(), SENTINEL)
        << "slot 115 is CopySubresourceRegion1; it must not write a getter's "
           "output into a coordinate argument";
}

// ID3D11DeviceContext1 binds a WINDOW of a constant buffer: pFirstConstant and
// pNumConstants are measured in constants of 16 bytes, and Learn requires each
// pFirstConstant entry to be a multiple of 16. ANGLE binds sub-ranges of one
// large buffer this way whenever the device exposes ID3D11DeviceContext1, so a
// guest that gets the interface and finds these unimplemented reads its uniforms
// from the wrong offset.
TEST_F(D3D11ManagerTest, Context1ConstantBufferWindowRoundTripsAndResets) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1000;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1040;
    constexpr GuestAddr BINDING = DATA_BASE + 0x1050;
    constexpr GuestAddr FIRST_IN = DATA_BASE + 0x1060;
    constexpr GuestAddr COUNT_IN = DATA_BASE + 0x1070;
    constexpr GuestAddr BUF_BACK = DATA_BASE + 0x1080;
    constexpr GuestAddr FIRST_BACK = DATA_BASE + 0x1090;
    constexpr GuestAddr COUNT_BACK = DATA_BASE + 0x10A0;

    const std::array<u32, 6> desc{4096, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr cbuffer = memory.read_u64(BUF_OUT).value();
    ASSERT_NE(cbuffer, 0u);
    ASSERT_TRUE(memory.write_u64(BINDING, cbuffer).is_ok());

    // (setter1, getter1, plain setter) per stage this frontend feeds.
    struct Stage { const char* name; usize set1; usize get1; usize plain; };
    const std::array<Stage, 3> stages{{
        {"VS", 119, 125, 7},
        {"PS", 123, 129, 16},
        {"CS", 124, 130, 71},
    }};

    for (const Stage& stage : stages) {
        constexpr u32 SLOT = 2;
        constexpr u32 FIRST = 32;  // constants, a multiple of 16
        constexpr u32 COUNT = 16;
        ASSERT_TRUE(memory.write_u32(FIRST_IN, FIRST).is_ok());
        ASSERT_TRUE(memory.write_u32(COUNT_IN, COUNT).is_ok());
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BINDING);
        set_arg(4, FIRST_IN);
        set_arg(5, COUNT_IN);
        d3d11.impl_ContextMethod(marshaler, stage.set1);

        ASSERT_TRUE(memory.write_u64(BUF_BACK, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(FIRST_BACK, 0xFFFFFFFFu).is_ok());
        ASSERT_TRUE(memory.write_u32(COUNT_BACK, 0xFFFFFFFFu).is_ok());
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BUF_BACK);
        set_arg(4, FIRST_BACK);
        set_arg(5, COUNT_BACK);
        d3d11.impl_ContextMethod(marshaler, stage.get1);
        EXPECT_EQ(memory.read_u64(BUF_BACK).value(), cbuffer)
            << stage.name << " Get*ConstantBuffers1 buffer";
        EXPECT_EQ(memory.read_u32(FIRST_BACK).value(), FIRST)
            << stage.name << " first constant";
        EXPECT_EQ(memory.read_u32(COUNT_BACK).value(), COUNT)
            << stage.name << " constant count";

        // The D3D11.0 entry point binds the whole buffer, so it must clear the
        // window a Set1 left behind. Leaving it would have the shader read from
        // an offset the guest never asked for.
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BINDING);
        d3d11.impl_ContextMethod(marshaler, stage.plain);
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BUF_BACK);
        set_arg(4, FIRST_BACK);
        set_arg(5, COUNT_BACK);
        d3d11.impl_ContextMethod(marshaler, stage.get1);
        EXPECT_EQ(memory.read_u32(FIRST_BACK).value(), 0u)
            << stage.name << ": the plain setter must reset the window";
        // Get*ConstantBuffers1 reports a whole-buffer bind as NumConstants
        // 4096, not 0 (measured, d3d11_a05_context_state_probe "vs plain
        // set(0,{cbA 4096B}) get1(0,2): ... num 4096 48"). This used to expect
        // 0; the oracle proved that wrong.
        EXPECT_EQ(memory.read_u32(COUNT_BACK).value(), 4096u)
            << stage.name << ": the plain setter must reset the count";

        // Learn: each pFirstConstant entry must be a multiple of 16; the
        // runtime drops such a call whole, so the previous window stays
        // (measured: "vs set1(cbB,first 8,num 16) not-mult-16 first: cbA
        // ... first 16 num 16"). Bind a valid window first to see that.
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BINDING);
        set_arg(4, FIRST_IN);
        set_arg(5, COUNT_IN);
        d3d11.impl_ContextMethod(marshaler, stage.set1);
        ASSERT_TRUE(memory.write_u32(FIRST_IN, 17).is_ok());
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BINDING);
        set_arg(4, FIRST_IN);
        set_arg(5, COUNT_IN);
        d3d11.impl_ContextMethod(marshaler, stage.set1);
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BUF_BACK);
        set_arg(4, FIRST_BACK);
        set_arg(5, COUNT_BACK);
        d3d11.impl_ContextMethod(marshaler, stage.get1);
        EXPECT_EQ(memory.read_u32(FIRST_BACK).value(), FIRST)
            << stage.name << ": a misaligned first constant drops the call";
        EXPECT_EQ(memory.read_u32(COUNT_BACK).value(), COUNT)
            << stage.name << ": a misaligned first constant drops the call";
        ASSERT_TRUE(memory.write_u32(FIRST_IN, FIRST).is_ok());

        // Leave the slot clean for the next stage.
        ASSERT_TRUE(memory.write_u64(BINDING, 0).is_ok());
        set_arg(0, context);
        set_arg(1, SLOT);
        set_arg(2, 1);
        set_arg(3, BINDING);
        d3d11.impl_ContextMethod(marshaler, stage.plain);
        ASSERT_TRUE(memory.write_u64(BINDING, cbuffer).is_ok());
    }
}

// ID3D11Device5::CreateFence and the ID3D11Fence interface, plus
// ID3D11DeviceContext4::Signal / Wait at slots 147 and 148.
//
// CONTEXT_METHOD_COUNT was 147 -- Context3's count -- while QI accepts the
// Context4 IID, so a guest could obtain the interface and find both slots null
// in its vtable. A call through either jumped to address zero.
//
// CreateFence also had the wrong stdcall cleanup: its InitialValue is a UINT64,
// which is two stack slots on x86, so the table's 20 bytes left the guest stack
// four bytes off on every call regardless of the return value.
TEST_F(D3D11ManagerTest, CreateFenceAndSignalWaitAreReachableAndOrdered) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr IID_BUF = DATA_BASE + 0x1000;
    constexpr GuestAddr FENCE_OUT = DATA_BASE + 0x1040;

    // ID3D11Fence {affde9d1-1df7-4bb7-8a34-0f46251dab80}, little-endian.
    const std::array<u8, 16> fence_iid{
        0xD1, 0xE9, 0xFD, 0xAF, 0xF7, 0x1D, 0xB7, 0x4B,
        0x8A, 0x34, 0x0F, 0x46, 0x25, 0x1D, 0xAB, 0x80};
    ASSERT_TRUE(memory.write_bytes(
        IID_BUF, reinterpret_cast<const Byte*>(fence_iid.data()),
        fence_iid.size()).is_ok());

    // These tests run the 64-bit dispatch, where InitialValue is one argument
    // slot: (this, InitialValue, Flags, riid, ppFence).
    constexpr u64 INITIAL = 0x1'0000'0007ull;  // wider than 32 bits on purpose
    ASSERT_TRUE(memory.write_u64(FENCE_OUT, 0).is_ok());
    set_arg(0, device);
    set_arg(1, INITIAL);
    set_arg(2, 0);  // D3D11_FENCE_FLAG_NONE
    set_arg(3, IID_BUF);
    set_arg(4, FENCE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 68);
    if (cpu.gpr.rax != d3d11::S_OK) {
        GTEST_SKIP() << "this backend has no timeline fences";
    }
    const GuestAddr fence = memory.read_u64(FENCE_OUT).value();
    ASSERT_NE(fence, 0u);

    // GetCompletedValue returns UINT64 by value and must carry the full width:
    // an initial value above 2^32 is exactly what a 32-bit-only return loses.
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, INITIAL)
        << "GetCompletedValue truncated the 64-bit timeline value";

    // A non-NONE flag promises sharing this frontend cannot provide, so it must
    // be refused rather than accepted and silently unshared.
    ASSERT_TRUE(memory.write_u64(FENCE_OUT, 0).is_ok());
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, 0x2);  // D3D11_FENCE_FLAG_SHARED
    set_arg(3, IID_BUF);
    set_arg(4, FENCE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 68);
    EXPECT_NE(cpu.gpr.rax, d3d11::S_OK) << "SHARED must not be claimed";
    EXPECT_EQ(memory.read_u64(FENCE_OUT).value(), 0u);

    // Reachability, which calling the dispatcher directly cannot test: the
    // GUEST reaches these through the vtable, and before the count was corrected
    // it held 147 entries while QI accepted the Context4 IID. Slots 147 and 148
    // were null, so a guest call jumped to address zero.
    const GuestAddr context_vtable = memory.read_u64(context).value();
    ASSERT_NE(context_vtable, 0u);
    for (const usize vtable_slot : {usize{147}, usize{148}}) {
        const auto entry = memory.read_u64(
            context_vtable + vtable_slot * sizeof(u64));
        ASSERT_TRUE(entry.is_ok());
        EXPECT_NE(entry.value(), 0u)
            << "context vtable slot " << vtable_slot
            << " is null; a guest calling it jumps to address zero";
    }

    set_arg(0, context);
    set_arg(1, fence);
    set_arg(2, INITIAL + 5);
    d3d11.impl_ContextMethod(marshaler, 147);  // Signal
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "Signal";
    set_arg(0, context);
    set_arg(1, fence);
    set_arg(2, INITIAL + 5);
    d3d11.impl_ContextMethod(marshaler, 148);  // Wait
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "Wait";

    // A signal is an ordering operation, so it must be recorded rather than
    // applied: the timeline has not moved just because the call returned.
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, INITIAL)
        << "Signal applied immediately instead of being recorded in order";

    // An address that is not a live fence is a caller error on both slots.
    set_arg(0, context);
    set_arg(1, fence + 0x1000);
    set_arg(2, 1);
    d3d11.impl_ContextMethod(marshaler, 147);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // QI must accept the fence's own IID and IUnknown, and refuse others.
    constexpr GuestAddr QI_OUT = DATA_BASE + 0x1080;
    ASSERT_TRUE(memory.write_u64(QI_OUT, 0).is_ok());
    set_arg(0, fence);
    set_arg(1, IID_BUF);
    set_arg(2, QI_OUT);
    d3d11.impl_FenceMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(QI_OUT).value(), fence);
    // The extra reference QI took, then the creation reference.
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "QI must take a reference";
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u) << "the last release must free the fence";
}

// ---------------------------------------------------------------------------
// Indirect draw / dispatch arguments
// ---------------------------------------------------------------------------

// Learn (DispatchIndirect / DrawInstancedIndirect / DrawIndexedInstancedIndirect):
// the argument buffer "must" carry D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, and
// the offset is DWORD-aligned. A guest that gets either wrong has a bug, and the
// honest response is to decline the call with a diagnostic -- not to execute it
// against whatever bytes are at that address, which would draw garbage geometry
// or dispatch an arbitrary number of threadgroups.
TEST_F(D3D11ManagerTest, IndirectArgumentBufferMustBeTaggedAndDwordAligned) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr CS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr CS_OUT = DATA_BASE + 0x1200;
    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1300;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1340;

    ASSERT_TRUE(memory.write_bytes(CS_BYTECODE, cs_lifetime,
                                   sizeof(cs_lifetime)).is_ok());
    set_arg(0, device);
    set_arg(1, CS_BYTECODE);
    set_arg(2, sizeof(cs_lifetime));
    set_arg(3, 0);
    set_arg(4, CS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(CS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 69); // CSSetShader

    // ByteWidth, Usage(DEFAULT), Bind(UAV), CPUAccess, Misc, Stride. A real
    // indirect-args buffer is UAV-bound because a compute shader writes it.
    const auto create_buffer = [&](u32 misc) {
        const std::array<u32, 6> desc{32, 0, 0x80, 0, misc, 0};
        EXPECT_TRUE(memory.write_bytes(
            BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, BUF_DESC);
        set_arg(2, 0);
        set_arg(3, BUF_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(BUF_OUT).value();
    };
    const GuestAddr untagged = create_buffer(0);
    const GuestAddr tagged = create_buffer(0x10); // DRAWINDIRECT_ARGS
    ASSERT_NE(untagged, 0u);
    ASSERT_NE(tagged, 0u);

    const auto dispatch_indirect = [&](GuestAddr args, u32 offset) {
        set_arg(0, context);
        set_arg(1, args);
        set_arg(2, offset);
        d3d11.impl_ContextMethod(marshaler, 42); // DispatchIndirect
    };

    // Every one of these is void in D3D, so the guest cannot be told by a
    // return value; the observable difference is whether GPU work happened.
    const unsigned before = backend.state.dispatches;
    dispatch_indirect(untagged, 0);
    EXPECT_EQ(backend.state.dispatches, before)
        << "a buffer without DRAWINDIRECT_ARGS must not dispatch";
    dispatch_indirect(tagged, 2);
    EXPECT_EQ(backend.state.dispatches, before)
        << "a non-DWORD-aligned offset must not dispatch";
    dispatch_indirect(tagged, 64);
    EXPECT_EQ(backend.state.dispatches, before)
        << "an offset past the end of the buffer must not dispatch";
    dispatch_indirect(tagged, 24);
    EXPECT_EQ(backend.state.dispatches, before)
        << "a 12-byte block that overruns the 32-byte buffer (24 + 12 > 32) "
           "must not dispatch (WARP: IndirectArgumentBlocksFollowTheMeasured"
           "Bounds)";
    dispatch_indirect(0, 0);
    EXPECT_EQ(backend.state.dispatches, before)
        << "a null argument buffer must not dispatch";

    dispatch_indirect(tagged, 16);
    EXPECT_EQ(backend.state.dispatches, before + 1)
        << "a tagged buffer at an aligned, in-range offset dispatches";
}

// A direct Dispatch of zero groups is still dropped, but the indirect form must
// not inherit that rule: the counts live in GPU memory and zero is a value the
// GPU is entitled to have written. Treating it as "nothing to do" is the
// contract; dropping the call would instead skip a frame's real work whenever
// the CPU-side struct happened to be zeroed -- which for an indirect call it
// always is, because the CPU has nothing to put there.
TEST_F(D3D11ManagerTest, IndirectDispatchIgnoresTheCpuSideGroupCounts) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr CS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr CS_OUT = DATA_BASE + 0x1200;
    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1300;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1340;

    ASSERT_TRUE(memory.write_bytes(CS_BYTECODE, cs_lifetime,
                                   sizeof(cs_lifetime)).is_ok());
    set_arg(0, device);
    set_arg(1, CS_BYTECODE);
    set_arg(2, sizeof(cs_lifetime));
    set_arg(3, 0);
    set_arg(4, CS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(CS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 69);

    const std::array<u32, 6> desc{32, 0, 0x80, 0, 0x10, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr args = memory.read_u64(BUF_OUT).value();

    // Direct Dispatch(0, 0, 0) is dropped.
    const unsigned before = backend.state.dispatches;
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 41); // Dispatch
    EXPECT_EQ(backend.state.dispatches, before);

    // DispatchIndirect reaches the backend regardless.
    set_arg(0, context);
    set_arg(1, args);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 42);
    EXPECT_EQ(backend.state.dispatches, before + 1);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
