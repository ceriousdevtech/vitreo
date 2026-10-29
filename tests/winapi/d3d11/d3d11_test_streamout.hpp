// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_streamout.hpp
/// @brief Stream-output fixture (D3D11StreamOutTest) and the DXBC builders it uses.

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// Stream output (11.18)
//
// Every expected HRESULT and count below is what Windows returned for the same
// call: tools/oracle_probe/d3d11_stream_out_probe.expected.
// ---------------------------------------------------------------------------


struct SignatureElementSpec {
    const char* name;
    u32 index;
    u32 system_value;
    u32 component_type;
    u32 reg;
    u32 mask;
};

inline void push_u32(std::vector<u8>& out, u32 word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        out.push_back(static_cast<u8>((word >> (byte * 8)) & 0xFFu));
    }
}

[[nodiscard]] inline std::vector<u8> signature_chunk(
    const std::vector<SignatureElementSpec>& elements) {
    std::vector<u8> data;
    push_u32(data, static_cast<u32>(elements.size()));
    push_u32(data, 8u);
    std::vector<u8> strings;
    const u32 strings_at = 8u + 24u * static_cast<u32>(elements.size());
    for (const auto& e : elements) {
        push_u32(data, strings_at + static_cast<u32>(strings.size()));
        push_u32(data, e.index);
        push_u32(data, e.system_value);
        push_u32(data, e.component_type);
        push_u32(data, e.reg);
        push_u32(data, e.mask | (e.mask << 8));
        for (const char* c = e.name; *c != 0; ++c) strings.push_back(*c);
        strings.push_back(0);
    }
    while (strings.size() % 4 != 0) strings.push_back(0);
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

/// A DXBC container with ISGN, OSGN and SHEX chunks.
[[nodiscard]] inline std::vector<u8> dxbc_with_signatures(
    const std::vector<u32>& tokens,
    const std::vector<SignatureElementSpec>& inputs,
    const std::vector<SignatureElementSpec>& outputs) {
    std::vector<u8> code;
    for (const u32 t : tokens) push_u32(code, t);
    const std::array<std::pair<u32, std::vector<u8>>, 3> chunks{{
        {0x4E475349u, signature_chunk(inputs)},   // ISGN
        {0x4E47534Fu, signature_chunk(outputs)},  // OSGN
        {0x58454853u, code},                      // SHEX
    }};
    u32 at = 32u + 4u * static_cast<u32>(chunks.size());
    std::vector<u32> offsets;
    for (const auto& [_, data] : chunks) {
        offsets.push_back(at);
        at += 8u + static_cast<u32>(data.size());
    }
    std::vector<u8> blob;
    push_u32(blob, 0x43425844u);
    for (int i = 0; i < 4; ++i) push_u32(blob, 0u);
    push_u32(blob, 1u);
    push_u32(blob, at);
    push_u32(blob, static_cast<u32>(chunks.size()));
    for (const u32 o : offsets) push_u32(blob, o);
    for (const auto& [fourcc, data] : chunks) {
        push_u32(blob, fourcc);
        push_u32(blob, static_cast<u32>(data.size()));
        blob.insert(blob.end(), data.begin(), data.end());
    }
    return blob;
}

/// vs_4_0 that reads only SV_VertexID -- the probe's vertex shader:
///   o0 SV_Position = (0,0,0,1)
///   o1 TEXCOORD0   = v0.xxxx
///   o2 TEXCOORD1.xy = v0.xx
/// No input layout and no vertex buffer are needed to draw it.
[[nodiscard]] inline std::vector<u8> vertex_id_streaming_vs() {
    const std::vector<u32> tokens{
        0x00010040u, 31u,
        0x04000060u, 0x00101012u, 0u, 6u,          // dcl_input_sgv v0.x, vertex_id
        0x04000067u, 0x001020F2u, 0u, 1u,          // dcl_output_siv o0, position
        0x03000065u, 0x001020F2u, 1u,              // dcl_output o1.xyzw
        0x03000065u, 0x00102032u, 2u,              // dcl_output o2.xy
        0x08000036u, 0x001020F2u, 0u,              // mov o0, l(0,0,0,1)
        0x00004002u, 0u, 0u, 0u, 0x3F800000u,
        0x05000036u, 0x001020F2u, 1u, 0x00101006u, 0u,  // mov o1, v0.xxxx
        0x05000036u, 0x00102032u, 2u, 0x00101006u, 0u,  // mov o2.xy, v0.xx
        0x0100003Eu,
    };
    return dxbc_with_signatures(
        tokens, {{"SV_VertexID", 0, 6, 1, 0, 0x1}},
        {{"SV_Position", 0, 1, 3, 0, 0xF},
         {"TEXCOORD", 0, 0, 3, 1, 0xF},
         {"TEXCOORD", 1, 0, 3, 2, 0x3}});
}

struct SoEntrySpec {
    u32 stream;
    const char* name;  // nullptr = gap
    u32 index;
    u8 start;
    u8 count;
    u8 slot;
};


class D3D11StreamOutTest : public D3D11ManagerTest {
protected:
    static constexpr GuestAddr SO_BASE = DATA_BASE + 0x4000;
    static constexpr GuestAddr VS_BYTECODE = SO_BASE;            // < 0x400
    static constexpr GuestAddr DECL = SO_BASE + 0x400;
    static constexpr GuestAddr NAMES = SO_BASE + 0x600;
    static constexpr GuestAddr STRIDES = SO_BASE + 0x700;
    static constexpr GuestAddr SHADER_OUT = SO_BASE + 0x720;
    static constexpr GuestAddr LINKAGE = SO_BASE + 0x728;
    static constexpr GuestAddr TARGETS = SO_BASE + 0x740;
    static constexpr GuestAddr OFFSETS = SO_BASE + 0x780;
    static constexpr GuestAddr GET_OUT = SO_BASE + 0x7A0;
    static constexpr GuestAddr BUFFER_DESC = SO_BASE + 0x7E0;
    static constexpr GuestAddr BUFFER_OUT_ = SO_BASE + 0x800;
    static constexpr u64 POISON = 0xA5A5A5A5A5A5A5A5ull;

    void SetUp() override {
        D3D11ManagerTest::SetUp();
        // Stream-output counts are made on the GPU; the fake runs the
        // accounting kernels' C++ models over real buffer contents.
        backend.state.live_buffers = true;
        // The fixture maps DATA_BASE..+0x3000; this suite needs its own page.
        ASSERT_TRUE(memory.map_region(SO_BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        create_default_device();
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        marshaler.attach_cpu(cpu);
        vs_blob = vertex_id_streaming_vs();
        ASSERT_TRUE(memory.write_bytes(
            VS_BYTECODE, reinterpret_cast<const Byte*>(vs_blob.data()),
            vs_blob.size()).is_ok());
    }

    /// CreateGeometryShaderWithStreamOutput. Returns the HRESULT; the object
    /// (or NULL) is at SHADER_OUT unless @p null_out.
    u32 create_so(const std::vector<SoEntrySpec>& entries,
                  const std::vector<u32>& strides, u32 raster,
                  bool null_out = false) {
        GuestAddr name_at = NAMES;
        for (usize i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const GuestAddr base = DECL + i * 24;
            GuestAddr name = 0;
            if (e.name != nullptr) {
                const std::string text(e.name);
                EXPECT_TRUE(memory.write_bytes(
                    name_at, reinterpret_cast<const Byte*>(text.c_str()),
                    text.size() + 1).is_ok());
                name = name_at;
                name_at += 32;
            }
            EXPECT_TRUE(memory.write_u32(base, e.stream).is_ok());
            EXPECT_TRUE(memory.write_u32(base + 4, 0).is_ok());
            EXPECT_TRUE(memory.write_u64(base + 8, name).is_ok());
            EXPECT_TRUE(memory.write_u32(base + 16, e.index).is_ok());
            const std::array<u8, 4> tail{e.start, e.count, e.slot, 0};
            EXPECT_TRUE(memory.write_bytes(
                base + 20, reinterpret_cast<const Byte*>(tail.data()), 4)
                            .is_ok());
        }
        for (usize i = 0; i < strides.size(); ++i) {
            EXPECT_TRUE(memory.write_u32(STRIDES + i * 4, strides[i]).is_ok());
        }
        EXPECT_TRUE(memory.write_u64(SHADER_OUT, POISON).is_ok());
        EXPECT_TRUE(memory.write_u64(LINKAGE, POISON).is_ok());
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, device);
        set_arg(1, so_bytecode_at);
        set_arg(2, vs_blob.size());
        set_arg(3, DECL);
        set_arg(4, entries.size());
        set_arg(5, strides.empty() ? 0 : STRIDES);
        set_arg(6, strides.size());
        set_arg(7, raster);
        set_arg(8, 0);
        set_arg(9, null_out ? 0 : SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestAddr make_so_buffer(u32 bytes, u32 bind = 0x11 /* SO|VB */) {
        const std::array<u32, 6> desc{bytes, 0, bind, 0, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, BUFFER_OUT_);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(BUFFER_OUT_).value();
    }

    /// A DRAWINDIRECT_ARGS buffer holding @p args.
    GuestAddr make_indirect_args(const std::vector<u32>& args) {
        constexpr GuestAddr INIT = SO_BASE + 0xE00;
        constexpr GuestAddr SUBRESOURCE = SO_BASE + 0xE80;
        EXPECT_TRUE(memory.write_bytes(
            INIT, reinterpret_cast<const Byte*>(args.data()),
            args.size() * 4).is_ok());
        EXPECT_TRUE(memory.write_u64(SUBRESOURCE, INIT).is_ok());
        EXPECT_TRUE(memory.write_u64(SUBRESOURCE + 8, 0).is_ok());
        const std::array<u32, 6> desc{
            static_cast<u32>(args.size() * 4), 0, 0x80 /* UAV */, 0,
            0x10 /* DRAWINDIRECT_ARGS */, 0};
        EXPECT_TRUE(memory.write_bytes(
            BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, BUFFER_DESC);
        set_arg(2, SUBRESOURCE);
        set_arg(3, BUFFER_OUT_);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(BUFFER_OUT_).value();
    }

    /// Bytecode past VS_BYTECODE's 0x400 bytes (a multi-stream program):
    /// written to a page of its own, which create_so then reads.
    void use_large_bytecode(const unsigned char* bytes, usize size) {
        constexpr GuestAddr LARGE = 0x40000;
        if (!large_mapped) {
            ASSERT_TRUE(memory.map_region(LARGE, 0x1000,
                                          MemoryProtection::ReadWrite).is_ok());
            large_mapped = true;
        }
        ASSERT_LE(size, 0x1000u);
        ASSERT_TRUE(memory.write_bytes(
            LARGE, reinterpret_cast<const Byte*>(bytes), size).is_ok());
        so_bytecode_at = LARGE;
        vs_blob.assign(bytes, bytes + size);
    }

    GuestAddr make_vertex_shader() {
        set_arg(0, device);
        set_arg(1, VS_BYTECODE);
        set_arg(2, vs_blob.size());
        set_arg(3, 0);
        set_arg(4, GET_OUT);
        d3d11.impl_DeviceMethod(marshaler, 12);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(GET_OUT).value();
    }

    void context_call(usize slot, std::initializer_list<u64> args) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, context);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_ContextMethod(marshaler, slot);
    }

    void so_set_targets(const std::vector<GuestAddr>& buffers,
                        const std::vector<u32>& offsets) {
        for (usize i = 0; i < buffers.size(); ++i) {
            EXPECT_TRUE(memory.write_u64(TARGETS + i * 8, buffers[i]).is_ok());
            EXPECT_TRUE(memory.write_u32(OFFSETS + i * 4, offsets[i]).is_ok());
        }
        context_call(37, {buffers.size(), buffers.empty() ? 0 : TARGETS,
                          buffers.empty() ? 0 : OFFSETS});
    }

    u32 add_ref(usize method_count_slot, GuestAddr object,
                void (D3D11Manager::*impl)(Marshaler&, usize)) {
        set_arg(0, object);
        (d3d11.*impl)(marshaler, method_count_slot);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestAddr device{0};
    GuestAddr context{0};
    Marshaler marshaler{memory};
    std::vector<u8> vs_blob;
    GuestAddr so_bytecode_at{VS_BYTECODE};
    bool large_mapped{false};
};

}  // namespace vitreo::winapi::d3d11_test
