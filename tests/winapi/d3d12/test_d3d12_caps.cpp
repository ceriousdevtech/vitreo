// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_caps.cpp
/// @brief ID3D12Device::CheckFeatureSupport against Windows: every size,
/// input rule and written byte of tools/oracle_probe/d3d12_caps_probe
/// {64,32}.expected (Windows, WARP12) is replayed through the guest path.
/// The contract is the sizes, the input validation, the HRESULTs and which
/// members get written; the capability VALUES describe this device, so a
/// value that differs from WARP's is checked against the rule that produced
/// it rather than against WARP (see the area report's "environment" list).

#include "winapi/graphics/d3d12/d3d12_caps.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d12 {
namespace {

std::vector<std::string> read_lines(const char* name) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR "/") + name);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::vector<u8> from_hex(const std::string& hex) {
    std::vector<u8> bytes;
    for (usize i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    }
    return bytes;
}

/// A device like the one Vitreo builds on an Apple GPU: 1x/2x/4x MSAA,
/// unified cache-coherent memory, 32-wide SIMD groups.
CapsContext device_caps(bool is_64bit) {
    CapsContext caps;
    caps.is_64bit = is_64bit;
    caps.formats.sample_count_2 = true;
    caps.formats.sample_count_4 = true;
    caps.formats.float32_filtering = true;
    caps.tile_based_renderer = true;
    caps.unified_memory = true;
    caps.cache_coherent_uma = true;
    caps.max_buffer_length = 1ull << 34;
    caps.simd_width = 32;
    return caps;
}

class D3D12CapsTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBase = 0x30000000;
    static constexpr GuestAddr kData = kBase;
    static constexpr GuestAddr kLevels = kBase + 0x800;
    static constexpr GuestAddr kMetaOut = kBase + 0x900;
    static constexpr GuestAddr kTypes = kBase + 0xA00;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
    }

    /// d3d12_caps_probe.c prep(): 0xCD everywhere, then the default inputs.
    /// Returns the byte offsets that are inputs.
    std::set<u32> prep(u32 feature, bool is_64bit) {
        std::vector<u8> cd(0x1000, 0xCD);
        EXPECT_TRUE(memory.write_bytes(kBase, cd.data(), cd.size()).is_ok());
        std::set<u32> inputs;
        const auto put = [&](u32 offset, u32 value) {
            EXPECT_TRUE(memory.write_u32(kData + offset, value).is_ok());
            for (u32 i = 0; i < 4; ++i) inputs.insert(offset + i);
        };
        const auto put_ptr = [&](u32 offset, GuestAddr value) {
            if (is_64bit) {
                EXPECT_TRUE(memory.write_u64(kData + offset, value).is_ok());
                for (u32 i = 0; i < 8; ++i) inputs.insert(offset + i);
            } else {
                put(offset, static_cast<u32>(value));
            }
        };
        switch (feature) {
        case 1: case 16: case 24: case 10: case 33:
            put(0, 0);
            break;
        case 2:
            put(0, 4);
            put_ptr(is_64bit ? 8 : 4, kLevels);
            for (u32 i = 0; i < 4; ++i) {
                static const u32 levels[] = {0xB000, 0xB100, 0xC000, 0xC100};
                EXPECT_TRUE(memory.write_u32(kLevels + i * 4, levels[i]).is_ok());
            }
            break;
        case 3: case 5:
            put(0, 28);
            break;
        case 4:
            put(0, 28);
            put(4, 4);
            put(8, 0);
            break;
        case 7:
            put(0, 0x69);
            break;
        case 12:
            put(0, 3);
            break;
        case 20:
            put(0, 0);
            put(4, 0);
            break;
        case 31: {
            for (u32 i = 0; i < 20; ++i) {
                EXPECT_TRUE(memory.write_u32(kData + (i / 4) * 4, 0).is_ok());
                inputs.insert(i);
            }
            put_ptr(is_64bit ? 24 : 20, 0);
            put_ptr(is_64bit ? 32 : 24, 0);
            put_ptr(is_64bit ? 40 : 28, kMetaOut);
            put_ptr(is_64bit ? 48 : 32, 64);
            break;
        }
        case 34:
            put(0, 0);
            put(4, 0);
            put_ptr(8, 0);
            break;
        case 51:
            put(0, 28);
            put(4, 3);
            for (u32 o = 8; o < 28; o += 4) put(o, o == 8 ? 1u : 0u);
            break;
        default:
            break;
        }
        return inputs;
    }

    std::vector<u8> bytes(u32 count) {
        std::vector<u8> out(count);
        EXPECT_TRUE(memory.read_bytes(kData, out.data(), count).is_ok());
        return out;
    }

    GuestMemory memory;
};

TEST_F(D3D12CapsTest, AcceptedSizesMatchWindows) {
    // The undocumented feature values WARP answers (not in SDK 26100's
    // D3D12_FEATURE): refused here.
    const std::set<u32> undocumented = {54, 55, 58};
    for (const bool is_64bit : {true, false}) {
        const CapsContext caps = device_caps(is_64bit);
        unsigned checked = 0;
        for (const std::string& line : read_lines(is_64bit ? "d3d12_caps_probe64.expected"
                                                           : "d3d12_caps_probe32.expected")) {
            unsigned id = 0;
            char name[96] = {};
            if (std::sscanf(line.c_str(), "scan %u %95s", &id, name) != 2) continue;
            std::map<u32, u32> accepted;
            std::istringstream words(line);
            std::string word;
            while (words >> word) {
                unsigned size = 0, hr = 0;
                if (std::sscanf(word.c_str(), "%u:0x%x", &size, &hr) == 2) accepted[size] = hr;
            }
            if (undocumented.count(id) != 0) continue;
            const u32 limit = accepted.empty() ? 320u : accepted.rbegin()->first + 32u;
            for (u32 size = 0; size <= limit; ++size) {
                prep(id, is_64bit);
                const u32 hr = check_feature_support(memory, caps, id, kData, size);
                const auto it = accepted.find(size);
                const u32 want = it == accepted.end() ? E_INVALIDARG : it->second;
                EXPECT_EQ(hr, want) << (is_64bit ? "x64 " : "x86 ") << name << " size " << size;
                ++checked;
            }
        }
        EXPECT_GT(checked, 3000u);
    }
}

TEST_F(D3D12CapsTest, WritesTheMembersWindowsWrites) {
    for (const bool is_64bit : {true, false}) {
        const CapsContext caps = device_caps(is_64bit);
        unsigned checked = 0;
        for (const std::string& line : read_lines(is_64bit ? "d3d12_caps_probe64.expected"
                                                           : "d3d12_caps_probe32.expected")) {
            char name[96] = {};
            unsigned id = 0, hr = 0;
            char hex[1024] = {};
            if (std::sscanf(line.c_str(), "dump %95s %u hr=0x%x bytes=%1023s", name, &id, &hr, hex) !=
                4) {
                continue;
            }
            const std::vector<u8> windows = from_hex(hex);
            const std::set<u32> inputs = prep(id, is_64bit);
            const u32 got = check_feature_support(memory, caps, id, kData,
                                                  static_cast<u32>(windows.size()));
            EXPECT_EQ(got, hr) << name;
            const std::vector<u8> ours = bytes(static_cast<u32>(windows.size()));
            for (u32 i = 0; i < windows.size(); ++i) {
                if (inputs.count(i) != 0) continue;
                EXPECT_EQ(windows[i] != 0xCD, ours[i] != 0xCD)
                    << (is_64bit ? "x64 " : "x86 ") << name << " byte " << i;
            }
            ++checked;
        }
        EXPECT_EQ(checked, 47u);
    }
}

TEST_F(D3D12CapsTest, InputRulesMatchWindows) {
    for (const bool is_64bit : {true, false}) {
        const CapsContext caps = device_caps(is_64bit);
        // This device's level is 11_0 (no logic ops, no 16x UAV-only
        // rendering): the lists whose Windows answer is above it are the
        // device's, not the rule's.
        ASSERT_EQ(max_feature_level(caps), 0xB000u);
        unsigned matched = 0;
        for (const std::string& line : read_lines(is_64bit ? "d3d12_caps_probe64.expected"
                                                           : "d3d12_caps_probe32.expected")) {
            char name[96] = {};
            unsigned node = 0, id = 0, hr = 0;
            char hex[256] = {};
            if (std::sscanf(line.c_str(), "node %95s node=%u %u hr=0x%x bytes=%255s", name, &node, &id,
                            &hr, hex) == 5) {
                const std::vector<u8> windows = from_hex(hex);
                prep(id, is_64bit);
                ASSERT_TRUE(memory.write_u32(kData, node).is_ok());
                EXPECT_EQ(check_feature_support(memory, caps, id, kData,
                                                static_cast<u32>(windows.size())),
                          hr)
                    << line;
                if (hr != S_OK) {
                    const std::vector<u8> ours = bytes(static_cast<u32>(windows.size()));
                    EXPECT_EQ(std::memcmp(ours.data() + 4, windows.data() + 4, windows.size() - 4) == 0 ||
                                  id == 34,  // TYPES' pointer input differs by address
                              true)
                        << line;
                }
                ++matched;
                continue;
            }
            char tag[64] = {};
            unsigned max = 0, count = 0;
            if (std::sscanf(line.c_str(), "levels %63s hr=0x%x max=0x%x count=%u", tag, &hr, &max,
                            &count) == 4) {
                struct Case {
                    const char* tag;
                    std::vector<u32> levels;
                    bool null_ptr;
                };
                static const Case cases[] = {
                    {"one_11_0", {0xB000}, false},    {"one_12_2", {0xC200}, false},
                    {"one_9_1", {0x9100}, false},     {"one_1_0_core", {0x1000}, false},
                    {"one_1_0_generic", {0x100}, false},
                    {"all", {0x9100, 0xA000, 0xB000, 0xB100, 0xC000, 0xC200}, false},
                    {"desc", {0xC100, 0xB100, 0xA000}, false},
                    {"bogus", {0x1234, 0xB000}, false}, {"bogus_only", {0x1234}, false},
                    {"zero_level", {0, 0xB000}, false}, {"future", {0xD000, 0xB000}, false},
                    {"dup", {0xB000, 0xB000, 0x9300}, false}, {"count0", {}, false},
                    {"count0_null", {}, true},        {"null_ptr", {0xB000, 0}, true},
                };
                for (const Case& c : cases) {
                    if (std::string(tag) != c.tag) continue;
                    prep(2, is_64bit);
                    ASSERT_TRUE(memory.write_u32(kData, static_cast<u32>(c.levels.size())).is_ok());
                    for (usize i = 0; i < c.levels.size(); ++i) {
                        ASSERT_TRUE(memory.write_u32(kLevels + i * 4, c.levels[i]).is_ok());
                    }
                    if (is_64bit) {
                        ASSERT_TRUE(memory.write_u64(kData + 8, c.null_ptr ? 0 : kLevels).is_ok());
                    } else {
                        ASSERT_TRUE(memory.write_u32(kData + 4, c.null_ptr ? 0u : static_cast<u32>(kLevels))
                                        .is_ok());
                    }
                    const u32 got = check_feature_support(memory, caps, 2, kData, is_64bit ? 24 : 12);
                    const u32 got_max = memory.read_u32(kData + (is_64bit ? 16 : 8)).value();
                    // Windows' rule on this device's level: the highest
                    // requested level not above 11_0.
                    u32 want_max = 0;
                    for (u32 l : c.levels) {
                        if (l <= 0xB000u && l > want_max) want_max = l;
                    }
                    if (c.levels.empty() || c.null_ptr) {
                        EXPECT_EQ(got, E_INVALIDARG) << tag;
                        EXPECT_EQ(got, hr) << tag;
                        EXPECT_EQ(got_max, 0xCDCDCDCDu) << tag;
                    } else {
                        EXPECT_EQ(got, want_max == 0 ? DXGI_ERROR_UNSUPPORTED : S_OK) << tag;
                        EXPECT_EQ(got_max, want_max) << tag;
                        if (max <= 0xB000u) {  // WARP's answer is within this level
                            EXPECT_EQ(got, hr) << tag;
                            EXPECT_EQ(got_max, max) << tag;
                        }
                    }
                }
                ++matched;
                continue;
            }
            unsigned in = 0, out = 0;
            if (std::sscanf(line.c_str(), "shader_model in=0x%x hr=0x%x out=0x%x", &in, &hr, &out) == 3 ||
                std::sscanf(line.c_str(), "root_signature in=0x%x hr=0x%x out=0x%x", &in, &hr, &out) ==
                    3) {
                const bool sm = line.rfind("shader_model", 0) == 0;
                prep(sm ? 7 : 12, is_64bit);
                ASSERT_TRUE(memory.write_u32(kData, in).is_ok());
                EXPECT_EQ(check_feature_support(memory, caps, sm ? 7 : 12, kData, 4), hr) << line;
                const u32 ours = memory.read_u32(kData).value();
                const u32 device = sm ? 0x51u : 2u;  // SM 5.1, root signature 1.1
                EXPECT_EQ(ours, hr == S_OK ? std::min(in, device) : in) << line;
                if (hr != S_OK || in <= device) EXPECT_EQ(ours, out) << line;
                ++matched;
                continue;
            }
            unsigned type = 0, prio = 0;
            if (std::sscanf(line.c_str(), "queue_priority type=%u prio=%u hr=0x%x bytes=%255s", &type,
                            &prio, &hr, hex) == 4) {
                prep(20, is_64bit);
                ASSERT_TRUE(memory.write_u32(kData, type).is_ok());
                ASSERT_TRUE(memory.write_u32(kData + 4, prio).is_ok());
                EXPECT_EQ(check_feature_support(memory, caps, 20, kData, 12), hr) << line;
                const u32 supported = memory.read_u32(kData + 8).value();
                if (hr != S_OK) {
                    EXPECT_EQ(supported, 0xCDCDCDCDu) << line;
                } else {
                    EXPECT_EQ(supported, (type == 0 || type == 2 || type == 3) && prio == 0 ? 1u : 0u)
                        << line;
                }
                ++matched;
                continue;
            }
            unsigned ptr = 0, count_out = 0;
            if (std::sscanf(line.c_str(), "session_types count=%u ptr=%u hr=0x%x count_out=%u", &count,
                            &ptr, &hr, &count_out) == 4) {
                prep(34, is_64bit);
                ASSERT_TRUE(memory.write_u32(kData + 4, count).is_ok());
                if (ptr) ASSERT_TRUE(memory.write_u32(kData + 8, static_cast<u32>(kTypes)).is_ok());
                EXPECT_EQ(check_feature_support(memory, caps, 34, kData, is_64bit ? 16 : 12), hr) << line;
                EXPECT_EQ(memory.read_u32(kData + 4).value(), count_out) << line;
                EXPECT_EQ(memory.read_u32(kTypes).value(), 0xCDCDCDCDu) << line;
                ++matched;
                continue;
            }
            unsigned v = 0;
            if (std::sscanf(line.c_str(), "meta_command v=%u hr=0x%x", &v, &hr) == 2) {
                prep(31, is_64bit);
                EXPECT_EQ(check_feature_support(memory, caps, 31, kData, is_64bit ? 56 : 36), hr) << line;
                EXPECT_EQ(memory.read_u32(kMetaOut).value(), 0xCDCDCDCDu) << line;
                ++matched;
                continue;
            }
            unsigned fmt = 0, dim = 0, heap = 0, supported = 0;
            if (std::sscanf(line.c_str(), "placed fmt=%u dim=%u heap=%u hr=0x%x supported=0x%x", &fmt, &dim,
                            &heap, &hr, &supported) == 5) {
                prep(51, is_64bit);
                EXPECT_EQ(check_feature_support(memory, caps, 51, kData, 32), hr) << line;
                EXPECT_EQ(memory.read_u32(kData + 28).value(), supported) << line;
                ++matched;
            }
        }
        // 24 node, 15 levels, 18 + 7 version, 63 priority, 6 session, 4
        // meta-command and 336 placed-resource lines.
        EXPECT_EQ(matched, 473u);
    }
}

TEST_F(D3D12CapsTest, FormatQueriesFollowWindowsRules) {
    const CapsContext caps = device_caps(true);
    unsigned checked = 0;
    for (const std::string& line : read_lines("d3d12_caps_probe64.expected")) {
        unsigned f = 0, hr = 0, s1 = 0, s2 = 0, info_hr = 0, planes = 0;
        if (std::sscanf(line.c_str(), "format %u support hr=0x%x s1=0x%x s2=0x%x info hr=0x%x planes=%u", &f,
                        &hr, &s1, &s2, &info_hr, &planes) != 6) {
            continue;
        }
        // FORMAT_INFO is the runtime's format knowledge: exact.
        prep(5, true);
        ASSERT_TRUE(memory.write_u32(kData, f).is_ok());
        EXPECT_EQ(check_feature_support(memory, caps, 5, kData, 8), info_hr) << line;
        EXPECT_EQ(memory.read_u8(kData + 4).value(), info_hr == S_OK ? planes : 0xCDu) << line;

        // FORMAT_SUPPORT: this device's support is within Windows' (the
        // D3D11 WARP ceiling intersected with Metal), a format Windows
        // refuses is refused, and both masks are always written.
        prep(3, true);
        ASSERT_TRUE(memory.write_u32(kData, f).is_ok());
        const u32 ours_hr = check_feature_support(memory, caps, 3, kData, 12);
        const u32 ours1 = memory.read_u32(kData + 4).value();
        const u32 ours2 = memory.read_u32(kData + 8).value();
        if (hr != S_OK) {
            EXPECT_EQ(ours_hr, hr) << line;
            EXPECT_EQ(ours1, 0u) << line;
            EXPECT_EQ(ours2, 0u) << line;
        } else if (ours_hr == S_OK) {
            EXPECT_EQ(ours1 & ~s1, 0u) << line << " ours s1=" << std::hex << ours1;
            EXPECT_EQ(ours2 & ~s2, 0u) << line << " ours s2=" << std::hex << ours2;
        } else {
            EXPECT_EQ(ours_hr, E_FAIL) << line;
            EXPECT_EQ(ours1, 0u) << line;
        }

        // MULTISAMPLE_QUALITY_LEVELS: HRESULT exact; NumQualityLevels exact
        // where it does not depend on the device (unknown format, bad
        // count, one sample untiled), else this device's is 0 or Windows'.
        const auto msaa = line.find(" msaa ");
        std::istringstream rest(line.substr(msaa + 6));
        std::string item;
        while (rest >> item) {
            unsigned count = 0, flags = 0, qhr = 0, levels = 0;
            if (std::sscanf(item.c_str(), "%u/%u:%x:%x", &count, &flags, &qhr, &levels) != 4) continue;
            prep(4, true);
            ASSERT_TRUE(memory.write_u32(kData, f).is_ok());
            ASSERT_TRUE(memory.write_u32(kData + 4, count).is_ok());
            ASSERT_TRUE(memory.write_u32(kData + 8, flags).is_ok());
            EXPECT_EQ(check_feature_support(memory, caps, 4, kData, 16), qhr) << line << " " << item;
            const u32 ours = memory.read_u32(kData + 12).value();
            if (qhr != S_OK || (count == 1 && flags == 0)) {
                EXPECT_EQ(ours, levels) << line << " " << item;
            } else {
                EXPECT_TRUE(ours == 0 || ours == levels) << line << " " << item << " ours " << ours;
            }
        }
        ++checked;
    }
    EXPECT_EQ(checked, 193u);
}

TEST_F(D3D12CapsTest, FeatureLevelFollowsTheImplementedTiers) {
    CapsContext caps = device_caps(true);
    EXPECT_EQ(max_feature_level(caps), 0xB000u);
    caps.logic_ops = true;
    caps.sample_counts_with_no_outputs = 0x1F;
    EXPECT_EQ(max_feature_level(caps), 0xB100u);
    caps.resource_binding_tier = 2;
    caps.tiled_resources_tier = 2;
    EXPECT_EQ(max_feature_level(caps), 0xB100u);  // still no typed UAV load formats
    caps.typed_uav_load_additional_formats = true;
    EXPECT_EQ(max_feature_level(caps), 0xC000u);
}

TEST_F(D3D12CapsTest, ShaderModelIsFiveOneAndNoDxilFeatureIsClaimed) {
    const CapsContext caps = device_caps(true);
    prep(7, true);
    ASSERT_TRUE(memory.write_u32(kData, 0x66).is_ok());
    EXPECT_EQ(check_feature_support(memory, caps, 7, kData, 4), S_OK);
    EXPECT_EQ(memory.read_u32(kData).value(), 0x51u);
    // OPTIONS5 RaytracingTier, OPTIONS6 VRS tier, OPTIONS7 mesh / sampler
    // feedback, OPTIONS21 work graphs: all NOT_SUPPORTED.
    const struct {
        u32 feature;
        u32 size;
        u32 offset;
    } none[] = {{27, 12, 8}, {30, 20, 8}, {32, 8, 0}, {32, 8, 4}, {53, 16, 0}, {8, 24, 0}, {8, 24, 20}};
    for (const auto& n : none) {
        prep(n.feature, true);
        ASSERT_EQ(check_feature_support(memory, caps, n.feature, kData, n.size), S_OK);
        EXPECT_EQ(memory.read_u32(kData + n.offset).value(), 0u) << n.feature << "@" << n.offset;
    }
}

TEST_F(D3D12CapsTest, LayoutRulesUseTheDeviceFacts) {
    CapsContext caps = device_caps(true);
    LayoutCaps layout = layout_caps(caps);
    ResourceDesc bc1;
    bc1.dimension = RESOURCE_DIMENSION_TEXTURE2D;
    bc1.width = 37;
    bc1.height = 19;
    bc1.depth_or_array_size = 1;
    bc1.mip_levels = 1;
    bc1.format = 71;
    bc1.sample_count = 1;
    // D3D12_OPTIONS8.UnalignedBlockTexturesSupported FALSE: 37x19 BC1 is
    // refused; with it (WARP) it is accepted.
    EXPECT_NE(resource_desc_violation(bc1, layout), nullptr);
    caps.unaligned_block_textures = true;
    layout = layout_caps(caps);
    EXPECT_EQ(resource_desc_violation(bc1, layout), nullptr);
    // No sampler feedback on this device: a feedback-format texture is not a
    // resource it can describe.
    ResourceDesc feedback = bc1;
    feedback.width = 64;
    feedback.height = 32;
    feedback.format = 189;
    feedback.flags = RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    feedback.mip_region_width = feedback.mip_region_height = 4;
    feedback.mip_region_depth = 1;
    EXPECT_NE(resource_desc_violation(feedback, layout), nullptr);
}

/// A backend with an Apple GPU's answers: 1/2/4x MSAA, unified memory,
/// 16 GiB buffers, no sparse textures.
class AppleLikeBackend final : public graphics::GraphicsBackendDevice {
public:
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override { return "Fake"; }
    [[nodiscard]] void* native_device() const noexcept override { return nullptr; }
    [[nodiscard]] void* native_command_queue() const noexcept override { return nullptr; }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc&, const void*) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D> create_texture_2d(
        const graphics::BackendTexture2DDesc&, const void*, unsigned) override {
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
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline> create_compute_pipeline(
        const graphics::BackendComputePipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass> create_render_pass_standalone(
        const graphics::BackendRenderPassDesc&) override {
        return nullptr;
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall&) override { return false; }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*, const graphics::BackendBuffer*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d(graphics::BackendTexture2D*,
                                       const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override { return false; }
    void flush() override {}
    [[nodiscard]] bool supports_texture_sample_count(u32 count) const override {
        return count == 1 || count == 2 || count == 4;
    }
    [[nodiscard]] u64 max_buffer_length() const override { return 1ull << 34; }
    [[nodiscard]] bool has_unified_memory() const override { return true; }
};

TEST_F(D3D12CapsTest, BackendContextCarriesTheHardwareFactsOnly) {
    AppleLikeBackend backend;
    const CapsContext caps = backend_caps_context(backend, true);
    EXPECT_TRUE(caps.formats.sample_count_4);
    EXPECT_FALSE(caps.formats.sample_count_8);
    EXPECT_TRUE(caps.unified_memory);
    EXPECT_TRUE(caps.cache_coherent_uma);
    // Nothing the frontend implements is claimed from the backend alone.
    EXPECT_EQ(caps.tiled_resources_tier, 0u);
    EXPECT_EQ(caps.resource_binding_tier, 1u);
    EXPECT_FALSE(caps.logic_ops);
    EXPECT_EQ(max_feature_level(caps), 0xB000u);
    u32 per_resource = 0, per_process = 0;
    gpu_virtual_address_bits(caps, per_resource, per_process);
    EXPECT_EQ(per_resource, 34u);  // floor(log2(maxBufferLength))
    EXPECT_EQ(per_process, 47u);
    gpu_virtual_address_bits(backend_caps_context(backend, false), per_resource, per_process);
    EXPECT_EQ(per_resource, 31u);  // WARP x86: 31 / 31
    EXPECT_EQ(per_process, 31u);
    // 8x is refused by the quality-level query on this backend.
    bool write = false;
    u32 levels = 7;
    EXPECT_EQ(multisample_quality_levels(28, 8, 0, caps, write, levels), S_OK);
    EXPECT_EQ(levels, 0u);
    EXPECT_EQ(multisample_quality_levels(28, 4, 0, caps, write, levels), S_OK);
    EXPECT_EQ(levels, 1u);
}

TEST_F(D3D12CapsTest, MarshalEntryPointReadsTheSignature) {
    static constexpr GuestAddr kStack = kBase + 0xC00;
    for (const bool is_64bit : {true, false}) {
        const CapsContext caps = device_caps(is_64bit);
        cpu::CpuState cpu;
        if (is_64bit) {
            cpu.init_long_mode();
        } else {
            cpu.init_protected_mode();
        }
        cpu.gpr.rsp = kStack;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        prep(7, is_64bit);
        ASSERT_TRUE(memory.write_u32(kData, 0x60).is_ok());
        const u64 args[] = {0x1234, 7, kData, 4};
        for (usize i = 0; i < 4; ++i) {
            if (is_64bit) {
                (i == 0 ? cpu.gpr.rcx : i == 1 ? cpu.gpr.rdx : i == 2 ? cpu.gpr.r8 : cpu.gpr.r9) = args[i];
            } else {
                ASSERT_TRUE(memory.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
        marshal_check_feature_support(m, memory, caps);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK) << is_64bit;
        EXPECT_EQ(memory.read_u32(kData).value(), 0x51u) << is_64bit;
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d12
