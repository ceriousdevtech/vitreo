// SPDX-License-Identifier: LGPL-2.1-only
//
// Tiled resources tier 2 in the DXBC translator: the measured opcode numbers
// (tests/oracle_data/dxbc_tiled_opcode_probe.expected), the status forms'
// operand layout and the MinLOD clamp, on fxc's own bytecode
// (tiled_status_shaders.h).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_bindings.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_to_spirv_sm4.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "tiled_status_shaders.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace vitreo;
using namespace vitreo::graphics;
using dxbc_spirv_detail::Sm4Opcode;

namespace {

DxbcTranslationOptions tiled_options() {
    DxbcTranslationOptions options{};
    auto& t = options.tiled_status;
    t.enabled = true;
    t.srv_companion_base = 16;
    t.uav_companion_base = 32;
    t.status_sampler_base = 16;
    t.srv_residency = true;
    t.srv_residency_set = 0;
    t.srv_residency_binding = 15;
    t.uav_residency = true;
    t.uav_residency_set = 0;
    t.uav_residency_binding = 16;
    return options;
}

MslCompileOptions table_msl() {
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {17, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 0,
                                   MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {48, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1,
                                   MslDescriptorClass::Texture};
    msl.argument_buffer_sets[2] = {32, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 2,
                                   MslDescriptorClass::Sampler};
    msl.argument_buffer_sets[3] = {8, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3,
                                   MslDescriptorClass::Texture};
    return msl;
}

}  // namespace

// The enum is the measured table, name for name.
TEST(DxbcTiledStatus, OpcodesMatchTheFxcMeasurement) {
    const std::map<std::string, Sm4Opcode> names = {
        {"gather4_s", Sm4Opcode::GATHER4_S},
        {"gather4_c_s", Sm4Opcode::GATHER4_C_S},
        {"gather4_po_s", Sm4Opcode::GATHER4_PO_S},
        {"gather4_po_c_s", Sm4Opcode::GATHER4_PO_C_S},
        {"ld_s", Sm4Opcode::LD_S},
        {"ldms_s", Sm4Opcode::LD_MS_S},
        {"ld_uav_typed_s", Sm4Opcode::LD_UAV_TYPED_S},
        {"ld_raw_s", Sm4Opcode::LD_RAW_S},
        {"ld_structured_s", Sm4Opcode::LD_STRUCTURED_S},
        {"sample_l_s", Sm4Opcode::SAMPLE_L_S},
        {"sample_c_lz_s", Sm4Opcode::SAMPLE_C_LZ_S},
        {"sample_cl_s", Sm4Opcode::SAMPLE_CL_S},
        {"sample_b_cl_s", Sm4Opcode::SAMPLE_B_CL_S},
        {"sample_d_cl_s", Sm4Opcode::SAMPLE_D_CL_S},
        {"sample_c_cl_s", Sm4Opcode::SAMPLE_C_CL_S},
        {"check_access_fully_mapped", Sm4Opcode::CHECK_ACCESS_FULLY_MAPPED},
    };
    std::ifstream file(std::string(VITREO_SOURCE_DIR) +
                       "/tests/oracle_data/dxbc_tiled_opcode_probe.expected");
    ASSERT_TRUE(file.is_open());
    std::string line;
    usize matched = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string name;
        u32 value = 0;
        if (!(fields >> name >> value)) continue;
        const auto found = names.find(name);
        ASSERT_NE(found, names.end()) << name;
        EXPECT_EQ(static_cast<u32>(found->second), value) << name;
        ++matched;
    }
    EXPECT_EQ(matched, names.size());
}

TEST(DxbcTiledStatus, EveryStatusFormTranslatesWithTheResidencyBindings) {
    const auto container = parse_dxbc(tiled_opcode_ps, sizeof(tiled_opcode_ps));
    ASSERT_TRUE(container.valid());
    EXPECT_TRUE(dxbc_uses_tiled_status(container));
    const auto spirv = translate_dxbc_to_spirv(container, tiled_options());
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                          ShaderStage::Fragment, table_msl());
    ASSERT_TRUE(msl.valid());
    const std::string text = msl.msl_text();
    // sample(s, uv, off, clamp): the per-sample MinLOD clamp.
    EXPECT_NE(text.find("min_lod_clamp("), std::string::npos);
    EXPECT_NE(text.find("_residency"), std::string::npos);
    EXPECT_NE(text.find("_status_twin"), std::string::npos);
}

TEST(DxbcTiledStatus, AStatusWithoutResidencyBindingsFailsTranslation) {
    const auto container = parse_dxbc(tiled_opcode_ps, sizeof(tiled_opcode_ps));
    ASSERT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    EXPECT_FALSE(spirv.valid());
    EXPECT_NE(spirv.failure.find("tiled_status"), std::string::npos)
        << spirv.failure;
}

TEST(DxbcTiledStatus, TheA20ProbeShaderTranslates) {
    const auto container = parse_dxbc(tiled_status_cs, sizeof(tiled_status_cs));
    ASSERT_TRUE(container.valid());
    EXPECT_TRUE(dxbc_uses_tiled_status(container));
    const auto spirv = translate_dxbc_to_spirv(container, tiled_options());
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                          ShaderStage::Compute, table_msl());
    ASSERT_TRUE(msl.valid());
}
