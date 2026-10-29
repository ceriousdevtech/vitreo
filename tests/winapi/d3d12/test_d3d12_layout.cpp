// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_layout.cpp
/// @brief GetCopyableFootprints(1), GetResourceAllocationInfo(1/2/3) and
/// GetResourceTiling replayed against Windows: every call of
/// tools/oracle_probe/d3d12_layout_probe{64,32}.expected (Windows, WARP12)
/// runs again through the guest path, with WARP's own format support (from
/// d3d12_caps_probe64.expected [format]) as the device facts, and must write
/// the bytes Windows wrote. The few Windows answers this implementation does
/// not reproduce are listed in kEnvironment with the reason.

#include "winapi/graphics/d3d12/d3d12_layout.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi::d3d12 {
namespace {

constexpr u64 kCD64 = 0xCDCDCDCDCDCDCDCDull;

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

/// key=value pairs of a probe line (values decimal or 0x hex; "mr=a,b,c"
/// splits into mr0..mr2).
std::map<std::string, u64> fields(const std::string& line) {
    std::map<std::string, u64> out;
    std::istringstream words(line);
    std::string word;
    while (words >> word) {
        const auto eq = word.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = word.substr(0, eq);
        const std::string value = word.substr(eq + 1);
        if (value.find(',') != std::string::npos) {
            std::istringstream parts(value);
            std::string part;
            int i = 0;
            while (std::getline(parts, part, ',')) {
                out[key + std::to_string(i++)] = std::strtoull(part.c_str(), nullptr, 0);
            }
            continue;
        }
        out[key] = std::strtoull(value.c_str(), nullptr, 0);
    }
    return out;
}

ResourceDesc desc_from(std::map<std::string, u64>& f) {
    ResourceDesc d;
    d.dimension = static_cast<u32>(f["dim"]);
    d.alignment = f["al"];
    d.width = f["w"];
    d.height = static_cast<u32>(f["h"]);
    d.depth_or_array_size = static_cast<u16>(f["da"]);
    d.mip_levels = static_cast<u16>(f["mips"]);
    d.format = static_cast<u32>(f["fmt"]);
    d.sample_count = static_cast<u32>(f["sc"]);
    d.sample_quality = static_cast<u32>(f["sq"]);
    d.layout = static_cast<u32>(f["lay"]);
    d.flags = static_cast<u32>(f["fl"]);
    d.mip_region_width = static_cast<u32>(f["mr0"]);
    d.mip_region_height = static_cast<u32>(f["mr1"]);
    d.mip_region_depth = static_cast<u32>(f["mr2"]);
    return d;
}

/// WARP's D3D12 FORMAT_SUPPORT Support1 and MULTISAMPLE_QUALITY_LEVELS
/// (flags NONE), d3d12_caps_probe64.expected [format].
struct WarpFormats {
    std::array<u32, 193> support1{};
    std::map<std::pair<u32, u32>, u32> levels;
};

const WarpFormats& warp_formats() {
    static const WarpFormats formats = [] {
        WarpFormats w;
        for (const std::string& line : read_lines("d3d12_caps_probe64.expected")) {
            unsigned f = 0, hr = 0, s1 = 0;
            if (std::sscanf(line.c_str(), "format %u support hr=0x%x s1=0x%x", &f, &hr, &s1) != 3) {
                continue;
            }
            if (f < w.support1.size()) w.support1[f] = hr == 0 ? s1 : 0;
            const auto msaa = line.find(" msaa ");
            std::istringstream rest(line.substr(msaa + 6));
            std::string item;
            while (rest >> item) {
                unsigned count = 0, flags = 0, qhr = 0, levels = 0;
                if (std::sscanf(item.c_str(), "%u/%u:%x:%x", &count, &flags, &qhr, &levels) == 4 &&
                    flags == 0 && qhr == 0) {
                    w.levels[{f, count}] = levels;
                }
            }
        }
        return w;
    }();
    return formats;
}

LayoutCaps warp_caps() {
    LayoutCaps caps;
    caps.format_support1 = [](u32 f) { return f < 193 ? warp_formats().support1[f] : 0u; };
    caps.quality_levels = [](u32 f, u32 count) {
        const auto it = warp_formats().levels.find({f, count});
        return it == warp_formats().levels.end() ? 0u : it->second;
    };
    caps.max_buffer_width = 0xFFFFFFFCull;  // WARP x64 (d3d12_layout_probe [alloc] buf23/buf24)
    caps.node_mask = 1;
    caps.relaxed_format_casting = true;  // WARP D3D12_OPTIONS12
    caps.unaligned_block_textures = true;  // WARP D3D12_OPTIONS8
    return caps;
}

/// Windows answers this implementation deliberately does not give.
struct Environment {
    const char* tag;
    const char* reason;
};
const Environment kEnvironment[] = {
    // D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT (0x400) is newer than the 26100
    // SDK; the runtime accepts it, this device has no tight alignment and
    // refuses it as an undefined flag.
    {"fl400", "flag newer than SDK 26100"},
};

bool is_environment(const std::string& tag) {
    for (const auto& e : kEnvironment) {
        if (tag == e.tag) return true;
    }
    return false;
}

class D3D12LayoutTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBase = 0x20000000;
    static constexpr GuestAddr kDesc = kBase;
    static constexpr GuestAddr kLayouts = kBase + 0x1000;
    static constexpr GuestAddr kRows = kBase + 0x4000;
    static constexpr GuestAddr kRowSizes = kBase + 0x5000;
    static constexpr GuestAddr kTotal = kBase + 0x7000;
    static constexpr GuestAddr kRet = kBase + 0x7100;
    static constexpr GuestAddr kInfo1 = kBase + 0x7200;
    static constexpr GuestAddr kDescs = kBase + 0x8000;
    static constexpr GuestAddr kCounts = kBase + 0x9000;
    static constexpr GuestAddr kLists = kBase + 0x9100;
    static constexpr GuestAddr kFormats = kBase + 0x9200;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
    }

    void fill_cd() {
        std::vector<u8> cd(0x10000, 0xCD);
        ASSERT_TRUE(memory.write_bytes(kBase, cd.data(), cd.size()).is_ok());
    }

    u64 r64(GuestAddr a) { return memory.read_u64(a).value(); }
    u32 r32(GuestAddr a) { return memory.read_u32(a).value(); }

    void replay_footprints(const char* file, unsigned& checked, unsigned& environment);
    void replay_allocation(const char* file, bool is_64bit, unsigned& checked);
    void replay_tiling(const char* file, unsigned& checked);

    GuestMemory memory;
};

void D3D12LayoutTest::replay_footprints(const char* file, unsigned& checked,
                                        unsigned& environment) {
    const LayoutCaps caps = warp_caps();
    const std::vector<std::string> lines = read_lines(file);
    ASSERT_FALSE(lines.empty()) << file;
    for (usize i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const bool v1 = line.rfind("fp1 ", 0) == 0;
        if (line.rfind("fp ", 0) != 0 && !v1) continue;
        if (line.find("skipped") != std::string::npos) continue;
        std::string tag;
        {
            std::istringstream s(line);
            s >> tag >> tag;
        }
        auto f = fields(line);
        const ResourceDesc desc = desc_from(f);
        const u32 first = static_cast<u32>(f["first"]);
        const u32 num = static_cast<u32>(f["num"]);
        const u64 base = f["base"];
        const unsigned mask = static_cast<unsigned>(f["mask"]);
        const u64 total = f["total"];

        fill_cd();
        u8 bytes[RESOURCE_DESC1_SIZE];
        encode_resource_desc(desc, v1, bytes);
        ASSERT_TRUE(memory.write_bytes(kDesc, bytes, v1 ? RESOURCE_DESC1_SIZE : RESOURCE_DESC_SIZE)
                        .is_ok());
        guest_get_copyable_footprints(memory, caps, v1, kDesc, first, num, base,
                                      (mask & 1) ? kLayouts : 0, (mask & 2) ? kRows : 0,
                                      (mask & 4) ? kRowSizes : 0, (mask & 8) ? kTotal : 0);
        std::ostringstream diff;
        const u64 got_total = r64(kTotal);
        if (got_total != total) diff << " total " << got_total << " != " << total;
        for (usize j = i + 1; j < lines.size() && lines[j].rfind("  [", 0) == 0; ++j) {
            unsigned index = 0;
            unsigned long long off = 0, rs = 0;
            unsigned fmt = 0, w = 0, h = 0, d = 0, pitch = 0, rows = 0;
            if (std::sscanf(lines[j].c_str(),
                            "  [%u] off=%llu fmt=%u w=%u h=%u d=%u pitch=%u rows=%u rs=%llu", &index,
                            &off, &fmt, &w, &h, &d, &pitch, &rows, &rs) != 9) {
                continue;
            }
            const GuestAddr l = kLayouts + index * PLACED_SUBRESOURCE_FOOTPRINT_SIZE;
            const u64 got[] = {r64(l),       r32(l + 8),  r32(l + 12),
                               r32(l + 16),  r32(l + 20), r32(l + 24),
                               r32(kRows + index * 4), r64(kRowSizes + index * 8)};
            const u64 want[] = {off, fmt, w, h, d, pitch, rows, rs};
            static const char* names[] = {"off", "fmt", "w", "h", "d", "pitch", "rows", "rs"};
            for (int k = 0; k < 8; ++k) {
                if (got[k] != want[k]) {
                    diff << " [" << index << "]." << names[k] << " " << got[k] << " != " << want[k];
                }
            }
        }
        if (diff.str().empty()) {
            ++checked;
        } else if (is_environment(tag)) {
            ++environment;
        } else {
            ADD_FAILURE() << file << ":" << (i + 1) << " " << line << "\n " << diff.str()
                          << "\n violation: "
                          << (resource_desc_violation(desc, caps) ? resource_desc_violation(desc, caps)
                                                                  : "none");
        }
    }
}

TEST_F(D3D12LayoutTest, CopyableFootprintsMatchWindowsX64) {
    unsigned checked = 0, environment = 0;
    replay_footprints("d3d12_layout_probe64.expected", checked, environment);
    EXPECT_GT(checked, 1000u);
    EXPECT_LE(environment, std::size(kEnvironment));
}

TEST_F(D3D12LayoutTest, CopyableFootprintsMatchWindowsX86) {
    unsigned checked = 0, environment = 0;
    replay_footprints("d3d12_layout_probe32.expected", checked, environment);
    EXPECT_GT(checked, 1000u);
    EXPECT_LE(environment, std::size(kEnvironment));
}

TEST_F(D3D12LayoutTest, FormatInfoPlaneCountsMatchWindows) {
    unsigned checked = 0;
    for (const std::string& line : read_lines("d3d12_layout_probe64.expected")) {
        unsigned f = 0, hr = 0, planes = 0;
        if (std::sscanf(line.c_str(), "fi fmt=%u hr=0x%x planes=%u", &f, &hr, &planes) != 3) continue;
        EXPECT_EQ(format_plane_count(f), hr == 0 ? planes : 0u) << line;
        ++checked;
    }
    EXPECT_EQ(checked, 192u);
}

/// The castable-format lists of d3d12_layout_probe.c's "c_*" cases, which the
/// probe does not print.
std::vector<std::vector<u32>> castable_lists(const std::string& tag, bool& counts_null) {
    counts_null = false;
    if (tag == "c_srgb" || tag == "c_typeless") return {{29}};
    if (tag == "c_r32u") return {{42}};
    if (tag == "c_bc1") return {{71}};
    if (tag == "c_three") return {{34, 41, 31}};
    if (tag == "c_unk") return {{0}};
    if (tag == "c_zero") return {{}};
    if (tag == "c_two") return {{29}, {}};
    if (tag == "c_count_nullptr") return {{0xFFFFFFFFu}};  // count 1, NULL list
    counts_null = true;
    return {};
}

void D3D12LayoutTest::replay_allocation(const char* file, bool is_64bit, unsigned& checked) {
    const LayoutCaps caps = warp_caps();
    const std::vector<std::string> lines = read_lines(file);
    ASSERT_FALSE(lines.empty());
    for (usize i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.rfind("ai ", 0) != 0 || line.find("skipped") != std::string::npos) continue;
        std::string tag;
        {
            std::istringstream s(line);
            s >> tag >> tag;
        }
        auto f = fields(line);
        const u32 api = static_cast<u32>(f["api"]);
        const u32 visible = static_cast<u32>(f["mask"]);
        const u32 n = static_cast<u32>(f["n"]);
        const u64 size = f["size"];
        const u64 align = f["align"];
        std::vector<ResourceDesc> descs;
        std::vector<std::map<std::string, u64>> info1;
        usize j = i + 1;
        for (; j < lines.size() && lines[j].rfind("  d", 0) == 0; ++j) {
            auto df = fields(lines[j]);
            descs.push_back(desc_from(df));
        }
        for (; j < lines.size() && lines[j].rfind("  [", 0) == 0; ++j) info1.push_back(fields(lines[j]));
        ASSERT_EQ(descs.size(), std::min<u32>(n, 8)) << line;

        fill_cd();
        const bool v1 = api >= 2;
        const usize stride = v1 ? RESOURCE_DESC1_SIZE : RESOURCE_DESC_SIZE;
        for (usize k = 0; k < descs.size(); ++k) {
            u8 bytes[RESOURCE_DESC1_SIZE];
            encode_resource_desc(descs[k], v1, bytes);
            ASSERT_TRUE(memory.write_bytes(kDescs + k * stride, bytes, stride).is_ok());
        }
        bool counts_null = true;
        const auto lists = castable_lists(tag, counts_null);
        const usize ptr = is_64bit ? 8 : 4;
        GuestAddr format_cursor = kFormats;
        for (usize k = 0; k < lists.size(); ++k) {
            const bool null_list = lists[k].size() == 1 && lists[k][0] == 0xFFFFFFFFu;
            ASSERT_TRUE(memory.write_u32(kCounts + k * 4, null_list ? 1u : static_cast<u32>(lists[k].size()))
                            .is_ok());
            const GuestAddr list = null_list || lists[k].empty() ? 0 : format_cursor;
            if (is_64bit) {
                ASSERT_TRUE(memory.write_u64(kLists + k * ptr, list).is_ok());
            } else {
                ASSERT_TRUE(memory.write_u32(kLists + k * ptr, static_cast<u32>(list)).is_ok());
            }
            if (list != 0) {
                for (u32 fmt : lists[k]) {
                    ASSERT_TRUE(memory.write_u32(format_cursor, fmt).is_ok());
                    format_cursor += 4;
                }
            }
        }
        const bool with_info1 = !info1.empty();
        guest_get_resource_allocation_info(memory, caps, is_64bit, api, kRet, visible, n,
                                           n != 0 ? kDescs : 0, counts_null ? 0 : kCounts,
                                           counts_null ? 0 : kLists, with_info1 ? kInfo1 : 0);
        const u64 got_size = r64(kRet);
        const u64 got_align = r64(kRet + 8);
        const bool windows_valid = size != ~0ull;
        const bool all_buffers = [&] {
            for (const auto& d : descs) {
                if (d.dimension != RESOURCE_DIMENSION_BUFFER) return false;
            }
            return true;
        }();
        // Texture SizeInBytes is the adapter's (Learn): compared only where
        // the spec estimate is the whole story (every texture fits one
        // alignment unit, which the multi-resource cases are built from).
        const bool sizes_comparable = all_buffers || tag == "aam" || tag == "ama" || tag == "maa" ||
                                      tag == "ssas" || tag == "sbs" || tag == "mask" ||
                                      tag == "noinfo1" || tag.rfind("t1x1", 0) == 0 ||
                                      tag.rfind("t32x32", 0) == 0 || tag.rfind("t64x64", 0) == 0;
        std::ostringstream diff;
        if ((got_size != ~0ull) != windows_valid) diff << " validity";
        if (got_align != align) diff << " align " << got_align << " != " << align;
        if (sizes_comparable && got_size != size) diff << " size " << got_size << " != " << size;
        for (usize k = 0; k < info1.size(); ++k) {
            const GuestAddr e = kInfo1 + k * RESOURCE_ALLOCATION_INFO1_SIZE;
            if (r64(e + 8) != info1[k]["align"]) diff << " [" << k << "].align";
            if (sizes_comparable && (r64(e) != info1[k]["off"] || r64(e + 16) != info1[k]["size"])) {
                diff << " [" << k << "].off/size";
            }
            if (!sizes_comparable && info1[k]["size"] == kCD64 && r64(e + 16) != kCD64) {
                diff << " [" << k << "] written";
            }
        }
        // WARP's x86 buffer sizing truncates Width + 15 to 32 bits (buf20..23
        // read 0 on x86, 4 GiB on x64): a driver artefact, not the contract.
        const bool x86_artefact = !is_64bit && all_buffers && size == 0;
        if (diff.str().empty()) {
            ++checked;
        } else if (!x86_artefact) {
            ADD_FAILURE() << file << ":" << (i + 1) << " " << line << "\n" << diff.str()
                          << " got size=" << got_size;
        }
    }
}

TEST_F(D3D12LayoutTest, AllocationInfoMatchesWindowsX64) {
    unsigned checked = 0;
    replay_allocation("d3d12_layout_probe64.expected", true, checked);
    EXPECT_GT(checked, 400u);
}

TEST_F(D3D12LayoutTest, AllocationInfoMatchesWindowsX86) {
    unsigned checked = 0;
    replay_allocation("d3d12_layout_probe32.expected", false, checked);
    EXPECT_GT(checked, 400u);
}

void D3D12LayoutTest::replay_tiling(const char* file, unsigned& checked) {
    const std::vector<std::string> lines = read_lines(file);
    ASSERT_FALSE(lines.empty());
    ResourceDesc desc;
    bool reserved = false;
    bool have = false;
    for (usize i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.rfind("rt ", 0) == 0) {
            auto f = fields(line);
            desc = desc_from(f);
            reserved = line.find(" reserved ") != std::string::npos;
            have = f["hr"] == 0;
            continue;
        }
        if (line.rfind("  rt ", 0) != 0 || !have) continue;
        auto f = fields(line);
        const u32 in = static_cast<u32>(f["in"]);
        const u32 first = static_cast<u32>(f["first"]);
        const unsigned nullmask = static_cast<unsigned>(f["null"]);
        TiledLayout layout;
        if (reserved) ASSERT_TRUE(standard_tiled_layout(desc, layout)) << line;
        fill_cd();
        ASSERT_TRUE(memory.write_u32(kRows, in).is_ok());
        guest_get_resource_tiling(memory, desc, reserved, layout, (nullmask & 1) ? 0 : kTotal,
                                  (nullmask & 2) ? 0 : kRet, (nullmask & 4) ? 0 : kInfo1,
                                  (nullmask & 8) ? 0 : kRows, first, (nullmask & 16) ? 0 : kLayouts);
        std::ostringstream diff;
        const u8 packed0 = memory.read_u8(kRet).value();
        const u8 packed1 = memory.read_u8(kRet + 1).value();
        const u64 got[] = {r32(kTotal),     packed0,           packed1,
                           r32(kRet + 4),   r32(kRet + 8),     r32(kInfo1),
                           r32(kInfo1 + 4), r32(kInfo1 + 8),   r32(kRows)};
        const u64 want[] = {f["tiles"],   f["packed0"], f["packed1"], f["packed2"], f["packed3"],
                            f["shape0"],  f["shape1"],  f["shape2"],  f["out"]};
        for (int k = 0; k < 9; ++k) {
            if (got[k] != want[k]) diff << " field" << k << " " << got[k] << " != " << want[k];
        }
        for (usize j = i + 1; j < lines.size() && lines[j].rfind("    [", 0) == 0; ++j) {
            unsigned index = 0, w = 0, h = 0, d = 0, start = 0;
            if (std::sscanf(lines[j].c_str(), "    [%u] w=%u h=%u d=%u start=%u", &index, &w, &h, &d,
                            &start) != 5) {
                continue;
            }
            const GuestAddr e = kLayouts + index * SUBRESOURCE_TILING_SIZE;
            if (r32(e) != w || memory.read_u16(e + 4).value() != h ||
                memory.read_u16(e + 6).value() != d || r32(e + 8) != start) {
                diff << " [" << index << "]";
            }
        }
        if (diff.str().empty()) {
            ++checked;
        } else {
            ADD_FAILURE() << file << ":" << (i + 1) << " " << line << "\n" << diff.str();
        }
    }
}

TEST_F(D3D12LayoutTest, ResourceTilingMatchesWindows) {
    unsigned checked = 0;
    replay_tiling("d3d12_layout_probe64.expected", checked);
    replay_tiling("d3d12_layout_probe32.expected", checked);
    EXPECT_GT(checked, 40u);
}

TEST_F(D3D12LayoutTest, ResourceDescRoundTripsThroughGuestBytes) {
    // D3D12_RESOURCE_DESC offsets (SDK 26100 d3d12.h, identical on x86 and
    // x64): Alignment 8, Width 16, Height 24, DepthOrArraySize 28, MipLevels
    // 30, Format 32, SampleDesc 36, Layout 44, Flags 48; DESC1 adds
    // SamplerFeedbackMipRegion at 52.
    ResourceDesc d;
    d.dimension = 3;
    d.alignment = 0x10000;
    d.width = 0x123456789ull;
    d.height = 77;
    d.depth_or_array_size = 6;
    d.mip_levels = 9;
    d.format = 28;
    d.sample_count = 4;
    d.sample_quality = 0xFFFFFFFFu;
    d.layout = 2;
    d.flags = 0x21;
    d.mip_region_width = 4;
    d.mip_region_height = 8;
    d.mip_region_depth = 1;
    u8 b[RESOURCE_DESC1_SIZE];
    encode_resource_desc(d, true, b);
    u64 width = 0;
    std::memcpy(&width, b + 16, 8);
    EXPECT_EQ(width, 0x123456789ull);
    u16 mips = 0;
    std::memcpy(&mips, b + 30, 2);
    EXPECT_EQ(mips, 9);
    u32 region_depth = 0;
    std::memcpy(&region_depth, b + 60, 4);
    EXPECT_EQ(region_depth, 1u);
    const ResourceDesc back = decode_resource_desc(b, true);
    EXPECT_EQ(back.width, d.width);
    EXPECT_EQ(back.flags, d.flags);
    EXPECT_EQ(back.mip_region_height, 8u);
    const ResourceDesc plain = decode_resource_desc(b, false);
    EXPECT_EQ(plain.mip_region_width, 0u);
}

TEST_F(D3D12LayoutTest, LearnRowSizeExample) {
    // Learn GetCopyableFootprints: a 32-texel-wide 4-byte texture has a
    // 128-byte row and a 256-byte (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) pitch.
    ResourceDesc d;
    d.dimension = RESOURCE_DIMENSION_TEXTURE2D;
    d.width = 32;
    d.height = 8;
    d.depth_or_array_size = 1;
    d.mip_levels = 1;
    d.format = 28;
    d.sample_count = 1;
    Footprint got;
    u64 total = 0;
    ASSERT_TRUE(copyable_footprints(d, 0, 1, 0, warp_caps(),
                                    [&got](u32, const Footprint& f) { got = f; }, total));
    EXPECT_EQ(got.row_size, 128u);
    EXPECT_EQ(got.row_pitch, 256u);
    EXPECT_EQ(total, 7u * 256u + 128u);
}

TEST_F(D3D12LayoutTest, TextureAllocationTakesTheBackendRequirement) {
    LayoutCaps caps = warp_caps();
    ResourceDesc d;
    d.dimension = RESOURCE_DIMENSION_TEXTURE2D;
    d.width = 64;
    d.height = 64;
    d.depth_or_array_size = 1;
    d.mip_levels = 1;
    d.format = 28;
    d.sample_count = 1;
    caps.texture_heap_requirement = [](const ResourceDesc&, u32) {
        return TextureHeapRequirement{true, 70000, 16384};
    };
    std::vector<AllocationInfo1> per;
    AllocationInfo info = resource_allocation_info(0, {d}, {}, caps, &per);
    EXPECT_EQ(info.alignment, DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    EXPECT_EQ(info.size, 131072u);  // 70000 rounded up to the 64 KiB alignment
    // A 4 KiB request the backend cannot honour is the "not small" answer
    // (Learn, small resources): UINT64_MAX with 64 KiB, which apps retry from.
    d.alignment = SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    info = resource_allocation_info(0, {d}, {}, caps, &per);
    EXPECT_EQ(info.size, ~0ull);
    EXPECT_EQ(info.alignment, DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    caps.texture_heap_requirement = [](const ResourceDesc&, u32) {
        return TextureHeapRequirement{true, 3000, 4096};
    };
    info = resource_allocation_info(0, {d}, {}, caps, &per);
    EXPECT_EQ(info.size, 4096u);
    EXPECT_EQ(info.alignment, SMALL_RESOURCE_PLACEMENT_ALIGNMENT);
    caps.texture_heap_requirement = [](const ResourceDesc&, u32) {
        return TextureHeapRequirement{false, 0, 0};
    };
    info = resource_allocation_info(0, {d}, {}, caps, &per);
    EXPECT_EQ(info.size, ~0ull);
}

/// The vtable entry points read the SDK signature on each bitness: x64
/// fastcall, x86 stdcall with the UINT64 BaseOffset in two slots.
TEST_F(D3D12LayoutTest, MarshalEntryPointsReadBothBitnesses) {
    static constexpr GuestAddr kStack = kBase + 0xE000;
    const LayoutCaps caps = warp_caps();
    ResourceDesc d;
    d.dimension = RESOURCE_DIMENSION_TEXTURE2D;
    d.width = 64;
    d.height = 32;
    d.depth_or_array_size = 1;
    d.mip_levels = 1;
    d.format = 28;
    d.sample_count = 1;
    u8 bytes[RESOURCE_DESC_SIZE];
    encode_resource_desc(d, false, bytes);
    constexpr u64 kBaseOffset = 0x100000200ull;
    for (const bool is_64bit : {true, false}) {
        fill_cd();
        ASSERT_TRUE(memory.write_bytes(kDesc, bytes, sizeof(bytes)).is_ok());
        cpu::CpuState cpu;
        if (is_64bit) {
            cpu.init_long_mode();
        } else {
            cpu.init_protected_mode();
        }
        cpu.gpr.rsp = kStack;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        // GetCopyableFootprints(this, pDesc, First, Num, BaseOffset, pLayouts,
        // pNumRows, pRowSizeInBytes, pTotalBytes).
        std::vector<u64> args = {0x1234, kDesc, 0, 1};
        if (is_64bit) {
            args.push_back(kBaseOffset);
        } else {
            args.push_back(kBaseOffset & 0xFFFFFFFFull);
            args.push_back(kBaseOffset >> 32);
        }
        args.insert(args.end(), {kLayouts, kRows, kRowSizes, kTotal});
        for (usize i = 0; i < args.size(); ++i) {
            if (is_64bit) {
                switch (i) {
                case 0: cpu.gpr.rcx = args[i]; break;
                case 1: cpu.gpr.rdx = args[i]; break;
                case 2: cpu.gpr.r8 = args[i]; break;
                case 3: cpu.gpr.r9 = args[i]; break;
                default:
                    ASSERT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]).is_ok());
                }
            } else {
                ASSERT_TRUE(memory.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
        marshal_get_copyable_footprints(m, memory, caps, is_64bit, false);
        EXPECT_EQ(r64(kLayouts), kBaseOffset) << is_64bit;
        EXPECT_EQ(r32(kLayouts + 24), 256u) << is_64bit;
        EXPECT_EQ(r32(kRows), 32u) << is_64bit;
        EXPECT_EQ(r64(kTotal), 31u * 256u + 256u) << is_64bit;

        // GetResourceAllocationInfo1(this, RetVal, visibleMask, n, pDescs,
        // pInfo1) returns RetVal.
        ASSERT_TRUE(memory.write_bytes(kDescs, bytes, sizeof(bytes)).is_ok());
        const u64 alloc_args[] = {0x1234, kRet, 0, 1, kDescs, kInfo1};
        for (usize i = 0; i < 6; ++i) {
            if (is_64bit) {
                switch (i) {
                case 0: cpu.gpr.rcx = alloc_args[i]; break;
                case 1: cpu.gpr.rdx = alloc_args[i]; break;
                case 2: cpu.gpr.r8 = alloc_args[i]; break;
                case 3: cpu.gpr.r9 = alloc_args[i]; break;
                default:
                    ASSERT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8, alloc_args[i]).is_ok());
                }
            } else {
                ASSERT_TRUE(
                    memory.write_u32(kStack + 4 + i * 4, static_cast<u32>(alloc_args[i])).is_ok());
            }
        }
        marshal_get_resource_allocation_info(m, memory, caps, is_64bit, 1);
        EXPECT_EQ(cpu.gpr.rax & (is_64bit ? ~0ull : 0xFFFFFFFFull), kRet) << is_64bit;
        EXPECT_EQ(r64(kRet), 65536u) << is_64bit;
        EXPECT_EQ(r64(kRet + 8), 65536u) << is_64bit;
        EXPECT_EQ(r64(kInfo1 + 16), 65536u) << is_64bit;
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d12
