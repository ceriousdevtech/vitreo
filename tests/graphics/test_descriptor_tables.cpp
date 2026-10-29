// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_descriptor_tables.cpp
/// @brief Argument-buffer descriptor tables: the layout contract and the GPU.
///
/// D3D register spaces are far wider than Metal's flat per-stage argument
/// namespace. A shader may read t0..t127 and b0..b13; Metal allows 31 buffer
/// arguments per stage, and the whole b#/t#/u#/input-assembler layout has to
/// share them. Argument buffers collapse a whole register space into one buffer
/// argument, which is the only way the full contract fits — and the same
/// primitive is what a D3D12 descriptor heap is.
///
/// The thing that has to be right is the LAYOUT. The host writes descriptors at
/// byte offsets and the shader reads them by `[[id(n)]]`; if the two disagree the
/// shader samples the wrong resource and the draw still succeeds. So these tests
/// assert the layout against a real GPU rather than against a comment.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/glsl_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace vitreo::graphics;

namespace {

/// Compute shader declaring a wide t# space, mirroring what our DXBC translator
/// emits: one declaration per D3D register, set 1 for t#, set 2 for s#, set 3
/// for u#. `SLOTS` of them, so the top slot is far past Metal's flat limit.
[[nodiscard]] std::string wide_srv_compute_source(unsigned slots,
                                                 unsigned read_slot) {
    std::string src = "#version 450\n";
    src += "layout(local_size_x = 1) in;\n";
    for (unsigned i = 0; i < slots; ++i) {
        src += "layout(set = 1, binding = " + std::to_string(i) +
               ") uniform texture2D t" + std::to_string(i) + ";\n";
    }
    src += "layout(set = 2, binding = 0) uniform sampler s0;\n";
    src += "layout(set = 3, binding = 0, rgba8) uniform writeonly image2D u0;\n";
    src += "void main() {\n";
    src += "  vec4 v = texture(sampler2D(t" + std::to_string(read_slot) +
           ", s0), vec2(0.5));\n";
    src += "  imageStore(u0, ivec2(gl_GlobalInvocationID.xy), v);\n";
    src += "}\n";
    return src;
}

[[nodiscard]] bool compile(const std::string& glsl, unsigned slots,
                           std::string& msl_out) {
    glsl_compiler_init();
    GlslCompileOptions gopts{};
    gopts.vulkan_semantics = true;  // keep set= / binding= decorations
    gopts.auto_map_bindings = false;
    std::vector<vitreo::u32> spirv;
    if (!compile_glsl_to_spirv(glsl.c_str(), ShaderStage::Compute, gopts,
                               spirv)) {
        return false;
    }
    MslCompileOptions mopts{};
    mopts.argument_buffers = true;
    // Derive the buffer index from the same constant the backend binds at.
    // Hardcoding 1/2/3 here instead cost a debugging round: the shader read
    // [[buffer(1)]] while the backend bound set 1 at index 3, and the dispatch
    // succeeded and sampled nothing. That is precisely the silent failure the
    // shared constant exists to prevent, so the test must not restate it.
    const auto index = [](unsigned set) {
        return BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    mopts.argument_buffer_sets[1] = {slots, index(1),
                                     MslDescriptorClass::Texture};
    mopts.argument_buffer_sets[2] = {1, index(2), MslDescriptorClass::Sampler};
    mopts.argument_buffer_sets[3] = {1, index(3), MslDescriptorClass::Texture};
    const auto compiled = compile_spirv_to_msl(
        spirv.data(), spirv.size(), ShaderStage::Compute, mopts);
    if (!compiled.valid()) return false;
    msl_out = compiled.msl_text();
    return true;
}

} // namespace

// The layout claim this whole mechanism rests on: slot N lives at byte N * 8,
// and a slot the shader never names still occupies its 8 bytes. Read it off the
// generated MSL: every declared slot must carry its own id, equal to its D3D
// register number.
TEST(DescriptorTableLayout, EverySlotKeepsItsRegisterNumberAsItsArgumentId) {
    constexpr unsigned SLOTS = 32;
    std::string msl;
    ASSERT_TRUE(compile(wide_srv_compute_source(SLOTS, 0), SLOTS, msl));

    if (std::getenv("VITREO_DUMP_MSL") != nullptr) std::fputs(msl.c_str(), stderr);
    // The set-1 argument buffer must arrive as one buffer argument, not 32, at
    // the index the backend binds it.
    const std::string expected_index =
        "[[buffer(" +
        std::to_string(BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1) + ")]]";
    EXPECT_NE(msl.find(expected_index), std::string::npos) << msl;

    // Slot N at id(N), including the slots the shader never reads -- that is
    // what pad_argument_buffer_resources buys, and what lets the host encode
    // absolute D3D register numbers.
    for (unsigned slot = 0; slot < SLOTS; ++slot) {
        const std::string id = "[[id(" + std::to_string(slot) + ")]]";
        EXPECT_NE(msl.find(id), std::string::npos)
            << "slot " << slot << " has no argument id\n" << msl;
    }

    // And the flat texture namespace must be empty for this set: if SPIRV-Cross
    // fell back to discrete bindings we would see [[texture(n)]] instead, and
    // slots past Metal's limit would silently bind nothing.
    EXPECT_EQ(msl.find("[[texture("), std::string::npos)
        << "set 1 fell back to discrete texture bindings\n" << msl;
}


namespace {

/// A 1x1 R8 texture holding `value`, readable by a shader.
[[nodiscard]] std::unique_ptr<BackendTexture2D> value_texture(
    GraphicsBackendDevice& device, std::uint8_t value) {
    BackendTexture2DDesc td{};
    td.width = td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::R8Unorm;
    td.shader_resource = true;
    auto texture = device.create_texture_2d(td, &value, 1);
    return texture;
}

[[nodiscard]] BackendTextureViewDesc whole_texture_view() {
    BackendTextureViewDesc view{};
    view.dimension = BackendTextureViewDimension::Texture2D;
    view.mip_levels = 1;
    view.array_size = 1;
    return view;
}

} // namespace

// The test that justifies the mechanism. Slot 20 is past every flat limit this
// backend has (8 texture slots, 4 constant buffers), so a shader reading it
// through the flat path binds nothing and samples black.
//
// A decoy sits at slot 3. If the host and the shader disagreed about where slot
// 20 lives -- an off-by-one in the id mapping, packing that skipped the
// undeclared slots, a wrong byte stride -- the shader would read the decoy or an
// empty slot, and the dispatch would still succeed. Reading the right value is
// the only evidence that the layout is right.
TEST(DescriptorTable, ShaderReadsSlotsBeyondTheFlatTextureLimit) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }

    constexpr unsigned SLOTS = 32;
    constexpr unsigned READ_SLOT = 20;
    constexpr unsigned DECOY_SLOT = 3;
    static_assert(READ_SLOT >= BACKEND_MAX_TEXTURE_SLOTS,
                  "the point is to read past the flat limit");

    std::string msl;
    ASSERT_TRUE(compile(wide_srv_compute_source(SLOTS, READ_SLOT), SLOTS, msl))
        << "shader compilation";

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid()) << msl;

    // 0xCC at the slot the shader reads, 0x11 at the decoy.
    auto wanted = value_texture(device, 0xCC);
    auto decoy = value_texture(device, 0x11);
    ASSERT_NE(wanted, nullptr);
    ASSERT_NE(decoy, nullptr);

    BackendTexture2DDesc od{};
    od.width = od.height = 1;
    od.mip_levels = od.array_size = 1;
    od.pixel_format = PixelFormat::RGBA8Unorm;
    od.unordered_access = true;
    od.render_target = true;
    auto output = device.create_texture_2d(od, nullptr, 0);
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    auto srvs = device.create_descriptor_table(
        {SLOTS, BackendDescriptorClass::Texture});
    auto samplers = device.create_descriptor_table(
        {1, BackendDescriptorClass::Sampler});
    auto uavs = device.create_descriptor_table(
        {1, BackendDescriptorClass::Texture});
    ASSERT_NE(srvs, nullptr);
    ASSERT_NE(samplers, nullptr);
    ASSERT_NE(uavs, nullptr);
    EXPECT_EQ(srvs->slot_count(), SLOTS);

    const auto view = whole_texture_view();
    ASSERT_TRUE(srvs->set_texture(READ_SLOT, wanted.get(), view));
    ASSERT_TRUE(srvs->set_texture(DECOY_SLOT, decoy.get(), view));
    ASSERT_TRUE(uavs->set_texture(0, output.get(), view));

    BackendSamplerDesc sampler{};
    sampler.min_filter = BackendFilter::Nearest;
    sampler.mag_filter = BackendFilter::Nearest;
    sampler.mipmaps_enabled = false;
    sampler.address_u = BackendAddressMode::Clamp;
    sampler.address_v = BackendAddressMode::Clamp;
    ASSERT_TRUE(samplers->set_sampler(0, sampler));

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    // Tables only: the no-mixing rule means nothing goes in the flat arrays.
    call.descriptor_tables[1] = srvs.get();
    call.descriptor_tables[2] = samplers.get();
    call.descriptor_tables[3] = uavs.get();
    ASSERT_TRUE(device.dispatch(call));

    std::array<std::uint8_t, 4> pixel{};
    // Row pitch is in BYTES: one RGBA8 pixel is four.
    ASSERT_TRUE(output->readback(0, pixel.data(), 4));
    EXPECT_NEAR(pixel[0], 0xCC, 1)
        << "read slot " << READ_SLOT << " but got "
        << static_cast<unsigned>(pixel[0]) << " (decoy at slot " << DECOY_SLOT
        << " holds 0x11)";
}

// A typed buffer view (D3D Buffer<T>) in a table slot is read through its own
// element window and format, exactly as a flat binding reads it. The slot used
// to be left empty -- every table-mode shader read zeros from Buffer<T> -- so a
// decoy view at slot 1 and a FirstElement window at slot 9 pin both the slot
// and the window.
TEST(DescriptorTable, TypedBufferSlotsReadTheirOwnViewWindow) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    constexpr unsigned SLOTS = 16;
    constexpr unsigned READ_SLOT = 9;
    constexpr unsigned DECOY_SLOT = 1;
    constexpr unsigned FIRST = 4;
    constexpr unsigned COUNT = 8;
    constexpr unsigned WORDS = 64;

    std::string glsl = "#version 450\nlayout(local_size_x = 1) in;\n";
    for (unsigned i = 0; i < SLOTS; ++i) {
        glsl += "layout(set = 1, binding = " + std::to_string(i) +
                ") uniform utextureBuffer t" + std::to_string(i) + ";\n";
    }
    glsl += "layout(set = 3, binding = 0, r32ui) uniform writeonly uimage2D u0;\n";
    glsl += "void main() {\n  int i = int(gl_GlobalInvocationID.x);\n";
    glsl += "  imageStore(u0, ivec2(i, 0), uvec4(texelFetch(t" +
            std::to_string(READ_SLOT) + ", i).x));\n}\n";
    glsl_compiler_init();
    GlslCompileOptions gopts{};
    gopts.vulkan_semantics = true;
    gopts.auto_map_bindings = false;
    std::vector<vitreo::u32> spirv;
    ASSERT_TRUE(compile_glsl_to_spirv(glsl.c_str(), ShaderStage::Compute,
                                      gopts, spirv));
    MslCompileOptions mopts{};
    mopts.argument_buffers = true;
    mopts.argument_buffer_sets[1] = {SLOTS,
                                     BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1,
                                     MslDescriptorClass::Texture};
    mopts.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3,
                                     MslDescriptorClass::Texture};
    const auto compiled = compile_spirv_to_msl(
        spirv.data(), spirv.size(), ShaderStage::Compute, mopts);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    ASSERT_NE(msl.find("texture_buffer"), std::string::npos) << msl;

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid()) << msl;

    std::vector<vitreo::u32> words(WORDS), decoy_words(WORDS);
    for (unsigned i = 0; i < WORDS; ++i) {
        words[i] = 1000u + i;
        decoy_words[i] = 9000u + i;
    }
    BackendBufferDesc bd{};
    bd.byte_width = WORDS * 4;
    bd.shader_resource = true;
    auto storage = device.create_buffer(bd, words.data());
    auto decoy_storage = device.create_buffer(bd, decoy_words.data());
    ASSERT_TRUE(storage && decoy_storage);
    auto view = device.create_texture_buffer(*storage, PixelFormat::R32Uint,
                                             FIRST, COUNT, false);
    auto decoy = device.create_texture_buffer(*decoy_storage,
                                              PixelFormat::R32Uint, 0, COUNT,
                                              false);
    ASSERT_TRUE(view && view->valid() && decoy && decoy->valid());

    BackendTexture2DDesc od{};
    od.width = COUNT;
    od.height = 1;
    od.mip_levels = od.array_size = 1;
    od.pixel_format = PixelFormat::R32Uint;
    od.unordered_access = true;
    auto output = device.create_texture_2d(od, nullptr, 0);
    ASSERT_NE(output, nullptr);

    auto srvs = device.create_descriptor_table(
        {SLOTS, BackendDescriptorClass::Texture});
    auto uavs = device.create_descriptor_table(
        {1, BackendDescriptorClass::Texture});
    ASSERT_TRUE(srvs && uavs);
    ASSERT_TRUE(srvs->set_texture_buffer(READ_SLOT, view.get()));
    ASSERT_TRUE(srvs->set_texture_buffer(DECOY_SLOT, decoy.get()));
    ASSERT_TRUE(uavs->set_texture(0, output.get(), whole_texture_view()));

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = COUNT;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = call.threads_per_group_y =
        call.threads_per_group_z = 1;
    call.descriptor_tables[1] = srvs.get();
    call.descriptor_tables[3] = uavs.get();
    ASSERT_TRUE(device.dispatch(call));

    std::array<vitreo::u32, COUNT> texels{};
    ASSERT_TRUE(output->readback(0, texels.data(), COUNT * 4));
    for (unsigned i = 0; i < COUNT; ++i) {
        EXPECT_EQ(texels[i], 1000u + FIRST + i) << "texel " << i;
    }

    // Clearing the slot is still allowed and leaves nothing bound.
    ASSERT_TRUE(srvs->set_texture_buffer(READ_SLOT, nullptr));
}

// A table is persistent state, not a per-draw payload: a slot keeps its value
// until it is overwritten. That is what lets a frontend mirror D3D's binding
// model, where PSSetShaderResources touches only the range it names.
//
// And a dispatch takes a SNAPSHOT. Rebinding a slot afterwards must not reach
// back and change work already submitted -- which it would if the table handed
// the GPU the buffer it keeps mutating.
TEST(DescriptorTable, RebindingAfterDispatchDoesNotChangeTheSubmittedWork) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }

    constexpr unsigned SLOTS = 32;
    constexpr unsigned READ_SLOT = 20;
    std::string msl;
    ASSERT_TRUE(compile(wide_srv_compute_source(SLOTS, READ_SLOT), SLOTS, msl));

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);

    auto first = value_texture(device, 0x40);
    auto second = value_texture(device, 0xF0);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto make_output = [&] {
        BackendTexture2DDesc od{};
        od.width = od.height = 1;
        od.mip_levels = od.array_size = 1;
        od.pixel_format = PixelFormat::RGBA8Unorm;
        od.unordered_access = true;
        return device.create_texture_2d(od, nullptr, 0);
    };

    auto srvs = device.create_descriptor_table(
        {SLOTS, BackendDescriptorClass::Texture});
    auto samplers = device.create_descriptor_table(
        {1, BackendDescriptorClass::Sampler});
    auto uavs = device.create_descriptor_table(
        {1, BackendDescriptorClass::Texture});
    ASSERT_NE(srvs, nullptr);

    BackendSamplerDesc sampler{};
    sampler.mipmaps_enabled = false;
    ASSERT_TRUE(samplers->set_sampler(0, sampler));
    const auto view = whole_texture_view();

    const auto run = [&](BackendTexture2D* source, BackendTexture2D* out) {
        EXPECT_TRUE(srvs->set_texture(READ_SLOT, source, view));
        EXPECT_TRUE(uavs->set_texture(0, out, view));
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = 1;
        call.threads_per_group_y = 1;
        call.threads_per_group_z = 1;
        call.descriptor_tables[1] = srvs.get();
        call.descriptor_tables[2] = samplers.get();
        call.descriptor_tables[3] = uavs.get();
        EXPECT_TRUE(device.dispatch(call));
    };

    auto out_first = make_output();
    auto out_second = make_output();
    ASSERT_NE(out_first, nullptr);
    ASSERT_NE(out_second, nullptr);

    // Two dispatches, the slot rebound between them. Read both back only after
    // both are submitted, so a table that aliased one live buffer would show the
    // second value in the first result.
    run(first.get(), out_first.get());
    run(second.get(), out_second.get());

    std::array<std::uint8_t, 4> a{};
    std::array<std::uint8_t, 4> b{};
    ASSERT_TRUE(out_first->readback(0, a.data(), 4));
    ASSERT_TRUE(out_second->readback(0, b.data(), 4));
    EXPECT_NEAR(a[0], 0x40, 1) << "first dispatch saw the rebound texture";
    EXPECT_NEAR(b[0], 0xF0, 1) << "second dispatch missed the rebind";
}

// Slots the shader never reads, and slots nothing was bound to, are both legal:
// D3D lets a shader declare registers it does not use and lets a caller unbind
// ones it no longer needs. Neither may take the dispatch down.
TEST(DescriptorTable, EmptyAndClearedSlotsAreNotAnError) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }

    auto table = device.create_descriptor_table(
        {8, BackendDescriptorClass::Texture});
    ASSERT_NE(table, nullptr);

    auto texture = value_texture(device, 0x7F);
    ASSERT_NE(texture, nullptr);
    const auto view = whole_texture_view();

    EXPECT_TRUE(table->set_texture(4, texture.get(), view));
    table->clear_slot(4);
    // A null resource is the documented way to unbind, and must not be refused.
    EXPECT_TRUE(table->set_texture(4, nullptr, view));
    // Out of range is a caller error, and is refused rather than silently
    // writing past the table.
    EXPECT_FALSE(table->set_texture(8, texture.get(), view));
    EXPECT_FALSE(table->set_buffer(99, nullptr, 0));
}

// A fragment shader whose t# space is one past the flat texture capacity is
// the shape that takes the descriptor-table path. Constant buffers, textures,
// and samplers each occupy their own set, which is how the DXBC translator
// assigns D3D register spaces.
TEST(DescriptorTableLayout, FragmentShaderPastFlatTextureCapacityCompiles) {
    std::string src = "#version 450\nlayout(location=0) out vec4 o;\n";
    src += "layout(set=0, binding=0) uniform B0 { vec4 a; };\n";
    src += "layout(set=0, binding=1) uniform B1 { vec4 b; };\n";
    for (unsigned i = 0; i < 9; ++i) {
        src += "layout(set=1, binding=" + std::to_string(i) +
               ") uniform texture2D t" + std::to_string(i) + ";\n";
    }
    for (unsigned i = 0; i < 3; ++i) {
        src += "layout(set=2, binding=" + std::to_string(i) +
               ") uniform sampler s" + std::to_string(i) + ";\n";
    }
    src += "void main() {\n";
    src += "  o = texture(sampler2D(t8, s2), vec2(0.5)) + a + b;\n}\n";

    glsl_compiler_init();
    GlslCompileOptions gopts{};
    gopts.vulkan_semantics = true;
    gopts.auto_map_bindings = false;
    std::vector<vitreo::u32> spirv;
    ASSERT_TRUE(compile_glsl_to_spirv(src.c_str(), ShaderStage::Fragment, gopts,
                                      spirv));
    MslCompileOptions mopts{};
    mopts.argument_buffers = true;
    const auto index = [](unsigned set) {
        return BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    mopts.argument_buffer_sets[0] = {2, index(0), MslDescriptorClass::Buffer};
    mopts.argument_buffer_sets[1] = {9, index(1), MslDescriptorClass::Texture};
    mopts.argument_buffer_sets[2] = {3, index(2), MslDescriptorClass::Sampler};
    const auto compiled = compile_spirv_to_msl(
        spirv.data(), spirv.size(), ShaderStage::Fragment, mopts);
    ASSERT_TRUE(compiled.valid()) << "fragment descriptor table failed to compile";
}

// A caller that under-counts the table (buffer SRVs omitted from the high
// water) used to abort inside argument-buffer padding. The compiler grows the
// table to the highest binding the SPIR-V actually declares.
TEST(DescriptorTableLayout, DeclaredBindingPastSlotCountStillCompiles) {
    const char* src = R"(#version 450
layout(location=0) out vec4 o;
layout(set=1, binding=0) uniform texture2D t0;
layout(set=1, binding=6) uniform texture2D t6;
layout(set=2, binding=0) uniform sampler s0;
void main() { o = texture(sampler2D(t6, s0), vec2(0.5)); }
)";
    glsl_compiler_init();
    GlslCompileOptions gopts{};
    gopts.vulkan_semantics = true;
    gopts.auto_map_bindings = false;
    std::vector<vitreo::u32> spirv;
    ASSERT_TRUE(compile_glsl_to_spirv(src, ShaderStage::Fragment, gopts, spirv));
    MslCompileOptions mopts{};
    mopts.argument_buffers = true;
    const auto index = [](unsigned set) {
        return BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    mopts.argument_buffer_sets[1] = {2, index(1), MslDescriptorClass::Texture};
    mopts.argument_buffer_sets[2] = {1, index(2), MslDescriptorClass::Sampler};
    const auto compiled = compile_spirv_to_msl(
        spirv.data(), spirv.size(), ShaderStage::Fragment, mopts);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("[[id(6)]]"),
              std::string::npos);
}

// Apple Metal Feature Set Tables: Write is independent of read_write support.
// Start with nonzero bits, then require a compute store to produce zero in
// every component. Zero has an all-zero encoding in each listed format.
TEST(DescriptorTable, StoreOnlyFormatsWriteThroughFlatAndTableBindings) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const PixelFormat formats[] = {
        PixelFormat::A8Unorm, PixelFormat::R8Snorm,
        PixelFormat::R16Unorm, PixelFormat::R16Snorm,
        PixelFormat::RG8Unorm, PixelFormat::RG8Snorm,
        PixelFormat::RG8Uint, PixelFormat::RG8Sint,
        PixelFormat::RG16Unorm, PixelFormat::RG16Snorm,
        PixelFormat::RG16Float, PixelFormat::RG16Uint, PixelFormat::RG16Sint,
        PixelFormat::RG32Float, PixelFormat::RG32Uint, PixelFormat::RG32Sint,
        PixelFormat::RGBA8Snorm, PixelFormat::RGBA16Unorm,
        PixelFormat::RGBA16Snorm, PixelFormat::RGB10A2Unorm,
        PixelFormat::RGB10A2Uint, PixelFormat::RG11B10Float,
    };
    for (const auto format : formats) {
        SCOPED_TRACE(static_cast<unsigned>(format));
        const auto info = format_table(format);
        const bool uint_format = format == PixelFormat::RG8Uint ||
            format == PixelFormat::RG16Uint || format == PixelFormat::RG32Uint ||
            format == PixelFormat::RGB10A2Uint;
        const bool sint_format = format == PixelFormat::RG8Sint ||
            format == PixelFormat::RG16Sint || format == PixelFormat::RG32Sint;
        const std::string type = uint_format ? "uint" : sint_format ? "int" : "float";
        for (bool table_mode : {false, true}) {
            SCOPED_TRACE(table_mode);
            if (table_mode && !device.supports_descriptor_tables()) continue;
            std::string msl = "#include <metal_stdlib>\nusing namespace metal;\n";
            if (table_mode) {
                msl += "struct Outputs { texture2d<" + type +
                    ", access::write> output [[id(0)]]; };\n";
                msl += "kernel void main0(constant Outputs& outputs [[buffer(" +
                    std::to_string(BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3) +
                    ")]]) { outputs.output.write(" + type + "4(0), uint2(0)); }";
            } else {
                msl += "kernel void main0(texture2d<" + type +
                    ", access::write> output [[texture(8)]]) { output.write(" +
                    type + "4(0), uint2(0)); }";
            }
            BackendComputePipelineDesc pd{};
            pd.compute_entry_point = "main0";
            pd.compute_shader_msl = msl;
            auto pipeline = device.create_compute_pipeline(pd);
            ASSERT_NE(pipeline, nullptr);
            ASSERT_TRUE(pipeline->valid()) << msl;
            BackendTexture2DDesc td{};
            td.width = td.height = td.mip_levels = td.array_size = 1;
            td.pixel_format = format;
            td.unordered_access = true;
            std::array<std::uint8_t, 16> bytes{};
            bytes.fill(0x55);
            auto texture = device.create_texture_2d(td, bytes.data(), info.bytes_per_pixel);
            ASSERT_NE(texture, nullptr);
            BackendDispatchCall call{};
            call.pipeline = pipeline.get();
            call.group_count_x = call.group_count_y = call.group_count_z = 1;
            std::unique_ptr<BackendDescriptorTable> table;
            if (table_mode) {
                table = device.create_descriptor_table({1, BackendDescriptorClass::Texture});
                ASSERT_NE(table, nullptr);
                ASSERT_TRUE(table->set_texture(0, texture.get(), whole_texture_view()));
                call.descriptor_tables[3] = table.get();
            } else {
                call.textures[8] = texture.get();
                call.texture_views[8] = whole_texture_view();
            }
            ASSERT_TRUE(device.dispatch(call));
            ASSERT_TRUE(texture->readback(0, bytes.data(), info.bytes_per_pixel));
            for (unsigned i = 0; i < info.bytes_per_pixel; ++i) EXPECT_EQ(bytes[i], 0);
        }
    }
}
