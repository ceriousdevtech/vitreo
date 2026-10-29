// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_surface.hpp
/// @brief The fixture ladder D3D11SurfaceTest > D3D11ResourceOpsTest > D3D11OptionalCapsTest and D3D11FixedFunctionTest.

#include "d3d11_test_streamout.hpp"

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// The rest of the D3D11 interface surface. Expected values are what Windows
// returned (tools/oracle_probe/d3d11_interface_surface_probe.c) except where a
// comment says the answer follows from a cap this device does not report.
// ---------------------------------------------------------------------------

class D3D11SurfaceTest : public D3D11StreamOutTest {
protected:
    static constexpr GuestAddr DESC = SO_BASE + 0xA00;   // < 0x200 bytes
    static constexpr GuestAddr OUT = SO_BASE + 0xC00;
    static constexpr GuestAddr OUT2 = SO_BASE + 0xC40;

    u32 device_call(usize slot, std::initializer_list<u64> args) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, device);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_DeviceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void write_words(GuestAddr at, std::initializer_list<u32> words) {
        u32 i = 0;
        for (const u32 w : words) {
            ASSERT_TRUE(memory.write_u32(at + 4 * i++, w).is_ok());
        }
    }
    GuestAddr texture(u32 mips = 1, u32 bind = 0x28 /* SRV|RTV */) {
        write_words(DESC, {16, 16, mips, 1, 28, 1, 0, 0, bind, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }
};

class D3D11ResourceOpsTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr A6 = 0x20000;  // two scratch pages
    static constexpr GuestAddr A6_DESC = A6;
    static constexpr GuestAddr A6_VIEW = A6 + 0x100;
    static constexpr GuestAddr A6_OUT = A6 + 0x200;
    static constexpr GuestAddr A6_ARG = A6 + 0x300;
    static constexpr GuestAddr A6_INIT = A6 + 0x400;
    static constexpr GuestAddr A6_DATA = A6 + 0x800;

    void SetUp() override {
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(A6, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
    }
    GuestAddr tex2d(std::initializer_list<u32> desc, GuestAddr init = 0) {
        write_words(A6_DESC, desc);
        EXPECT_EQ(device_call(5, {A6_DESC, init, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }
    GuestAddr tex3d(std::initializer_list<u32> desc, GuestAddr init = 0) {
        write_words(A6_DESC, desc);
        EXPECT_EQ(device_call(6, {A6_DESC, init, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }
    GuestAddr buffer(std::initializer_list<u32> desc, GuestAddr init = 0) {
        write_words(A6_DESC, desc);
        EXPECT_EQ(device_call(3, {A6_DESC, init, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }
    /// Create*View(resource, desc words or NULL) through device slot @p slot.
    GuestAddr make_view(usize slot, GuestAddr resource,
                        std::initializer_list<u32> desc) {
        GuestAddr desc_address = 0;
        if (desc.size() != 0) {
            write_words(A6_VIEW, desc);
            desc_address = A6_VIEW;
        }
        EXPECT_EQ(device_call(slot, {resource, desc_address, A6_OUT}),
                  d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }
    /// D3D11_SUBRESOURCE_DATA{pSysMem, pitch, slice pitch} at A6_INIT over
    /// @p bytes written at A6_DATA.
    GuestAddr init_data(const std::vector<u8>& bytes, u32 pitch,
                        u32 slice_pitch = 0) {
        EXPECT_TRUE(memory.write_bytes(A6_DATA, bytes.data(), bytes.size())
                        .is_ok());
        EXPECT_TRUE(memory.write_u64(A6_INIT, A6_DATA).is_ok());
        write_words(A6_INIT + 8, {pitch, slice_pitch});
        return A6_INIT;
    }
    void clear_uav(bool is_float, GuestAddr view,
                   std::array<u32, 4> values) {
        write_words(A6_ARG, {values[0], values[1], values[2], values[3]});
        context_call(is_float ? 52 : 51, {view, A6_ARG});
    }
    static u32 bits(float value) { return std::bit_cast<u32>(value); }
};

class D3D11OptionalCapsTest : public D3D11ResourceOpsTest {
protected:
    static constexpr GuestAddr BLOB = A6 + 0xC00;   // shader bytecode
    static constexpr GuestAddr PTRS = A6 + 0x1C00;
    static constexpr GuestAddr WINDOW = A6 + 0x1D00;

    GuestAddr compute_shader(const unsigned char* blob, usize size) {
        EXPECT_TRUE(memory.write_bytes(BLOB, blob, size).is_ok());
        EXPECT_TRUE(memory.write_u64(A6_OUT, 0xABABABABull).is_ok());
        last_hr = device_call(18, {BLOB, size, 0, A6_OUT});
        return memory.read_u64(A6_OUT).value();
    }
    std::array<u32, 14> options() {
        for (u32 i = 0; i < 56; i += 4) {
            EXPECT_TRUE(memory.write_u32(A6_ARG + i, 0xCDCDCDCDu).is_ok());
        }
        EXPECT_EQ(device_call(33, {5, A6_ARG, 56}), d3d11::S_OK);
        std::array<u32, 14> out{};
        for (u32 i = 0; i < 14; ++i) {
            out[i] = memory.read_u32(A6_ARG + 4 * i).value();
        }
        return out;
    }
    u32 last_hr{0};
};

// ---------------------------------------------------------------------------
// Area 07, fixed function (.docs/audits/2026-09-28-d3d11-contract-audit/
// area-07-fixed-function.md). Expected values are Windows measurements from
// tools/oracle_probe/d3d11_a07_*_probe.expected or D3D11.3 functional-spec
// rules; the shaders are that probe's fxc output.
// ---------------------------------------------------------------------------

#include "../../oracle_data/d3d11_a07_shaders.h"

class D3D11FixedFunctionTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr PAGE = SO_BASE + 0x1000;   // shader bytecode
    static constexpr GuestAddr ELEMS = PAGE + 0x1000;     // input elements
    static constexpr GuestAddr NAMES = PAGE + 0x1200;
    static constexpr GuestAddr DATA = PAGE + 0x1400;      // initial data
    static constexpr GuestAddr INIT = PAGE + 0x1C00;      // SUBRESOURCE_DATA
    static constexpr GuestAddr BIG = PAGE + 0x1D00;       // blend desc
    static constexpr GuestAddr PTRS = PAGE + 0x1F00;      // pointer arrays

    void SetUp() override {
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(PAGE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    GuestAddr shader(usize method, const unsigned char* blob, usize size) {
        EXPECT_TRUE(memory.write_bytes(PAGE, blob, size).is_ok());
        EXPECT_EQ(device_call(method, {PAGE, size, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    struct Element {
        const char* name;
        u32 format;
        u32 slot;
        u32 offset;
        u32 per_instance;
        u32 step;
    };
    /// CreateInputLayout against @p vs; returns the HRESULT, layout at OUT.
    u32 input_layout(const std::vector<Element>& elements,
                     const unsigned char* vs, usize vs_size) {
        GuestAddr name_at = NAMES;
        for (usize i = 0; i < elements.size(); ++i) {
            const auto& e = elements[i];
            const std::string text(e.name);
            EXPECT_TRUE(memory.write_bytes(
                name_at, reinterpret_cast<const Byte*>(text.c_str()),
                text.size() + 1).is_ok());
            const GuestAddr base = ELEMS + i * 32;
            EXPECT_TRUE(memory.write_u64(base, name_at).is_ok());
            write_words(base + 8, {0, e.format, e.slot, e.offset,
                                   e.per_instance, e.step});
            name_at += 16;
        }
        EXPECT_TRUE(memory.write_bytes(PAGE, vs, vs_size).is_ok());
        return device_call(11, {ELEMS, elements.size(), PAGE, vs_size, OUT});
    }

    /// DEFAULT buffer of @p bind holding @p words.
    GuestAddr buffer(u32 bind, const std::vector<u32>& words, u32 bytes = 0) {
        if (bytes == 0) bytes = static_cast<u32>(words.size() * 4);
        for (usize i = 0; i < words.size(); ++i) {
            EXPECT_TRUE(memory.write_u32(DATA + 4 * i, words[i]).is_ok());
        }
        EXPECT_TRUE(memory.write_u64(INIT, DATA).is_ok());
        write_words(INIT + 8, {0, 0, 0, 0});
        write_words(DESC, {bytes, 0, bind, 0, 0, 0});
        EXPECT_EQ(device_call(3, {DESC, INIT, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// A 16x16 render target of @p format bound alone, with viewport 0 over
    /// it; @p dsv optional.
    void bind_target(u32 format = 28, GuestAddr dsv = 0) {
        write_words(DESC, {16, 16, 1, 1, format, 1, 0, 0, 0x20, 0, 0});
        ASSERT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        ASSERT_EQ(device_call(9, {memory.read_u64(OUT).value(), 0, OUT2}),
                  d3d11::S_OK);
        ASSERT_TRUE(memory.write_u64(PTRS, memory.read_u64(OUT2).value())
                        .is_ok());
        context_call(33, {1, PTRS, dsv});
        write_words(DESC, {0, 0, 0x41800000u, 0x41800000u, 0, 0x3F800000u});
        context_call(44, {1, DESC});
    }

    /// A 16x16 depth target of @p format and its view.
    GuestAddr depth_view(u32 format) {
        write_words(DESC, {16, 16, 1, 1, format, 1, 0, 0, 0x40, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        EXPECT_EQ(device_call(10, {memory.read_u64(OUT).value(), 0, OUT2}),
                  d3d11::S_OK);
        return memory.read_u64(OUT2).value();
    }

    void use_shaders(const unsigned char* vs, usize vs_size,
                     const unsigned char* ps, usize ps_size) {
        context_call(11, {shader(12, vs, vs_size), 0, 0});
        context_call(9, {ps != nullptr ? shader(15, ps, ps_size) : 0, 0, 0});
    }

    /// Vertex streams from slot 0 (buffer, stride, offset each).
    void vertex_buffers(const std::vector<std::array<u64, 3>>& streams) {
        for (usize i = 0; i < streams.size(); ++i) {
            EXPECT_TRUE(memory.write_u64(PTRS + 8 * i, streams[i][0]).is_ok());
            EXPECT_TRUE(memory.write_u32(PTRS + 0x40 + 4 * i,
                                         static_cast<u32>(streams[i][1]))
                            .is_ok());
            EXPECT_TRUE(memory.write_u32(PTRS + 0x60 + 4 * i,
                                         static_cast<u32>(streams[i][2]))
                            .is_ok());
        }
        context_call(18, {0, streams.size(), PTRS, PTRS + 0x40, PTRS + 0x60});
    }

    GuestAddr rasterizer(std::initializer_list<u32> words) {
        write_words(DESC, words);
        EXPECT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// The draw's index stream as 32-bit words.
    std::vector<u32> drawn_indices() const {
        const auto& bytes = backend.state.last_draw_index_bytes;
        std::vector<u32> out(bytes.size() / 4);
        std::memcpy(out.data(), bytes.data(), out.size() * 4);
        return out;
    }

    template <typename T>
    T push_word(const std::vector<u8>& block, usize offset) const {
        T value{};
        EXPECT_GE(block.size(), offset + sizeof(T));
        if (block.size() >= offset + sizeof(T)) {
            std::memcpy(&value, block.data() + offset, sizeof(T));
        }
        return value;
    }

    /// The uint ATTR (slot 0) + INST (slot 1) layout of vs_pt, 64 elements
    /// each of 1000 + i and 2000 + i, as d3d11_a07_ia_probe binds them.
    void bind_attr_inst_streams(u32 step, bool bind_inst = true) {
        ASSERT_EQ(input_layout({{"ATTR", 42, 0, 0, 0, 0},
                                {"INST", 42, 1, 0, 1, step}},
                               g_vs_pt, sizeof(g_vs_pt)),
                  d3d11::S_OK);
        context_call(17, {memory.read_u64(OUT).value()});
        std::vector<u32> attr(64), inst(64);
        for (u32 i = 0; i < 64; ++i) {
            attr[i] = 1000 + i;
            inst[i] = 2000 + i;
        }
        const GuestAddr a = buffer(0x1, attr);
        const GuestAddr b = bind_inst ? buffer(0x1, inst) : 0;
        vertex_buffers({{a, 4, 0}, {b, 4, 0}});
        use_shaders(g_vs_pt, sizeof(g_vs_pt), g_ps_v, sizeof(g_ps_v));
    }
};

}  // namespace vitreo::winapi::d3d11_test
