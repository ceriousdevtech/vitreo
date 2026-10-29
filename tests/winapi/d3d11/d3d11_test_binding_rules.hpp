// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_binding_rules.hpp
/// @brief D3D11BindingRulesTest: the binding-rule fixture the input-layout rules derive from.

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {

// =============================================================================
// Device-context binding rules: the output-merger UAV model, read/write
// hazards, 32 IA slots, *SetConstantBuffers1 windows, CreateInputLayout
// signature validation. Every expected value is a Windows measurement from
// tools/oracle_probe/d3d11_a05_context_state_probe.expected (quoted as "a05")
// or d3d11_a05b_bind_rules_probe.expected ("a05b").
// =============================================================================


class D3D11BindingRulesTest : public D3D11ManagerTest {
protected:
    static constexpr GuestAddr BASE = DATA_BASE + 0x8000;
    static constexpr GuestAddr DESC = BASE;
    static constexpr GuestAddr OUT = BASE + 0x100;
    static constexpr GuestAddr PTRS_A = BASE + 0x200;
    static constexpr GuestAddr PTRS_B = BASE + 0x300;
    static constexpr GuestAddr U32_A = BASE + 0x400;
    static constexpr GuestAddr U32_B = BASE + 0x480;
    static constexpr GuestAddr GET = BASE + 0x500;
    static constexpr GuestAddr GET_U32_A = BASE + 0x700;
    static constexpr GuestAddr GET_U32_B = BASE + 0x780;
    static constexpr GuestAddr NAMES = BASE + 0x800;
    static constexpr GuestAddr ELEMENTS = BASE + 0x900;
    static constexpr GuestAddr BLOB = BASE + 0x1000;

    // DXGI formats, bind flags and view dimensions (d3d11.h / dxgiformat.h).
    static constexpr u32 RGBA8 = 28, R32_UINT = 42, R16_UINT = 57;
    static constexpr u32 R24G8_TYPELESS = 44, D24S8 = 45,
                         R24_UNORM_X8 = 46, X24_G8_UINT = 47;
    static constexpr u32 BIND_VB = 0x1, BIND_IB = 0x2, BIND_SRV = 0x8,
                         BIND_SO = 0x10, BIND_RT = 0x20, BIND_DS = 0x40,
                         BIND_UAV = 0x80;
    static constexpr u32 COLOUR = BIND_SRV | BIND_RT | BIND_UAV;
    static constexpr u32 KEEP = 0xFFFFFFFFu;

    // (setter, getter) slots per stage: VS HS DS GS PS CS.
    static constexpr std::array<std::pair<usize, usize>, 6> kSrvSlots{{
        {25, 84}, {59, 97}, {63, 101}, {31, 87}, {8, 73}, {67, 105}}};

    void SetUp() override {
        D3D11ManagerTest::SetUp();
        ASSERT_TRUE(memory.map_region(BASE, 0x3000,
                                      MemoryProtection::ReadWrite).is_ok());
        create_default_device();
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        marshaler.attach_cpu(cpu);
    }

    u64 call(bool on_device, usize slot, std::initializer_list<u64> args) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, on_device ? device : context);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        if (on_device) d3d11.impl_DeviceMethod(marshaler, slot);
        else d3d11.impl_ContextMethod(marshaler, slot);
        return cpu.gpr.rax;
    }
    u64 dev(usize slot, std::initializer_list<u64> args) {
        return call(true, slot, args);
    }
    u64 ctx(usize slot, std::initializer_list<u64> args) {
        return call(false, slot, args);
    }
    void words(GuestAddr at, std::initializer_list<u32> values) {
        for (const u32 v : values) {
            ASSERT_TRUE(memory.write_u32(at, v).is_ok());
            at += 4;
        }
    }
    void ptrs(GuestAddr at, std::initializer_list<GuestAddr> values) {
        for (const GuestAddr v : values) {
            ASSERT_TRUE(memory.write_u64(at, v).is_ok());
            at += 8;
        }
    }
    GuestAddr read_ptr(GuestAddr at) { return memory.read_u64(at).value(); }
    u32 read32(GuestAddr at) { return memory.read_u32(at).value(); }

    GuestAddr created(usize slot, GuestAddr first, GuestAddr second) {
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        EXPECT_EQ(dev(slot, {first, second, OUT}), d3d11::S_OK) << slot;
        return read_ptr(OUT);
    }
    GuestAddr texture2d(u32 size, u32 mips, u32 slices, u32 format, u32 bind,
                        u32 samples = 1) {
        words(DESC, {size, size, mips, slices, format, samples, 0, 0, bind, 0,
                     0});
        return created(5, DESC, 0);
    }
    GuestAddr texture3d(u32 size, u32 depth, u32 format, u32 bind) {
        words(DESC, {size, size, depth, 1, format, 0, bind, 0, 0});
        return created(6, DESC, 0);
    }
    GuestAddr buffer(u32 bytes, u32 bind) {
        words(DESC, {bytes, 0, bind, 0, 0, 0});
        return created(3, DESC, 0);
    }
    GuestAddr view(usize slot, GuestAddr resource,
                   std::initializer_list<u32> desc = {}) {
        if (desc.size() == 0) return created(slot, resource, 0);
        words(DESC, desc);
        return created(slot, resource, DESC);
    }
    GuestAddr srv(GuestAddr r, std::initializer_list<u32> d = {}) {
        return view(7, r, d);
    }
    GuestAddr uav(GuestAddr r, std::initializer_list<u32> d = {}) {
        return view(8, r, d);
    }
    GuestAddr rtv(GuestAddr r, std::initializer_list<u32> d = {}) {
        return view(9, r, d);
    }
    GuestAddr dsv(GuestAddr r, u32 flags) {
        return view(10, r, {D24S8, 3 /*TEXTURE2D*/, flags, 0, 0, 0});
    }

    /// OMSetRenderTargetsAndUnorderedAccessViews with inline arrays.
    void om(u32 num_rtvs, std::initializer_list<GuestAddr> rtvs,
            GuestAddr depth, u32 start, u32 num_uavs,
            std::initializer_list<GuestAddr> uavs,
            GuestAddr counts = 0) {
        ptrs(PTRS_A, rtvs);
        ptrs(PTRS_B, uavs);
        ctx(34, {num_rtvs, rtvs.size() != 0 ? PTRS_A : 0, depth, start,
                 num_uavs, uavs.size() != 0 ? PTRS_B : 0, counts});
    }
    void set_rts(std::initializer_list<GuestAddr> rtvs, GuestAddr depth) {
        ptrs(PTRS_A, rtvs);
        ctx(33, {rtvs.size(), rtvs.size() != 0 ? PTRS_A : 0, depth});
    }
    void set_cs_uavs(u32 start, std::initializer_list<GuestAddr> uavs) {
        ptrs(PTRS_B, uavs);
        ctx(68, {start, uavs.size(), PTRS_B, 0});
    }
    void set_srvs(usize setter, u32 start,
                  std::initializer_list<GuestAddr> views) {
        ptrs(PTRS_A, views);
        ctx(setter, {start, views.size(), PTRS_A});
    }
    void set_srv_all_stages(u32 start, std::initializer_list<GuestAddr> views) {
        for (const auto& stage : kSrvSlots) set_srvs(stage.first, start, views);
    }

    struct OmState {
        std::array<GuestAddr, 8> rtv{};
        GuestAddr dsv{0};
        std::array<GuestAddr, 8> uav{};
    };
    OmState om_get() {
        OmState out;
        ctx(90, {8, GET, GET + 0x40, 0, 8, GET + 0x80});
        for (usize i = 0; i < 8; ++i) out.rtv[i] = read_ptr(GET + i * 8);
        out.dsv = read_ptr(GET + 0x40);
        for (usize i = 0; i < 8; ++i) out.uav[i] = read_ptr(GET + 0x80 + i * 8);
        return out;
    }
    std::vector<GuestAddr> get_array(usize getter, u32 start, u32 n) {
        ctx(getter, {start, n, GET});
        std::vector<GuestAddr> out;
        for (u32 i = 0; i < n; ++i) out.push_back(read_ptr(GET + i * 8));
        return out;
    }
    /// Slot @p slot of every stage's SRV array, VS..CS.
    std::array<GuestAddr, 6> srv_on_all_stages(u32 slot) {
        std::array<GuestAddr, 6> out{};
        for (usize s = 0; s < kSrvSlots.size(); ++s) {
            out[s] = get_array(kSrvSlots[s].second, slot, 1)[0];
        }
        return out;
    }
    static std::array<GuestAddr, 6> all6(GuestAddr v) {
        return {v, v, v, v, v, v};
    }
    struct VbState {
        std::vector<GuestAddr> buffers;
        std::vector<u32> strides, offsets;
    };
    VbState vb_get(u32 start, u32 n) {
        ctx(79, {start, n, GET, GET_U32_A, GET_U32_B});
        VbState out;
        for (u32 i = 0; i < n; ++i) {
            out.buffers.push_back(read_ptr(GET + i * 8));
            out.strides.push_back(read32(GET_U32_A + i * 4));
            out.offsets.push_back(read32(GET_U32_B + i * 4));
        }
        return out;
    }
    void vb_set(u32 start, std::initializer_list<GuestAddr> buffers,
                std::initializer_list<u32> strides,
                std::initializer_list<u32> offsets) {
        ptrs(PTRS_A, buffers);
        {
            GuestAddr at = U32_A;
            for (const u32 v : strides) { (void)memory.write_u32(at, v); at += 4; }
            at = U32_B;
            for (const u32 v : offsets) { (void)memory.write_u32(at, v); at += 4; }
        }
        ctx(18, {start, buffers.size(), PTRS_A, U32_A, U32_B});
    }
    struct IbState { GuestAddr buffer; u32 format; u32 offset; };
    IbState ib_get() {
        ctx(80, {GET, GET_U32_A, GET_U32_B});
        return {read_ptr(GET), read32(GET_U32_A), read32(GET_U32_B)};
    }
    std::vector<GuestAddr> so_get() {
        ctx(93, {4, GET});
        std::vector<GuestAddr> out;
        for (u32 i = 0; i < 4; ++i) out.push_back(read_ptr(GET + i * 8));
        return out;
    }
    void so_set(std::initializer_list<GuestAddr> targets) {
        ptrs(PTRS_A, targets);
        words(U32_A, {0, 0, 0, 0});
        ctx(37, {targets.size(), PTRS_A, U32_A});
    }

    Marshaler marshaler{memory};
    GuestAddr device{0};
    GuestAddr context{0};
};

}  // namespace vitreo::winapi::d3d11_test
