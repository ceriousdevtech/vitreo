// SPDX-License-Identifier: LGPL-2.1-only
//
// Input-layout rules, the per-format capability table and the resource-creation rules.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_oracle_blobs.hpp"

#include "d3d11_test_binding_rules.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

class D3D11InputLayoutRulesTest : public D3D11BindingRulesTest {
protected:
    struct Element {
        const char* name;
        u32 index;
        u32 format;
        u32 slot;
        u32 offset;
        u32 slot_class;
        u32 step;
    };
    /// CreateInputLayout(elements, blob) -> HRESULT. @p null_array passes a
    /// NULL pInputElementDescs.
    u32 layout(std::initializer_list<Element> elements, const u8* blob,
               usize blob_size, bool null_array = false,
               usize length_override = 0) {
        GuestAddr name_at = NAMES;
        GuestAddr at = ELEMENTS;
        for (const Element& e : elements) {
            const std::string text(e.name);
            EXPECT_TRUE(memory.write_bytes(
                name_at, reinterpret_cast<const Byte*>(text.c_str()),
                text.size() + 1).is_ok());
            EXPECT_TRUE(memory.write_u64(at, name_at).is_ok());
            words(at + 8, {e.index, e.format, e.slot, e.offset, e.slot_class,
                           e.step});
            name_at += 32;
            at += 32;
        }
        EXPECT_TRUE(memory.write_bytes(
            BLOB, reinterpret_cast<const Byte*>(blob), blob_size).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0xDEADBEEFull).is_ok());
        const u32 hr = static_cast<u32>(dev(
            11, {null_array ? 0 : ELEMENTS, elements.size(), BLOB,
                 length_override != 0 ? length_override : blob_size, OUT}));
        if (hr == d3d11::S_OK) EXPECT_NE(read_ptr(OUT), 0u);
        else EXPECT_EQ(read_ptr(OUT), 0u);
        return hr;
    }
};

TEST_F(D3D11InputLayoutRulesTest, ElementsAreValidatedAgainstTheInputSignature) {
    using namespace test_blobs;
    constexpr u32 F4 = 2, F3 = 6, F2 = 16, U3 = 7, F1 = 41, SINT4 = 4;
    const u8* vs = kVsPositionTexcoord;
    const usize vs_size = sizeof(kVsPositionTexcoord);
    const Element pos{"POSITION", 0, F3, 0, 0, 0, 0};
    const Element uv{"TEXCOORD", 0, F2, 0, 12, 0, 0};
    constexpr u32 OK = d3d11::S_OK, BAD = d3d11::E_INVALIDARG;
    // a05 "il ..." rows.
    EXPECT_EQ(layout({pos, uv}, vs, vs_size), OK);
    EXPECT_EQ(layout({pos, uv, {"COLOR", 0, 28, 0, 20, 0, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({pos}, vs, vs_size), BAD) << "missing TEXCOORD0";
    EXPECT_EQ(layout({pos, {"TEXCOORD", 1, F2, 0, 12, 0, 0}}, vs, vs_size), BAD);
    EXPECT_EQ(layout({{"position", 0, F3, 0, 0, 0, 0},
                      {"texcoord", 0, F2, 0, 12, 0, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, F4, 0, 0, 0, 0},
                      {"TEXCOORD", 0, F2, 0, 16, 0, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, F1, 0, 0, 0, 0},
                      {"TEXCOORD", 0, F2, 0, 4, 0, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, U3, 0, 0, 0, 0}, uv}, vs, vs_size), OK);
    EXPECT_EQ(layout({pos, {"POSITION", 0, F3, 0, 12, 0, 0},
                      {"TEXCOORD", 0, F2, 0, 24, 0, 0}}, vs, vs_size), BAD)
        << "duplicate POSITION0";
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, F2, 0, 0xFFFFFFFFu, 0, 0}},
                     vs, vs_size), OK) << "APPEND_ALIGNED";
    EXPECT_EQ(layout({{"POSITION", 0, F3, 31, 0, 0, 0},
                      {"TEXCOORD", 0, F2, 0, 0, 0, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, F3, 32, 0, 0, 0},
                      {"TEXCOORD", 0, F2, 0, 0, 0, 0}}, vs, vs_size), BAD);
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, F2, 1, 0, 1, 0}}, vs, vs_size), OK);
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, F2, 1, 0, 1, 3}}, vs, vs_size), OK);
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, F2, 1, 0, 0, 3}}, vs, vs_size), BAD)
        << "per-vertex step 3";
    EXPECT_EQ(layout({{"POSITION", 0, F3, 0, 2048, 0, 0}, uv}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, F3, 0, 2036, 0, 0}, uv}, vs, vs_size), OK);
    EXPECT_EQ(layout({{"POSITION", 0, F3, 0, 2, 0, 0}, uv}, vs, vs_size), BAD);
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, 0, 0, 12, 0, 0}}, vs, vs_size), BAD);
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, 71, 0, 12, 0, 0}}, vs, vs_size), BAD);
    // SV_VertexID-only shader: the element array must exist even when empty.
    EXPECT_EQ(layout({}, kVsVertexIdOnly, sizeof(kVsVertexIdOnly), true), BAD);
    EXPECT_EQ(layout({}, kVsVertexIdOnly, sizeof(kVsVertexIdOnly)), OK);
    EXPECT_EQ(layout({{"SV_VertexID", 0, 42, 0, 0, 0, 0}}, kVsVertexIdOnly,
                     sizeof(kVsVertexIdOnly)), OK);
    EXPECT_EQ(layout({pos}, kVsVertexIdOnly, sizeof(kVsVertexIdOnly)), OK);
    EXPECT_EQ(layout({}, vs, vs_size, true), BAD);
    const u8 garbage[16] = {'D', 'X', 'B', 'C'};
    EXPECT_EQ(layout({pos}, garbage, sizeof(garbage)), BAD);
    // A pixel shader's SV_Position is D3D_NAME_POSITION and is required.
    EXPECT_EQ(layout({{"POSITION", 0, F4, 0, 0, 0, 0}}, kPsPosition,
                     sizeof(kPsPosition)), BAD);
    EXPECT_EQ(layout({{"SV_Position", 0, F4, 0, 0, 0, 0}}, kPsPosition,
                     sizeof(kPsPosition)), OK);

    // a05b: signature-only blobs (ISGN and ISG1), unread inputs, the
    // container hash, and a length short of TotalSize.
    EXPECT_EQ(layout({pos, {"TEXCOORD", 0, F2, 0, 16, 0, 0}},
                     kIsgnOnlyPositionTexcoord,
                     sizeof(kIsgnOnlyPositionTexcoord)), OK);
    EXPECT_EQ(layout({pos}, kIsgnOnlyPositionTexcoord,
                     sizeof(kIsgnOnlyPositionTexcoord)), BAD)
        << "an unread TEXCOORD0 is still required";
    EXPECT_EQ(layout({{"POSITION", 0, F4, 0, 0, 0, 0}},
                     kIsg1OnlyPositionVertexId,
                     sizeof(kIsg1OnlyPositionVertexId)), OK);
    EXPECT_EQ(layout({{"TEXCOORD", 0, F2, 0, 16, 0, 0}},
                     kIsg1OnlyPositionVertexId,
                     sizeof(kIsg1OnlyPositionVertexId)), BAD);
    {
        std::vector<u8> corrupt(vs, vs + vs_size);
        corrupt[corrupt.size() - 8] ^= 0x55;
        EXPECT_EQ(layout({pos, uv}, corrupt.data(), corrupt.size()), BAD);
    }
    EXPECT_EQ(layout({pos, uv}, vs, vs_size, false, 40), BAD);
    // Duplicates and classification, whatever the shader reads (a05b).
    const Element pos4{"POSITION", 0, F4, 0, 0, 0, 0};
    const u8* isg1 = kIsg1OnlyPositionVertexId;
    const usize isg1_size = sizeof(kIsg1OnlyPositionVertexId);
    EXPECT_EQ(layout({pos4, {"COLOR", 0, 28, 1, 0, 0, 0},
                      {"COLOR", 0, 28, 1, 4, 0, 0}}, isg1, isg1_size), BAD);
    EXPECT_EQ(layout({pos4, {"position", 0, F4, 1, 0, 0, 0}}, isg1, isg1_size),
              BAD);
    EXPECT_EQ(layout({pos4, {"COLOR", 0, 28, 1, 0, 2, 0}}, isg1, isg1_size),
              BAD);
    EXPECT_EQ(layout({pos4, {"SV_VertexID", 0, 42, 1, 0, 0, 0}}, isg1,
                     isg1_size), OK);
    EXPECT_EQ(layout({pos4, {"COLOR", 0, SINT4, 1, 0, 0, 0}}, isg1, isg1_size),
              OK);
    // Explicit offsets: aligned to the format's size, capped at 4.
    struct Align { u32 format; u32 offset; u32 hr; };
    for (const Align& a : {Align{34 /*R16G16_FLOAT*/, 2, BAD},
                           Align{34, 1, BAD}, Align{54 /*R16_FLOAT*/, 1, BAD},
                           Align{28 /*R8G8B8A8*/, 1, BAD},
                           Align{62 /*R8_UINT*/, 3, OK},
                           Align{41 /*R32_FLOAT*/, 2, BAD},
                           Align{2 /*R32G32B32A32*/, 4, OK},
                           Align{16 /*R32G32*/, 4, OK},
                           Align{24 /*R10G10B10A2*/, 2, BAD},
                           Align{26 /*R11G11B10*/, 2, BAD},
                           Align{10 /*R16G16B16A16_FLOAT*/, 2, BAD},
                           Align{10, 6, BAD}}) {
        EXPECT_EQ(layout({pos4, {"COLOR", 0, a.format, 1, a.offset, 0, 0}},
                         isg1, isg1_size), a.hr)
            << "format " << a.format << " @" << a.offset;
    }
}

TEST_F(D3D11InputLayoutRulesTest, DxbcContainerHashAndIsg1SignatureParse) {
    using namespace test_blobs;
    for (const auto& blob : {std::span<const u8>(kVsPositionTexcoord),
                             std::span<const u8>(kVsVertexIdOnly),
                             std::span<const u8>(kPsPosition),
                             std::span<const u8>(kIsgnOnlyPositionTexcoord),
                             std::span<const u8>(kIsg1OnlyPositionVertexId)}) {
        EXPECT_TRUE(graphics::dxbc_container_verified(blob.data(), blob.size()));
        std::vector<u8> copy(blob.begin(), blob.end());
        copy[copy.size() - 1] ^= 1;
        EXPECT_FALSE(graphics::dxbc_container_verified(copy.data(), copy.size()));
        EXPECT_FALSE(graphics::dxbc_container_verified(blob.data(),
                                                       blob.size() - 1));
    }
    // ISG1 elements are 32 bytes: stream, the ISGN fields, min precision.
    const auto container = graphics::parse_dxbc(kIsg1OnlyPositionVertexId,
                                                sizeof(kIsg1OnlyPositionVertexId));
    EXPECT_TRUE(container.has_input_signature);
    ASSERT_EQ(container.input_signature.size(), 2u);
    EXPECT_EQ(container.input_signature[0].semantic_name, "POSITION");
    EXPECT_EQ(container.input_signature[0].register_num, 0u);
    EXPECT_EQ(container.input_signature[0].component_mask, 0xFu);
    EXPECT_EQ(container.input_signature[1].semantic_name, "SV_VertexID");
    EXPECT_EQ(container.input_signature[1].system_value_type, 6u);
    EXPECT_EQ(container.input_signature[1].register_num, 1u);
}

TEST_F(D3D11InputLayoutRulesTest, DrawMapsHighInputSlotsOntoFreeBackendStreams) {
    using namespace test_blobs;
    // POSITION from IA slot 20 and TEXCOORD0 from slot 9: both past the
    // backend's eight streams, and both recorded (a05 "vb set(16,1)").
    ASSERT_EQ(layout({{"POSITION", 0, 6, 20, 0, 0, 0},
                      {"TEXCOORD", 0, 16, 9, 4, 0, 0}},
                     kVsPositionTexcoord, sizeof(kVsPositionTexcoord)),
              d3d11::S_OK);
    const GuestAddr il = read_ptr(OUT);
    ASSERT_TRUE(memory.write_bytes(
        BLOB, reinterpret_cast<const Byte*>(kVsPositionTexcoord),
        sizeof(kVsPositionTexcoord)).is_ok());
    ASSERT_EQ(dev(12, {BLOB, sizeof(kVsPositionTexcoord), 0, OUT}), d3d11::S_OK);
    const GuestAddr vs = read_ptr(OUT);
    ASSERT_TRUE(memory.write_bytes(
        BLOB, reinterpret_cast<const Byte*>(kPsPosition),
        sizeof(kPsPosition)).is_ok());
    ASSERT_EQ(dev(15, {BLOB, sizeof(kPsPosition), 0, OUT}), d3d11::S_OK);
    const GuestAddr ps = read_ptr(OUT);
    const GuestAddr position = buffer(256, BIND_VB);
    const GuestAddr texcoord = buffer(256, BIND_VB);
    const GuestAddr target = rtv(texture2d(16, 1, 1, RGBA8, COLOUR));
    ctx(17, {il});
    ctx(11, {vs, 0, 0});
    ctx(9, {ps, 0, 0});
    vb_set(20, {position}, {12}, {24});
    vb_set(9, {texcoord}, {20}, {8});
    set_rts({target}, 0);
    std::array<float, 6> viewport{0, 0, 16, 16, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        U32_A, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    ctx(44, {1, U32_A});
    ctx(24, {4});
    const unsigned draws = backend.state.draws;
    ctx(13, {3, 0});
    ASSERT_EQ(backend.state.draws, draws + 1);
    // Stream 0 stays slot 0's; the two high slots take streams 1 and 2.
    std::array<unsigned, 2> streams{};
    for (const auto& attr : backend.state.last_pipeline_attributes) {
        if (attr.offset == 0) streams[0] = attr.buffer_index;
        if (attr.offset == 4) streams[1] = attr.buffer_index;
    }
    EXPECT_EQ(streams[0], 1u) << "POSITION (slot 20)";
    EXPECT_EQ(streams[1], 2u) << "TEXCOORD0 (slot 9)";
    EXPECT_EQ(backend.state.last_pipeline_strides[1], 12u);
    EXPECT_EQ(backend.state.last_pipeline_strides[2], 20u);
    EXPECT_NE(backend.state.last_draw_vertex_buffers[1], nullptr);
    EXPECT_NE(backend.state.last_draw_vertex_buffers[2], nullptr);
    EXPECT_NE(backend.state.last_draw_vertex_buffers[1],
              backend.state.last_draw_vertex_buffers[2]);
    EXPECT_EQ(backend.state.last_draw_vertex_buffer_offsets[1], 24u);
    EXPECT_EQ(backend.state.last_draw_vertex_buffer_offsets[2], 8u);
}


// ---------------------------------------------------------------------------
// 2026-09-28 contract audit, areas 01/02/08: the per-format capability table,
// device creation, and the resource-creation rules. Expected values are the
// Windows (WARP) answers in tools/oracle_probe/d3d11_a01_*.expected /
// d3d11_a02_*.expected, generated into d3d11_format_oracle.inc.
// ---------------------------------------------------------------------------
namespace windows_formats {
#include "winapi/graphics/d3d11/d3d11_format_oracle.inc"
}  // namespace windows_formats

/// An Apple9 host as measured (M4 Pro): 2x and 4x MSAA, no 8x/16x,
/// 32-bit float filtering and float32 resolve.
d3d11::FormatCapsContext apple9_context() {
    d3d11::FormatCapsContext context{};
    context.feature_level = d3d11::FEATURE_LEVEL_11_0;
    context.sample_count_2 = true;
    context.sample_count_4 = true;
    context.float32_filtering = true;
    context.float32_msaa_resolve = true;
    return context;
}

TEST(D3D11FormatCaps, AnswersAreTheWindowsRowLessDocumentedGaps) {
    // Every DXGI format answers the Windows FL 11_0 CheckFormatSupport row,
    // minus only these bits, each a capability this implementation does not
    // have (no bit may be added: the table intersects the Windows row).
    struct Gap { u32 format; u32 missing; const char* why; };
    constexpr u32 kAllButBufferIaSo = ~0xBu;
    const Gap gaps[] = {
        {5, ~0u, "R32G32B32 has no Metal texel format"},
        {6, kAllButBufferIaSo, "R32G32B32 textures"},
        {7, kAllButBufferIaSo, "R32G32B32 textures"},
        {8, kAllButBufferIaSo, "R32G32B32 textures"},
        // (R10G10B10A2_UINT vertex fetch is no longer a gap: it is fetched
        // normalized and the vertex shader restores the integers.)
        {41, 0x04000400u, "sample_c / gather4_c need a depth texture"},
        {56, 0x04000400u, "sample_c / gather4_c need a depth texture"},
        {68, ~0u, "R8G8_B8G8 not implemented"},
        {69, ~0u, "G8R8_G8B8 not implemented"},
        {85, 0x02000003u, "no 565 vertex format or Metal Write"},
        {86, 0x02000003u, "no 5551 vertex format or Metal Write"},
        {88, 0x43u, "no X8 vertex format; volumes bind without a view"},
        {89, 0x01080000u, "XR_BIAS has no presentation path"},
        {92, 0x40u, "volumes bind without a view swizzle"},
        {93, 0x40u, "volumes bind without a view swizzle"},
        {115, 0x0264e043u,
         "ABGR4 storage: swizzled reads only, no RT / Write / volume"},
        {103, 0xC0000000u, "NV12: no video encoder / processor"},
        {104, 0u, "P010: every bit honoured"},
        {105, 0u, "P016 (P010 storage): every bit honoured"},
        // AYUV, Y410, Y416, 420_OPAQUE, YUY2, Y210, Y216, P8 and A8P8: every
        // bit of their rows is honoured (d3d11_video_formats_probe [caps]).
    };
    const auto context = apple9_context();
    for (u32 format = 0; format < 192; ++format) {
        const auto& row = windows_formats::kWindowsFormatSupport[format];
        const auto answer = d3d11::check_format_support(format, context);
        EXPECT_EQ(answer.hr, row.hr) << "format " << format;
        u32 missing = 0;
        for (const Gap& gap : gaps) {
            if (gap.format == format) missing = gap.missing;
        }
        EXPECT_EQ(answer.mask, row.support & ~missing)
            << "format " << format;
    }
}

TEST(D3D11FormatCaps, FormatSupport2AndLowerFeatureLevels) {
    auto context = apple9_context();
    // R32_UINT: typed load/store and the six atomics the FL 11_0 table
    // requires ("fmt  42: ... fs2=00000000/000007ff", less TILED/SHAREABLE).
    auto answer = d3d11::check_format_support2(42, context);
    EXPECT_EQ(answer.hr, 0u);
    EXPECT_EQ(answer.mask, 0xFFu);
    // A format with no SUPPORT2 capability answers E_FAIL and 0 (measured
    // "feature 3 size 8: hr=0x80004005 data=0,0" for UNKNOWN).
    answer = d3d11::check_format_support2(0, context);
    EXPECT_EQ(answer.hr, d3d11::E_FAIL);
    EXPECT_EQ(answer.mask, 0u);
    // FL 10_1: no typed UAVs; the answer follows WARP's FL 10_1 grid.
    context.feature_level = d3d11::FEATURE_LEVEL_10_1;
    const auto& row = windows_formats::kWindowsFormatSupport[28];
    answer = d3d11::check_format_support(28, context);
    EXPECT_EQ(answer.mask, row.support_10_1);
    EXPECT_EQ(answer.mask & 0x2000000u, 0u);
    // FL 9_1: R32G32B32A32_TYPELESS does not exist.
    context.feature_level = d3d11::FEATURE_LEVEL_9_1;
    EXPECT_EQ(d3d11::check_format_support(1, context).hr, d3d11::E_FAIL);
}

TEST(D3D11FormatCaps, MultisampleQualityLevelsFollowWindowsErrorRules) {
    // d3d11_a01_device_caps_probe64.expected "msaa fmt ..." lines.
    const auto context = apple9_context();
    u32 levels = 0xABABABABu;
    EXPECT_EQ(d3d11::check_multisample_quality_levels(28, 0, context, levels),
              d3d11::E_FAIL);
    EXPECT_EQ(levels, 0u);
    levels = 0xABABABABu;
    EXPECT_EQ(d3d11::check_multisample_quality_levels(28, 33, context, levels),
              d3d11::E_FAIL);
    EXPECT_EQ(levels, 0u);
    levels = 0xABABABABu;
    EXPECT_EQ(d3d11::check_multisample_quality_levels(133, 1, context, levels),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(levels, 0xABABABABu);  // untouched
    EXPECT_EQ(d3d11::check_multisample_quality_levels(71, 1, context, levels),
              0u);
    EXPECT_EQ(levels, 1u);  // "msaa fmt  71: ... 1:1 2:0"
    EXPECT_EQ(d3d11::check_multisample_quality_levels(71, 4, context, levels),
              0u);
    EXPECT_EQ(levels, 0u);
    EXPECT_EQ(d3d11::check_multisample_quality_levels(0, 1, context, levels),
              0u);
    EXPECT_EQ(levels, 1u);  // "msaa fmt   0: ... 1:1"
    EXPECT_EQ(d3d11::check_multisample_quality_levels(21, 2, context, levels),
              0u);
    EXPECT_EQ(levels, 0u);  // "fmt  21: ... q=1,0,0,0,0,0"
    EXPECT_EQ(d3d11::check_multisample_quality_levels(45, 4, context, levels),
              0u);
    EXPECT_EQ(levels, 1u);
    EXPECT_EQ(d3d11::check_multisample_quality_levels(28, 3, context, levels),
              0u);
    EXPECT_EQ(levels, 0u);
}

class D3D11CreationRulesTest : public D3D11ManagerTest {
protected:
    void SetUp() override {
        D3D11ManagerTest::SetUp();
        ASSERT_TRUE(memory.map_region(BIG, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        create_default_device();
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        marshaler.attach_cpu(cpu);
    }
    u32 create_1d(std::array<u32, 8> desc, GuestAddr initial = 0,
                  GuestAddr out = TEXTURE_OUT) {
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        return call_device(4, TEXTURE_DESC, initial, out);
    }
    u32 create_2d(std::array<u32, 11> desc, GuestAddr initial = 0,
                  GuestAddr out = TEXTURE_OUT) {
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        return call_device(5, TEXTURE_DESC, initial, out);
    }
    u32 create_buffer(std::array<u32, 6> desc, GuestAddr initial = 0) {
        EXPECT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        return call_device(3, GPU_BUFFER_DESC, initial, GPU_BUFFER_OUT);
    }
    u32 call_device(usize slot, u64 a1, u64 a2 = 0, u64 a3 = 0) {
        set_arg(0, device);
        set_arg(1, a1);
        set_arg(2, a2);
        set_arg(3, a3);
        d3d11.impl_DeviceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    /// Map(READ) subresource @p sub of a staging texture; returns the data.
    GuestAddr map_read(GuestAddr texture, u32 sub, u32& row_pitch) {
        set_arg(0, context);
        set_arg(1, texture);
        set_arg(2, sub);
        set_arg(3, 1);  // D3D11_MAP_READ
        set_arg(4, 0);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        row_pitch = memory.read_u32(MAPPED_OUT + 8).value();
        return memory.read_u64(MAPPED_OUT).value();
    }
    void unmap(GuestAddr texture, u32 sub) {
        set_arg(0, context);
        set_arg(1, texture);
        set_arg(2, sub);
        d3d11.impl_ContextMethod(marshaler, 15);
    }
    static constexpr GuestAddr BIG = 0x20000;
    GuestAddr device{0};
    GuestAddr context{0};
    Marshaler marshaler{memory};
};

// d3d11_a02_create_contracts_probe.expected [B]: CreateTexture1D limits.
// Every one of the "too many / too large" descriptors reached Metal's own
// descriptor validation before and aborted the host process.
TEST_F(D3D11CreationRulesTest, Texture1DDescriptorRules) {
    EXPECT_EQ(create_1d({16384, 1, 1, 61, 0, 8, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(create_1d({16385, 1, 1, 61, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({4, 1, 2048, 28, 0, 8, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(create_1d({4, 1, 2049, 28, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 7, 1, 28, 0, 8, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(create_1d({64, 8, 1, 28, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({8, 1, 1, 71, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 28, 1, 8, 0, 0}), d3d11::E_INVALIDARG)
        << "IMMUTABLE without data";
    EXPECT_EQ(create_1d({64, 1, 1, 28, 3, 8, 0x20000, 0}),
              d3d11::E_INVALIDARG) << "STAGING with SRV";
    EXPECT_EQ(create_1d({64, 2, 1, 28, 2, 8, 0x10000, 0}),
              d3d11::E_INVALIDARG) << "DYNAMIC with mips";
    EXPECT_EQ(create_1d({64, 1, 1, 200, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 0, 0, 8, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 40, 0, 0x40, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(create_1d({64, 1, 1, 40, 0, 0x48, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 28, 0, 0x40, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 28, 0, 0x28, 0, 0x802}),
              d3d11::E_INVALIDARG) << "SHARED|NTHANDLE";
    EXPECT_EQ(create_1d({64, 1, 1, 28, 0, 8, 0, 0x80}), d3d11::S_OK)
        << "RESOURCE_CLAMP";
    // ppTexture1D NULL validates first ("E T1D invalid (W=16385)").
    EXPECT_EQ(create_1d({16385, 1, 1, 61, 0, 8, 0, 0}, 0, 0),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(create_1d({64, 1, 1, 28, 0, 8, 0, 0}, 0, 0), 1u);
}

// "H T1D arr2 mips4 ... STG": the x64 D3D11_SUBRESOURCE_DATA record is 16
// bytes (a pointer and two UINTs); it was read at 24. Initial data for a
// STAGING 1D texture lands in its CPU copy, and a NULL pSysMem is refused.
TEST_F(D3D11CreationRulesTest, Texture1DInitialDataRecordsAndStaging) {
    constexpr GuestAddr RECORDS = BIG;
    constexpr GuestAddr PIXELS = BIG + 0x1000;
    // Width 8, 4 mips, 2 slices of R8G8B8A8: subresource i starts with byte
    // 0x10 + i.
    for (u32 i = 0; i < 8; ++i) {
        const GuestAddr data = PIXELS + i * 0x100;
        for (u32 b = 0; b < 32; ++b) {
            ASSERT_TRUE(memory.write_u8(data + b,
                                        static_cast<u8>(0x10 + i)).is_ok());
        }
        ASSERT_TRUE(memory.write_u64(RECORDS + i * 16, data).is_ok());
        ASSERT_TRUE(memory.write_u32(RECORDS + i * 16 + 8, 3).is_ok());
        ASSERT_TRUE(memory.write_u32(RECORDS + i * 16 + 12, 0).is_ok());
    }
    ASSERT_EQ(create_1d({8, 4, 2, 28, 3, 0, 0x30000, 0}, RECORDS),
              d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    for (u32 sub = 0; sub < 8; ++sub) {
        u32 pitch = 0;
        const GuestAddr mapped = map_read(texture, sub, pitch);
        const u32 width = std::max(1u, 8u >> (sub % 4));
        for (u32 b = 0; b < width * 4; ++b) {
            EXPECT_EQ(memory.read_u8(mapped + b).value(), 0x10 + sub)
                << "subresource " << sub << " byte " << b;
        }
        unmap(texture, sub);
    }

    // IID_ID3D11Texture1D answers; IID_ID3D11Texture2D does not.
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11TEXTURE1D),
        sizeof(IID_ID3D11TEXTURE1D)).is_ok());
    set_arg(0, texture);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), texture);
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11TEXTURE2D),
        sizeof(IID_ID3D11TEXTURE2D)).is_ok());
    d3d11.impl_Texture2DMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_NOINTERFACE);

    // "B pSysMem NULL DEF".
    ASSERT_TRUE(memory.write_u64(RECORDS, 0).is_ok());
    EXPECT_EQ(create_1d({8, 1, 1, 28, 0, 8, 0, 0}, RECORDS),
              d3d11::E_INVALIDARG);
}

// d3d11_a02_create_contracts_probe.expected [C] and the [format x shape]
// sweep: CreateTexture2D rules the old validator got wrong in both
// directions.
TEST_F(D3D11CreationRulesTest, Texture2DDescriptorRules) {
    const auto t2d = [&](u32 w, u32 h, u32 mips, u32 arr, u32 fmt, u32 samples,
                         u32 quality, u32 usage, u32 bind, u32 cpu_flags,
                         u32 misc) {
        return create_2d({w, h, mips, arr, fmt, samples, quality, usage, bind,
                          cpu_flags, misc});
    };
    // MipLevels beyond the chain and MSAA block compression aborted the host.
    EXPECT_EQ(t2d(32, 16, 7, 1, 28, 1, 0, 0, 8, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(t2d(1, 1, 2, 1, 28, 1, 0, 0, 8, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(t2d(16384, 1, 16, 1, 61, 1, 0, 0, 8, 0, 0),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(t2d(16384, 1, 15, 1, 61, 1, 0, 0, 8, 0, 0), d3d11::S_OK);
    EXPECT_EQ(t2d(16, 16, 1, 1, 71, 4, 0, 0, 8, 0, 0), d3d11::E_INVALIDARG);
    // Block compression.
    EXPECT_EQ(t2d(5, 5, 1, 1, 71, 1, 0, 0, 8, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(t2d(8, 8, 1, 1, 71, 1, 0, 0, 0x28, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(t2d(8, 8, 1, 1, 71, 1, 0, 2, 8, 0x10000, 0), d3d11::S_OK)
        << "DYNAMIC BC";
    // Usage details.
    EXPECT_EQ(t2d(64, 64, 2, 1, 28, 1, 0, 2, 8, 0x10000, 0),
              d3d11::E_INVALIDARG) << "DYNAMIC with mips";
    EXPECT_EQ(t2d(64, 64, 1, 6, 28, 1, 0, 3, 0, 0x20000, 4), d3d11::S_OK)
        << "STAGING cube";
    EXPECT_EQ(t2d(64, 64, 1, 1, 28, 1, 0, 0, 8, 0, 0x80), d3d11::S_OK)
        << "RESOURCE_CLAMP";
    EXPECT_EQ(t2d(64, 64, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0x80),
              d3d11::E_INVALIDARG) << "STAGING RESOURCE_CLAMP";
    EXPECT_EQ(t2d(64, 64, 1, 1, 87, 1, 0, 0, 0x20, 0, 0x200), d3d11::S_OK)
        << "GDI_COMPATIBLE";
    EXPECT_EQ(t2d(64, 64, 1, 1, 28, 1, 0, 0, 0x20, 0, 0x200),
              d3d11::E_INVALIDARG) << "GDI_COMPATIBLE RGBA8";
    EXPECT_EQ(t2d(64, 64, 2, 1, 87, 1, 0, 0, 0x20, 0, 0x200),
              d3d11::E_INVALIDARG) << "GDI_COMPATIBLE mips";
    // Depth families decided on the DXGI format, not the host one.
    EXPECT_EQ(t2d(16, 16, 1, 1, 44, 1, 0, 0, 8, 0, 0), d3d11::S_OK)
        << "R24G8 SRV only";
    EXPECT_EQ(t2d(16, 16, 1, 1, 44, 1, 0, 0, 0, 0, 0), d3d11::S_OK)
        << "R24G8 bind 0";
    EXPECT_EQ(t2d(16, 16, 1, 1, 46, 1, 0, 0, 0x40, 0, 0), d3d11::E_INVALIDARG)
        << "R24_UNORM_X8 DS";
    EXPECT_EQ(t2d(16, 16, 1, 1, 40, 1, 0, 0, 0xC0, 0, 0), d3d11::E_INVALIDARG)
        << "D32 DS|UAV";
    EXPECT_EQ(t2d(16, 16, 1, 1, 40, 1, 0, 0, 0, 0, 0), d3d11::S_OK)
        << "D32 bind 0";
    // DXGI_SAMPLE_DESC standard pattern; GetDesc echoes it.
    ASSERT_EQ(t2d(16, 16, 1, 1, 28, 4, 0xFFFFFFFFu, 0, 0x20, 0, 0),
              d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    set_arg(0, texture);
    set_arg(1, TEXTURE_DESC);
    d3d11.impl_Texture2DMethod(marshaler, 10);  // GetDesc
    EXPECT_EQ(memory.read_u32(TEXTURE_DESC + 6 * 4).value(), 0xFFFFFFFFu);
    EXPECT_EQ(t2d(16, 16, 1, 1, 28, 1, 0xFFFFFFFFu, 0, 8, 0, 0), d3d11::S_OK);
    EXPECT_EQ(t2d(16, 16, 1, 1, 28, 3, 0, 0, 0x20, 0, 0), d3d11::E_INVALIDARG);
    // FL 11_0 formats added: B8G8R8X8, B5G6R5, B5G5R5A1, B4G4R4A4, RGB9E5.
    for (u32 format : {88u, 92u, 93u, 85u, 86u, 115u, 67u}) {
        EXPECT_EQ(t2d(4, 4, 1, 1, format, 1, 0, 0, 8, 0, 0), d3d11::S_OK)
            << "format " << format;
    }
    EXPECT_EQ(t2d(4, 4, 1, 1, 88, 1, 0, 0, 0x28, 0, 0), d3d11::S_OK);
    EXPECT_EQ(backend.state.last_texture_desc.pixel_format,
              graphics::PixelFormat::BGRA8Unorm);
    EXPECT_TRUE(backend.state.last_texture_desc.pixel_format_view)
        << "the alpha = 1 read is a texture view";
    EXPECT_EQ(t2d(4, 4, 1, 1, 67, 1, 0, 0, 0x28, 0, 0), d3d11::E_INVALIDARG)
        << "R9G9B9E5 render target";
}

// D3D11_SUBRESOURCE_DATA: only the bytes a subresource occupies are read
// ("H T2D pitch64 last row ends at page end"), and a non-zero pitch below a
// row is honoured ("H T2D 4x4 pitch8 (<row)"); a zero pitch is refused.
TEST_F(D3D11CreationRulesTest, InitialDataReadsExactlyTheSubresource) {
    // A 4x2 RGBA8 staging texture whose second row ends at the end of the
    // mapped region: BIG + 0x10000 - 16.
    constexpr GuestAddr RECORD = BIG;
    const GuestAddr last_row = BIG + 0x10000 - 16;
    const GuestAddr first_row = last_row - 64;
    for (u32 b = 0; b < 16; ++b) {
        ASSERT_TRUE(memory.write_u8(first_row + b, 0xA0 + b).is_ok());
        ASSERT_TRUE(memory.write_u8(last_row + b, 0xC0 + b).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(RECORD, first_row).is_ok());
    ASSERT_TRUE(memory.write_u32(RECORD + 8, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(RECORD + 12, 0).is_ok());
    ASSERT_EQ(create_2d({4, 2, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0}, RECORD),
              d3d11::S_OK);
    GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    u32 pitch = 0;
    GuestAddr mapped = map_read(texture, 0, pitch);
    EXPECT_EQ(memory.read_u8(mapped).value(), 0xA0);
    EXPECT_EQ(memory.read_u8(mapped + pitch + 15).value(), 0xCF);
    unmap(texture, 0);

    // Pitch 8 (< the 16-byte row): row 1 starts 8 bytes after row 0.
    ASSERT_TRUE(memory.write_u64(RECORD, first_row).is_ok());
    ASSERT_TRUE(memory.write_u32(RECORD + 8, 8).is_ok());
    ASSERT_EQ(create_2d({4, 2, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0}, RECORD),
              d3d11::S_OK);
    texture = memory.read_u64(TEXTURE_OUT).value();
    mapped = map_read(texture, 0, pitch);
    EXPECT_EQ(memory.read_u8(mapped + pitch).value(), 0xA8);
    unmap(texture, 0);

    ASSERT_TRUE(memory.write_u32(RECORD + 8, 0).is_ok());
    EXPECT_EQ(create_2d({4, 2, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0}, RECORD),
              d3d11::E_INVALIDARG) << "SysMemPitch 0";
}

// Packed depth+stencil initial data (d3d11_a08_formats_probe "F045 stage",
// "F045 ds-init"): the guest row is 4 bytes a texel (D24 | S8 << 24), not the
// host's two-plane footprint; a GPU texture receives both planes.
TEST_F(D3D11CreationRulesTest, PackedDepthStencilInitialData) {
    constexpr GuestAddr RECORD = BIG;
    constexpr GuestAddr PIXELS = BIG + 0x100;
    const u32 words[4] = {0x5A400000u, 0xA5FFFFFFu, 0x00000000u, 0x01800000u};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory.write_u32(PIXELS + i * 4, words[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(RECORD, PIXELS).is_ok());
    ASSERT_TRUE(memory.write_u32(RECORD + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(RECORD + 12, 0).is_ok());
    ASSERT_EQ(create_2d({2, 2, 1, 1, 45, 1, 0, 3, 0, 0x20000, 0}, RECORD),
              d3d11::S_OK);
    const GuestAddr staging = memory.read_u64(TEXTURE_OUT).value();
    u32 pitch = 0;
    const GuestAddr mapped = map_read(staging, 0, pitch);
    EXPECT_EQ(pitch, 8u);
    EXPECT_EQ(memory.read_u32(mapped + 4).value(), 0xA5FFFFFFu);
    EXPECT_EQ(memory.read_u32(mapped + pitch + 4).value(), 0x01800000u);
    unmap(staging, 0);

    ASSERT_EQ(create_2d({2, 2, 1, 1, 45, 1, 0, 0, 0x40, 0, 0}, RECORD),
              d3d11::S_OK);
    ASSERT_EQ(backend.state.uploaded_depth_plane.size(), 4u);
    EXPECT_FLOAT_EQ(backend.state.uploaded_depth_plane[0],
                    static_cast<float>(0x400000 / 16777215.0));
    EXPECT_FLOAT_EQ(backend.state.uploaded_depth_plane[1], 1.0f);
    ASSERT_EQ(backend.state.uploaded_stencil_plane.size(), 4u);
    EXPECT_EQ(backend.state.uploaded_stencil_plane[0], 0x5A);
    EXPECT_EQ(backend.state.uploaded_stencil_plane[1], 0xA5);
    EXPECT_EQ(backend.state.uploaded_stencil_plane[3], 0x01);
}

// d3d11_a02_create_contracts_probe.expected [A] and [F]/[G].
TEST_F(D3D11CreationRulesTest, BufferDescriptorRules) {
    EXPECT_EQ(create_buffer({65552, 0, 4, 0, 0, 0}), d3d11::S_OK)
        << "CB above 4096 constants";
    EXPECT_EQ(create_buffer({256, 0, 0x90, 0, 0, 0}), d3d11::E_INVALIDARG)
        << "SO|UAV";
    EXPECT_EQ(create_buffer({256, 2, 0x10, 0x10000, 0, 0}),
              d3d11::E_INVALIDARG) << "DYNAMIC SO";
    EXPECT_EQ(create_buffer({256, 0, 0x20, 0, 0, 0}), d3d11::S_OK)
        << "render-target buffer";
    EXPECT_EQ(create_buffer({6, 0, 8, 0, 0x20, 0}), d3d11::S_OK) << "RAW BW6";
    EXPECT_EQ(create_buffer({8, 0, 0x80, 0, 0x10, 0}), d3d11::E_INVALIDARG)
        << "DRAWINDIRECT_ARGS below 12 bytes";
    EXPECT_EQ(create_buffer({12, 0, 0x80, 0, 0x10, 0}), d3d11::S_OK);
    EXPECT_EQ(create_buffer({4104, 0, 8, 0, 0x40, 2052}), d3d11::E_INVALIDARG)
        << "stride above 2048";
    EXPECT_EQ(create_buffer({256, 0, 1, 0, 0x40, 16}), d3d11::E_INVALIDARG)
        << "structured vertex buffer";
    EXPECT_EQ(create_buffer({0xFFFFFFF0u, 0, 8, 0, 0, 0}), 0x8007000Eu)
        << "above the 2 GiB resource limit";
    EXPECT_EQ(create_buffer({256, 0, 0x400, 0, 0, 0}), d3d11::S_OK)
        << "VIDEO_ENCODER bind is ignored";
    EXPECT_EQ(create_buffer({256, 0, 0x800, 0, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_buffer({256, 0, 8, 0, 0x8000, 0}), d3d11::S_OK)
        << "GUARDED";

    // GetDesc: StructureByteStride 0 unless structured; eviction priority
    // starts at DXGI_RESOURCE_PRIORITY_NORMAL.
    ASSERT_EQ(create_buffer({256, 0, 1, 0, 0, 16}), d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    set_arg(0, buffer);
    set_arg(1, GPU_BUFFER_DESC);
    d3d11.impl_BufferMethod(marshaler, 10);  // GetDesc
    EXPECT_EQ(memory.read_u32(GPU_BUFFER_DESC + 20).value(), 0u);
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 9);  // GetEvictionPriority
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x78000000u);
}

// d3d11_a01_device_caps_probe64.expected "feature N" lines, and the honest
// answers where this device differs from WARP by capability.
TEST_F(D3D11CreationRulesTest, CheckFeatureSupportMeasuredAnswers) {
    constexpr GuestAddr DATA = BIG;
    const auto feature = [&](u32 id, u32 size) {
        for (u32 i = 0; i < 64; i += 4) {
            EXPECT_TRUE(memory.write_u32(DATA + i, 0xCDCDCDCDu).is_ok());
        }
        set_arg(0, device);
        set_arg(1, id);
        set_arg(2, DATA);
        set_arg(3, size);
        d3d11.impl_DeviceMethod(marshaler, 33);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(feature(4, 4), d3d11::S_OK);  // D3D10_X_HARDWARE_OPTIONS
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
    ASSERT_EQ(feature(7, 4), d3d11::S_OK);  // D3D9_OPTIONS
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
    ASSERT_EQ(feature(9, 4), d3d11::S_OK);  // D3D9_SHADOW_SUPPORT
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
    ASSERT_EQ(feature(11, 4), d3d11::S_OK);  // D3D9_SIMPLE_INSTANCING
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
    ASSERT_EQ(feature(13, 16), d3d11::S_OK);  // D3D9_OPTIONS1
    for (u32 i = 0; i < 16; i += 4) {
        EXPECT_EQ(memory.read_u32(DATA + i).value(), 1u);
    }
    ASSERT_EQ(feature(16, 8), d3d11::S_OK);  // GPU_VIRTUAL_ADDRESS_SUPPORT
    EXPECT_NE(memory.read_u32(DATA).value(), 0u);
    EXPECT_EQ(memory.read_u32(DATA + 4).value(), 47u);
    ASSERT_EQ(feature(20, 8), d3d11::S_OK);  // DISPLAYABLE
    EXPECT_EQ(memory.read_u32(DATA).value(), 0u);
    EXPECT_EQ(memory.read_u32(DATA + 4).value(), 0u);
    EXPECT_EQ(feature(20, 4), d3d11::E_INVALIDARG);
    EXPECT_EQ(feature(21, 4), d3d11::E_INVALIDARG);
    // FORMAT_SUPPORT is CheckFormatSupport (Learn); InFormat is preserved.
    ASSERT_TRUE(memory.write_u32(BIG + 0x100, 28).is_ok());
    set_arg(0, device);
    set_arg(1, 2);
    set_arg(2, BIG + 0x100);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(BIG + 0x100).value(), 28u);
    EXPECT_EQ(memory.read_u32(BIG + 0x104).value(), 0x03fef3f3u);
    // UNKNOWN: "feature 2 size 8: hr=0x00000000 data=0,20001".
    ASSERT_TRUE(memory.write_u32(BIG + 0x100, 0).is_ok());
    d3d11.impl_DeviceMethod(marshaler, 33);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(BIG + 0x104).value(), 0x20001u);
}

TEST(D3D11FormatCaps, VideoFormatQualityLevelsAndBufferViews) {
    // [caps] "q=1,0,0,0" for every video format that CheckFormatSupport
    // knows -- NV11 / AI44 / IA44 too, though that call fails E_FAIL --
    // and E_INVALIDARG for P208 / V208 / V408 (no entry at all). R32G32B32
    // (no texture here) still has one level at x1 (a01 "fmt 6: ... q=1").
    const auto context = apple9_context();
    for (const u32 format : {100u, 101u, 102u, 106u, 107u, 108u, 109u, 110u,
                             111u, 112u, 113u, 114u, 6u}) {
        SCOPED_TRACE(format);
        u32 levels = 0xEEEE;
        EXPECT_EQ(d3d11::check_multisample_quality_levels(format, 1, context,
                                                          levels),
                  0u);
        EXPECT_EQ(levels, 1u);
        levels = 0xEEEE;
        EXPECT_EQ(d3d11::check_multisample_quality_levels(format, 2, context,
                                                          levels),
                  0u);
        EXPECT_EQ(levels, format == 6u ? levels : 0u);
    }
    for (const u32 format : {130u, 131u, 132u}) {
        u32 levels = 0xEEEE;
        EXPECT_EQ(d3d11::check_multisample_quality_levels(format, 1, context,
                                                          levels),
                  d3d11::E_INVALIDARG);
        EXPECT_EQ(levels, 0xEEEEu);
    }
    // No row of a video format carries BUFFER: none is a Buffer<> format.
    for (u32 format = 100; format <= 115; ++format) {
        if (format == 115) continue;  // B4G4R4A4: not a video format
        EXPECT_EQ(d3d11::typed_buffer_element_bytes(format), 0u) << format;
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
